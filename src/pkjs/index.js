var Clay = require('@rebble/clay');
var config = require('./config');
var configPage = require('./config-page');
var keys = require('message_keys');

// Settings are sent manually so a save that collides with a data update is retried.
var clay = new Clay(config.build(), configPage, { autoHandleEvents: false });

var API_BASE = 'https://api-v3.mbta.com';
// Optional: a free key from https://api-v3.mbta.com/ raises the rate limit.
var MBTA_API_KEY = '';
// Must match NUM_TIMES in src/c/watchface.c.
var MAX_TIMES = 7;
// Look back this far when querying schedules so delayed trains are still found.
var LOOKBACK_MS = 60 * 60 * 1000;

// Must match NUM_ROWS in src/c/watchface.c.
var NUM_ROWS = config.ROWS.length;
// Row display modes; must match RowMode in src/c/watchface.c.
var MODE_CLOCK = 0;
var MODE_MINUTES = 1;
// Per-departure status letters; must match FLAG_* in src/c/watchface.c.
var FLAG_NONE = ' ';
var FLAG_CANCELLED = 'X';
var FLAG_DELAYED = 'D';
// A commuter rail train predicted this much after its scheduled time is shown as delayed.
var DELAY_SEC = 3 * 60;

// A fetch still unfinished after this long is treated as hung and abandoned.
// The iOS Pebble app has no XMLHttpRequest timeout and suspends timers in the
// background, so this is checked against the clock on the next request.
var FETCH_HUNG_MS = 30 * 1000;

var s_fetching = false;
var s_refetch = false;
var s_fetch_started = 0;
var s_fetch_id = 0;

function pad2(n) {
  return (n < 10 ? '0' : '') + n;
}

// Parses "2026-10-16T16:05:00-04:00" into epoch seconds.
function parseIso(s) {
  if (!s) return 0;
  var m = /^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})(?:\.\d+)?(Z|([+-])(\d{2}):(\d{2}))$/.exec(s);
  if (!m) return 0;
  var ms = Date.UTC(+m[1], +m[2] - 1, +m[3], +m[4], +m[5], +m[6]);
  if (m[7] !== 'Z') {
    var offset = (+m[9] * 60 + +m[10]) * 60000;
    ms -= (m[8] === '+' ? 1 : -1) * offset;
  }
  return Math.floor(ms / 1000);
}

function buildQuery(params) {
  var parts = [];
  for (var k in params) {
    if (params.hasOwnProperty(k)) {
      parts.push(encodeURIComponent(k) + '=' + encodeURIComponent(params[k]));
    }
  }
  return parts.join('&');
}

function apiGet(path, params, callback) {
  var xhr = new XMLHttpRequest();
  xhr.open('GET', API_BASE + path + '?' + buildQuery(params), true);
  xhr.setRequestHeader('Accept', 'application/vnd.api+json');
  if (MBTA_API_KEY) {
    xhr.setRequestHeader('x-api-key', MBTA_API_KEY);
  }
  xhr.onload = function() {
    if (xhr.status >= 200 && xhr.status < 300) {
      try {
        callback(null, JSON.parse(xhr.responseText));
      } catch (e) {
        callback(e);
      }
    } else {
      callback(new Error('HTTP ' + xhr.status));
    }
  };
  xhr.onerror = function() {
    callback(new Error('Network error'));
  };
  xhr.timeout = 20000;
  xhr.ontimeout = xhr.onerror;
  xhr.send();
}

// UTC ms of 2am local on the nth Sunday of a month, given that zone's UTC offset.
function nthSunday2am(year, month, n, offsetHours) {
  var firstDow = new Date(Date.UTC(year, month, 1)).getUTCDay();
  var day = 1 + (7 - firstDow) % 7 + (n - 1) * 7;
  return Date.UTC(year, month, day, 2 - offsetHours);
}

// Boston's UTC offset in hours (-4 during daylight time, otherwise -5). Worked
// out by hand because the phone may be in another timezone and iOS PebbleKit JS
// is not guaranteed to have Intl timezone support.
function bostonOffsetHours(ms) {
  var year = new Date(ms).getUTCFullYear();
  var dstStart = nthSunday2am(year, 2, 2, -5);
  var dstEnd = nthSunday2am(year, 10, 1, -4);
  return ms >= dstStart && ms < dstEnd ? -4 : -5;
}

// The schedule queries covering departures from LOOKBACK_MS ago onwards, in
// Boston time. MBTA service days run past midnight (e.g. "25:10"), so before
// 3am that is yesterday's date with hours >= 24, plus today's service day so
// the first morning trains show once the last night train has gone.
function serviceWindows(nowMs) {
  var startMs = nowMs - LOOKBACK_MS;
  // Shifted so the UTC getters read Boston wall-clock time.
  var start = new Date(startMs + bostonOffsetHours(startMs) * 3600000);
  function ymd(d) {
    return d.getUTCFullYear() + '-' + pad2(d.getUTCMonth() + 1) + '-' + pad2(d.getUTCDate());
  }
  var hours = start.getUTCHours();
  var mins = pad2(start.getUTCMinutes());
  if (hours >= 3) {
    return [{ date: ymd(start), minTime: pad2(hours) + ':' + mins }];
  }
  var yesterday = new Date(start.getTime() - 24 * 60 * 60 * 1000);
  return [
    { date: ymd(yesterday), minTime: pad2(hours + 24) + ':' + mins },
    { date: ymd(start), minTime: '03:00' }
  ];
}

function indexIncluded(json) {
  var map = {};
  (json.included || []).forEach(function(item) {
    map[item.type + ':' + item.id] = item;
  });
  return map;
}

function isCancelled(attrs) {
  return attrs && (attrs.schedule_relationship === 'CANCELLED' ||
                   attrs.schedule_relationship === 'SKIPPED');
}

function savedSettings() {
  try {
    return JSON.parse(localStorage.getItem('clay-settings')) || {};
  } catch (e) {
    return {};
  }
}

// The line, station and direction chosen for a row, falling back to the defaults.
function rowConfig(row, saved) {
  var defaults = config.ROWS[row];
  function pick(key, fallback) {
    var v = saved[key + '[' + row + ']'];
    return v === undefined || v === null || v === '' ? fallback : String(v);
  }
  return {
    route: pick('ROUTE', defaults.route.value),
    stop: pick('STOP', defaults.stop.value),
    direction: pick('DIR', defaults.direction.value)
  };
}

function isCommuterRail(route) {
  return route.indexOf('CR-') === 0;
}

// Commuter rail: scheduled departures, overridden by live predictions when
// available, with cancelled and delayed trains flagged.
function fetchCommuterRail(row, callback) {
  var nowMs = Date.now();
  var nowSec = Math.floor(nowMs / 1000);
  var windows = serviceWindows(nowMs);
  var trains = [];
  var pending = windows.length;
  var failed = null;
  windows.forEach(function(win) {
    apiGet('/schedules', {
      'filter[stop]': row.stop,
      'filter[route]': row.route,
      'filter[direction_id]': row.direction,
      'filter[date]': win.date,
      'filter[min_time]': win.minTime,
      'include': 'prediction',
      'sort': 'departure_time',
      'page[limit]': 20,
      'fields[schedule]': 'departure_time,arrival_time',
      'fields[prediction]': 'departure_time,arrival_time,schedule_relationship'
    }, function(err, json) {
      if (err) {
        failed = err;
      } else {
        var included = indexIncluded(json);
        json.data.forEach(function(sched) {
          var a = sched.attributes;
          var rel = sched.relationships && sched.relationships.prediction;
          var pred = rel && rel.data ? included['prediction:' + rel.data.id] : null;
          var p = pred ? pred.attributes : null;
          var scheduled = parseIso(a.departure_time || a.arrival_time);
          var predicted = p ? parseIso(p.departure_time || p.arrival_time) : 0;
          // A cancelled train stays on the timetable at its scheduled time, crossed out.
          var cancelled = isCancelled(p);
          var t = cancelled ? scheduled : (predicted || scheduled);
          var flag = cancelled ? FLAG_CANCELLED
                   : predicted && predicted - scheduled >= DELAY_SEC ? FLAG_DELAYED : FLAG_NONE;
          if (t > nowSec) trains.push({ time: t, dest: ' ', flag: flag });
        });
      }
      if (--pending > 0) return;
      if (failed) return callback(failed);
      trains.sort(function(x, y) { return x.time - y.time; });
      callback(null, { mode: MODE_CLOCK, trains: trains.slice(0, MAX_TIMES) });
    });
  });
}

// Subway and light rail: live predictions. When trains in this direction go to
// different places (e.g. Ashmont vs Braintree), each is tagged with the first
// letter of its destination.
function fetchSubway(row, callback) {
  apiGet('/predictions', {
    'filter[stop]': row.stop,
    'filter[route]': row.route,
    'filter[direction_id]': row.direction,
    'include': 'trip',
    'sort': 'departure_time',
    'page[limit]': 15,
    'fields[prediction]': 'departure_time,arrival_time,schedule_relationship',
    'fields[trip]': 'headsign'
  }, function(err, json) {
    if (err) return callback(err);
    var included = indexIncluded(json);
    var nowSec = Math.floor(Date.now() / 1000);
    var trains = [];
    var headsigns = {};
    json.data.forEach(function(pred) {
      var a = pred.attributes;
      if (isCancelled(a)) return;
      // Arrival-only predictions are trains terminating here.
      if (!a.departure_time) return;
      var t = parseIso(a.departure_time);
      if (t <= nowSec) return;
      var tripRel = pred.relationships && pred.relationships.trip;
      var trip = tripRel && tripRel.data ? included['trip:' + tripRel.data.id] : null;
      var headsign = trip && trip.attributes.headsign ? trip.attributes.headsign : '';
      headsigns[headsign] = true;
      trains.push({ time: t, dest: headsign ? headsign.charAt(0).toUpperCase() : ' ' });
    });
    trains.sort(function(x, y) { return x.time - y.time; });
    trains = trains.slice(0, MAX_TIMES);
    if (Object.keys(headsigns).length < 2) {
      trains.forEach(function(t) { t.dest = ' '; });
    }
    callback(null, { mode: MODE_MINUTES, trains: trains });
  });
}

function putRow(dict, row, result) {
  var dest = '';
  var flags = '';
  for (var i = 0; i < MAX_TIMES; i++) {
    var train = result.trains[i];
    dict[keys.TIMES + row * MAX_TIMES + i] = train ? train.time : 0;
    dest += train ? train.dest : ' ';
    flags += train && train.flag ? train.flag : FLAG_NONE;
  }
  dict[keys.DEST + row] = dest;
  dict[keys.FLAGS + row] = flags;
  dict[keys.MODE + row] = result.mode;
}

function sendToWatch(dict, retries, what) {
  what = what || 'transit data';
  Pebble.sendAppMessage(dict, function() {
    console.log('Sent ' + what + ' to watch');
  }, function(e) {
    console.log('Failed to send ' + what + ': ' + JSON.stringify(e));
    if (retries > 0) {
      setTimeout(function() { sendToWatch(dict, retries - 1, what); }, 2000);
    }
  });
}

function refresh() {
  if (s_fetching) {
    if (Date.now() - s_fetch_started < FETCH_HUNG_MS) {
      s_refetch = true;
      return;
    }
    console.log('Abandoning hung fetch');
  }
  s_fetching = true;
  s_refetch = false;
  s_fetch_started = Date.now();
  var fetchId = ++s_fetch_id;

  var saved = savedSettings();
  var dict = {};
  var pending = NUM_ROWS;
  var failed = 0;
  function done() {
    if (--pending > 0) return;
    s_fetching = false;
    // Rows that failed to load are omitted so the watch keeps its last data;
    // the count tells it to show the fetch as failed.
    dict[keys.FETCH_FAILED] = failed;
    sendToWatch(dict, 2);
    if (s_refetch) refresh();
  }

  for (var r = 0; r < NUM_ROWS; r++) {
    (function(row) {
      var cfg = rowConfig(row, saved);
      var fetch = isCommuterRail(cfg.route) ? fetchCommuterRail : fetchSubway;
      fetch(cfg, function(err, result) {
        // A late reply from an abandoned fetch must not disturb its replacement.
        if (fetchId !== s_fetch_id) return;
        if (err) {
          console.log('Row ' + (row + 1) + ' (' + cfg.route + ' ' + cfg.stop + ') fetch failed: ' +
                      err.message);
          failed++;
        } else {
          putRow(dict, row, result);
        }
        done();
      });
    })(r);
  }
}

Pebble.addEventListener('showConfiguration', function() {
  // Rebuilt each time so the page includes the saved stations as fallback options.
  clay = new Clay(config.build(savedSettings()), configPage, { autoHandleEvents: false });
  Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener('webviewclosed', function(e) {
  if (!e || !e.response) return;
  var settings = clay.getSettings(e.response);
  // The watch only needs the labels; the phone uses the stations to fetch.
  for (var row = 0; row < NUM_ROWS; row++) {
    delete settings[keys.ROUTE + row];
    delete settings[keys.STOP + row];
    delete settings[keys.DIR + row];
  }
  sendToWatch(settings, 3, 'settings');
  refresh();
});

Pebble.addEventListener('ready', function() {
  console.log('PebbleKit JS ready');
  refresh();
});

Pebble.addEventListener('appmessage', function() {
  refresh();
});

// Exposed for tests.
if (typeof module !== 'undefined') {
  module.exports = { serviceWindows: serviceWindows, bostonOffsetHours: bostonOffsetHours };
}
