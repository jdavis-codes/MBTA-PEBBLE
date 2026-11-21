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
// A station nobody has chosen yet.
var MODE_UNSET = 2;
// Minutes to departure, for a bus: the watch says "buses" where it would say "trains".
var MODE_BUS = 3;
// Per-departure status letters; must match FLAG_* in src/c/watchface.c.
var FLAG_NONE = ' ';
var FLAG_CANCELLED = 'X';
var FLAG_DELAYED = 'D';
// A commuter rail train predicted this much after its scheduled time is shown as delayed.
var DELAY_SEC = 3 * 60;
// Alerts below this MBTA severity (0-10) are notices such as parking or stairs.
var ALERT_MIN_SEVERITY = 3;
// Must match ALERT_SIZE - 1 in src/c/watchface.c.
var ALERT_MAX_CHARS = 160;

// Bluebikes dock status (GBFS). The feed has no per-station query, so the
// whole system's status is fetched each time.
// The same feed is served from two addresses. Bluebikes' own compresses the
// download (about 18 KB instead of 234 KB), so it is tried first; Lyft's is
// the published one and is the fallback.
var BIKE_STATUS_URLS = [
  'https://gbfs.bluebikes.com/gbfs/en/station_status.json',
  'https://gbfs.lyft.com/gbfs/1.1/bos/en/station_status.json'
];
// Roughly what gzip leaves of the dock feed, for downloads whose size on the
// wire the phone does not report.
var BIKE_GZIP_RATIO = 0.076;
// The same for MBTA replies, which are smaller and compress less well.
var TRAIN_GZIP_RATIO = 0.2;
// Must match NUM_BIKES and BIKE_LEVELS in src/c/watchface.c.
var NUM_BIKES = 9;
var BIKE_LEVELS = 8;
var BIKE_NONE = '-';
// Levels past BIKE_LEVELS; must match src/c/watchface.c.
var BIKE_ONE_FREE = BIKE_LEVELS + 1;
var BIKE_FULL = BIKE_LEVELS + 2;
// Must match BIKE_MAX_EBIKES in src/c/watchface.c.
var BIKE_MAX_EBIKES = 8;
// What the watch's REQUEST message asks for; must match src/c/watchface.c.
var REQUEST_BIKES = 2;

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

// Data usage. Every download is counted per day in localStorage, split into
// trains (MBTA) and bikes (Bluebikes): how many, how much data they held, and
// how much of that crossed the network where the phone reports it.
var USAGE_KEY = 'usage';
var USAGE_DAYS = 3;

function usageDay(offset) {
  var d = new Date(Date.now() - (offset || 0) * 86400000);
  return d.getFullYear() + '-' + pad2(d.getMonth() + 1) + '-' + pad2(d.getDate());
}

function loadUsage() {
  try {
    return JSON.parse(localStorage.getItem(USAGE_KEY)) || {};
  } catch (e) {
    return {};
  }
}

function header(xhr, name) {
  try {
    return xhr.getResponseHeader(name) || '';
  } catch (e) {
    return '';
  }
}

function noteUsage(kind, xhr) {
  var size = xhr.responseText ? xhr.responseText.length : 0;
  var length = parseInt(header(xhr, 'Content-Length'), 10);
  var etag = header(xhr, 'ETag');
  // A phone that unpacks a compressed reply drops its Content-Encoding and
  // Content-Length headers before the code sees them. Two things still give
  // it away: the dock feed marks its compressed copy with a weak ETag ("W/"),
  // and both servers always send a length with an uncompressed reply.
  var gzip = /gzip|br|deflate/i.test(header(xhr, 'Content-Encoding')) || /^W\//.test(etag) ||
             (!(length > 0) && header(xhr, 'Content-Type') !== '');
  var usage = loadUsage();
  var today = usageDay(0);
  var day = usage[today] || (usage[today] = {});
  // What the phone lets the code see of the last reply, to tell "not
  // compressed" apart from "the phone hides the reply's headers".
  var all = '';
  try {
    all = xhr.getAllResponseHeaders() || '';
  } catch (e) {
    all = 'error: ' + e.message;
  }
  day[kind + 'Seen'] = 'type=' + (header(xhr, 'Content-Type') || 'none') +
                       ', encoding=' + (header(xhr, 'Content-Encoding') || 'none') +
                       ', length=' + (header(xhr, 'Content-Length') || 'none') +
                       ', etag=' + (etag ? etag.substring(0, 6) : 'none') +
                       ', counted as ' + (gzip ? 'compressed' : 'not compressed') +
                       ', ' + all.length + ' characters of headers, ' + size + ' of data';
  var u = day[kind] || (day[kind] = { n: 0, data: 0, wire: 0, gz: 0, est: 0 });
  u.n++;
  u.data += size;
  if (gzip) u.gz++;
  if (length > 0) {
    // A reported length is the size on the wire, compressed or not.
    u.wire += length;
  } else if (gzip) {
    // Compressed, but the phone does not say by how much: estimate it.
    u.wire += Math.round(size * (kind === 'bikes' ? BIKE_GZIP_RATIO : TRAIN_GZIP_RATIO));
    u.est++;
  } else {
    u.wire += size;
  }
  // Keep only the last few days.
  Object.keys(usage).sort().slice(0, -USAGE_DAYS).forEach(function(k) { delete usage[k]; });
  try {
    localStorage.setItem(USAGE_KEY, JSON.stringify(usage));
  } catch (e) { /* storage full: usage is only a diagnostic */ }
}

function megabytes(bytes) {
  return bytes >= 1048576 ? (bytes / 1048576).toFixed(1) + ' MB' : Math.round(bytes / 1024) + ' KB';
}

// The settings page gets two lines: today's Bluebikes and MBTA totals. With
// detail, for the logs, yesterday is added, along with how many requests were
// compressed and what the phone exposed about the last reply.
function usageLines(detail) {
  var usage = loadUsage();
  var lines = [];
  var days = [[usageDay(0), 'Today']];
  if (detail) days.push([usageDay(1), 'Yesterday']);
  days.forEach(function(d) {
    var day = usage[d[0]] || {};
    [['bikes', 'Bluebikes'], ['trains', 'MBTA']].forEach(function(k) {
      var u = day[k[0]];
      if (!u) {
        if (!detail) lines.push(d[1] + ', ' + k[1] + ': nothing downloaded yet');
        return;
      }
      lines.push(d[1] + ', ' + k[1] + ': ' + (u.est ? 'about ' : '') + megabytes(u.wire) +
                 ' in ' + u.n + (u.n === 1 ? ' request' : ' requests') +
                 (detail && u.gz ? ' (' + u.gz + ' compressed, ' + megabytes(u.data) + ' uncompressed)' : ''));
      if (detail && day[k[0] + 'Seen']) lines.push('Last ' + k[1] + ' reply: ' + day[k[0] + 'Seen']);
    });
  });
  return lines;
}

// Location test. Each time the watch asks for fresh data the phone is also
// asked where it is, the way the open-source weather faces do it (TimeStyle,
// pebble-generic-weather: getCurrentPosition with a 15 s timeout and a fix up
// to a minute old). Nothing is done with the position yet: the point is to
// learn whether the phone answers while the Pebble app is in the background.
// The results are kept per day in localStorage and shown in the settings.
var LOCATION_KEY = 'locationTest';
var LOCATION_OPTIONS = { timeout: 15000, maximumAge: 60000, enableHighAccuracy: false };

function loadLocationTest() {
  try {
    var t = JSON.parse(localStorage.getItem(LOCATION_KEY)) || {};
    return t.day === usageDay(0) ? t : { day: usageDay(0), asked: 0, ok: 0, failed: 0, gap: 0 };
  } catch (e) {
    return { day: usageDay(0), asked: 0, ok: 0, failed: 0, gap: 0 };
  }
}

function saveLocationTest(t) {
  try {
    localStorage.setItem(LOCATION_KEY, JSON.stringify(t));
  } catch (e) { /* only a diagnostic */ }
}

// One request to the phone at a time. A request the phone never answers must
// not hold up the timetables or pile up behind itself, so it is given up on
// after LOCATION_HUNG_MS, by a timer and, because the iPhone app suspends
// timers in the background, by the clock when the next request comes in.
var LOCATION_HUNG_MS = 20 * 1000;
var s_locating = 0;       // when the request in flight was made, or 0
var s_locationId = 0;
var s_locationWaiters = [];

function finishLocation(id, err, pos) {
  if (id !== s_locationId || !s_locating) return;
  var started = s_locating;
  s_locating = 0;
  var r = loadLocationTest();
  var now = Date.now();
  if (pos) {
    r.ok++;
    // The longest wait between two answers shows up background stalls.
    if (r.lastOk && now - r.lastOk > r.gap) r.gap = now - r.lastOk;
    r.lastOk = now;
    r.took = now - started;
    r.accuracy = Math.round(pos.coords.accuracy || 0);
    r.age = pos.timestamp ? Math.max(0, Math.round((now - pos.timestamp) / 1000)) : -1;
    // Rounded to about a kilometre: enough to see it is right, and it never leaves the phone.
    r.where = pos.coords.latitude.toFixed(2) + ', ' + pos.coords.longitude.toFixed(2);
  } else {
    r.failed++;
    r.error = err + ' at ' + clockTime(now);
  }
  saveLocationTest(r);
  var waiters = s_locationWaiters;
  s_locationWaiters = [];
  waiters.forEach(function(w) {
    try { w(pos ? null : new Error(err), pos); } catch (e) { console.log('Location waiter: ' + e); }
  });
}

// Asks the phone where it is; callback(err, pos) is optional.
function askLocation(callback) {
  if (callback) s_locationWaiters.push(callback);
  if (s_locating) {
    if (Date.now() - s_locating < LOCATION_HUNG_MS) return;
    finishLocation(s_locationId, 'no answer from the phone');
    // finishLocation called the waiters, this one included.
    return;
  }
  var id = ++s_locationId;
  s_locating = Date.now();
  var t = loadLocationTest();
  t.asked++;
  saveLocationTest(t);
  try {
    var geo = typeof navigator !== 'undefined' && navigator.geolocation;
    if (!geo) return finishLocation(id, 'the phone offers no location service to the watchface');
    geo.getCurrentPosition(function(pos) {
      finishLocation(id, null, pos);
    }, function(err) {
      finishLocation(id, err && err.code === 1 ? 'permission denied'
                         : err && err.code === 3 ? 'timed out' : 'position unavailable');
    }, LOCATION_OPTIONS);
    setTimeout(function() { finishLocation(id, 'no answer from the phone'); }, LOCATION_HUNG_MS);
  } catch (e) {
    finishLocation(id, 'the location request could not be made');
  }
}

function testLocation() {
  askLocation(null);
}

function clockTime(ms) {
  var d = new Date(ms);
  return pad2(d.getHours()) + ':' + pad2(d.getMinutes()) + ':' + pad2(d.getSeconds());
}

function locationLines() {
  var t = loadLocationTest();
  if (!t.asked) return ['Not asked yet today.'];
  var lost = Math.max(0, t.asked - t.ok - t.failed - (s_locating ? 1 : 0));
  var lines = ['Asked ' + t.asked + (t.asked === 1 ? ' time' : ' times') + ' today: ' + t.ok +
               ' answered, ' + t.failed + ' failed' + (lost ? ', ' + lost + ' lost' : '') + '.'];
  if (t.lastOk) {
    lines.push('Last answer at ' + clockTime(t.lastOk) + ': within ' + t.accuracy + ' m, ' +
               (t.age < 0 ? 'age unknown' : t.age + ' s old') + ', took ' +
               (t.took / 1000).toFixed(1) + ' s. Near ' + t.where + '.');
    lines.push('Longest wait between answers: ' + Math.round(t.gap / 60000) + ' min.');
  }
  if (t.error) lines.push('Last failure: ' + t.error + '.');
  return lines;
}

function apiGet(path, params, callback) {
  var xhr = new XMLHttpRequest();
  xhr.open('GET', API_BASE + path + '?' + buildQuery(params), true);
  xhr.setRequestHeader('Accept', 'application/vnd.api+json');
  if (MBTA_API_KEY) {
    xhr.setRequestHeader('x-api-key', MBTA_API_KEY);
  }
  xhr.onload = function() {
    noteUsage('trains', xhr);
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

function getJson(url, callback) {
  var xhr = new XMLHttpRequest();
  xhr.open('GET', url, true);
  xhr.onload = function() {
    noteUsage('bikes', xhr);
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
function serviceWindows(nowMs, lookbackMs) {
  var startMs = nowMs - (lookbackMs === undefined ? LOOKBACK_MS : lookbackMs);
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

// Rewrites the saved settings with remove + set. The emulator's localStorage
// (Python dbm.dumb) can keep the old length when a shorter value overwrites a
// longer one, leaving junk after the JSON; a remove rewrites its index.
function storeSettings(json) {
  localStorage.removeItem('clay-settings');
  localStorage.setItem('clay-settings', json);
}

function savedSettings() {
  var raw = localStorage.getItem('clay-settings');
  if (!raw) return {};
  try {
    return JSON.parse(raw) || {};
  } catch (e) {
    // Recover the settings object from before any trailing junk.
    for (var end = raw.lastIndexOf('}'); end > 0; end = raw.lastIndexOf('}', end - 1)) {
      try {
        var json = raw.slice(0, end + 1);
        var settings = JSON.parse(json);
        storeSettings(json);
        return settings;
      } catch (e2) {}
    }
    return {};
  }
}

// The line, station and direction chosen for a row, falling back to the defaults.
// A direction of "0:<name>" is outbound limited to trains headed for that endpoint.
function rowConfig(row, saved) {
  var defaults = config.ROWS[row];
  function pick(key, fallback) {
    var v = saved[key + '[' + row + ']'];
    return v === undefined || v === null || v === '' ? fallback : String(v);
  }
  var alerts = saved['ALERT_ON[' + row + ']'];
  var dir = pick('DIR', defaults.direction.value);
  var colon = dir.indexOf(':');
  var route = pick('ROUTE', defaults.route.value);
  var stop = pick('STOP', defaults.stop.value);
  return {
    // False until a line, station and direction have all been chosen.
    set: route !== '' && stop !== '' && dir !== '',
    route: route,
    stop: stop,
    direction: colon < 0 ? dir : dir.substring(0, colon),
    destination: colon < 0 ? null : dir.substring(colon + 1),
    alerts: alerts === true || alerts === 1 || alerts === '1' || alerts === 'true'
  };
}

function isCommuterRail(route) {
  return route.indexOf('CR-') === 0;
}

// Subway and light rail routes are named; everything else offered is a bus,
// whose route ids are numbers such as "66" or "741".
function isBus(route) {
  return !isCommuterRail(route) && !/^(Red|Orange|Blue|Mattapan|Green-)/.test(route);
}

// Some trains stop at an endpoint without ending there, so they count toward it too.
// Wickford Junction trains stop at Providence; Foxboro shuttles are headed to Foxboro.
var SERVED_HEADSIGNS = {
  'Providence': ['Providence', 'Wickford Junction'],
  'Foxboro': ['Foxboro', 'Foxboro (Shuttle)']
};

// Commuter rail: scheduled departures, overridden by live predictions when
// available, with cancelled and delayed trains flagged.
function fetchCommuterRail(row, callback) {
  var nowMs = Date.now();
  var nowSec = Math.floor(nowMs / 1000);
  var windows = serviceWindows(nowMs);
  var served = row.destination ? (SERVED_HEADSIGNS[row.destination] || [row.destination]) : null;
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
      'include': 'prediction,trip',
      'sort': 'departure_time',
      // A single-endpoint filter drops some schedules, so ask for more to fill the row.
      'page[limit]': row.destination ? 40 : 20,
      'fields[schedule]': 'departure_time,arrival_time',
      'fields[prediction]': 'departure_time,arrival_time,schedule_relationship',
      'fields[trip]': 'headsign'
    }, function(err, json) {
      if (err) {
        failed = err;
      } else {
        var included = indexIncluded(json);
        json.data.forEach(function(sched) {
          var a = sched.attributes;
          var rel = sched.relationships || {};
          var trip = rel.trip && rel.trip.data ? included['trip:' + rel.trip.data.id] : null;
          if (served && (!trip || served.indexOf(trip.attributes.headsign) < 0)) return;
          var pred = rel.prediction && rel.prediction.data
            ? included['prediction:' + rel.prediction.data.id] : null;
          var p = pred ? pred.attributes : null;
          var scheduled = parseIso(a.departure_time || a.arrival_time);
          var predicted = p ? parseIso(p.departure_time || p.arrival_time) : 0;
          // A cancelled train stays on the timetable at its scheduled time, crossed out.
          var cancelled = isCancelled(p);
          var t = cancelled ? scheduled : (predicted || scheduled);
          var flag = cancelled ? FLAG_CANCELLED
                   : predicted && predicted - scheduled >= DELAY_SEC ? FLAG_DELAYED : FLAG_NONE;
          if (t > nowSec) {
            trains.push({ time: t, dest: ' ', flag: flag,
                          to: badgedName(row.route, '', trip && trip.attributes.headsign) });
          }
        });
      }
      if (--pending > 0) return;
      if (failed) return callback(failed);
      trains.sort(function(x, y) { return x.time - y.time; });
      callback(null, { mode: MODE_CLOCK, trains: trains.slice(0, MAX_TIMES) });
    });
  });
}

var NO_BRANCH = ' ';
// Red Line southbound trains split at JFK/UMass.
var RED_BRANCHES = { Ashmont: 'A', Braintree: 'B' };
var RED_SOUTHBOUND = '0';
// Stops past the split, where only one Red Line branch runs.
var RED_SINGLE_BRANCH_STOPS = [
  'place-shmnl', 'place-fldcr', 'place-smmnl', 'place-asmnl',
  'place-nqncy', 'place-wlsta', 'place-qnctr', 'place-qamnl', 'place-brntn'
];
// Which of a merged Green Line row's branches serve its stop, keyed by "routes|stop".
var s_greenBranchCount = {};

function isMergedGreen(route) {
  return /^Green-[A-Z](,Green-[A-Z])+$/.test(route);
}

// The branch letter for one departure: "Green-D" -> "D", Red Line "Ashmont" -> "A".
// Departures without a branch (e.g. a short-turn to JFK/UMass) get NO_BRANCH.
function branchLetter(routeId, headsign) {
  var green = /^Green-([A-Z])$/.exec(routeId);
  if (green) return green[1];
  if (routeId === 'Red') return RED_BRANCHES[headsign] || NO_BRANCH;
  return NO_BRANCH;
}

// Calls back with whether a row's departures can be on more than one branch,
// so letters are shown even when the next few trains happen to share a branch.
function rowHasBranches(row, callback) {
  if (row.route === 'Red') {
    return callback(row.direction === RED_SOUTHBOUND &&
                    RED_SINGLE_BRANCH_STOPS.indexOf(row.stop) < 0);
  }
  if (!isMergedGreen(row.route)) return callback(false);
  var cacheKey = row.route + '|' + row.stop;
  if (s_greenBranchCount[cacheKey] !== undefined) {
    return callback(s_greenBranchCount[cacheKey] > 1);
  }
  apiGet('/routes', {
    'filter[stop]': row.stop,
    'filter[type]': '0',
    'fields[route]': 'short_name'
  }, function(err, json) {
    if (err) return callback(true);
    var branches = row.route.split(',');
    var count = json.data.filter(function(r) { return branches.indexOf(r.id) >= 0; }).length;
    s_greenBranchCount[cacheKey] = count;
    callback(count > 1);
  });
}

// Subway and light rail: live predictions, each tagged with its branch letter
// when the row's line branches at or beyond its stop.
function fetchSubway(row, callback) {
  rowHasBranches(row, function(showBranches) {
    fetchPredictions(row, showBranches, callback);
  });
}

// Bus rows that have had buses to more than one destination, keyed by
// "route|stop|direction|destination". Once seen, the row keeps its letters even while the
// next few buses happen to share a destination.
var s_busSplit = {};

// Tags each bus with the first letter of its destination where a route splits.
function tagBusDestinations(row, trains) {
  var key = row.route + '|' + row.stop + '|' + row.direction + '|' + (row.destination || '');
  var seen = {};
  trains.forEach(function(train) { seen[train.headsign] = true; });
  if (Object.keys(seen).length > 1) s_busSplit[key] = true;
  if (!s_busSplit[key]) return;
  trains.forEach(function(train) {
    var letter = train.headsign.charAt(0).toUpperCase();
    train.dest = /[A-Z0-9]/.test(letter) ? letter : NO_BRANCH;
  });
}

function fetchPredictions(row, showBranches, callback) {
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
    var bus = isBus(row.route);
    var trains = [];
    json.data.forEach(function(pred) {
      var a = pred.attributes;
      if (isCancelled(a)) return;
      // Arrival-only predictions are trains terminating here.
      if (!a.departure_time) return;
      var t = parseIso(a.departure_time);
      if (t <= nowSec) return;
      var dest = NO_BRANCH;
      var rel = pred.relationships || {};
      var trip = rel.trip && rel.trip.data ? included['trip:' + rel.trip.data.id] : null;
      var headsign = trip ? trip.attributes.headsign : '';
      // A bus row set to one destination only keeps the buses going there.
      if (bus && row.destination && headsign !== row.destination) return;
      if (showBranches) {
        dest = branchLetter(rel.route && rel.route.data ? rel.route.data.id : '', headsign);
      }
      var routeId = rel.route && rel.route.data ? rel.route.data.id : row.route;
      trains.push({ time: t, dest: dest, to: badgedName(routeId, '', headsign), headsign: headsign });
    });
    trains.sort(function(x, y) { return x.time - y.time; });
    trains = trains.slice(0, MAX_TIMES);
    if (bus) tagBusDestinations(row, trains);
    callback(null, { mode: bus ? MODE_BUS : MODE_MINUTES, trains: trains });
  });
}

// The watch fonts are only relied on for ASCII, and it measures text in bytes.
function alertText(header) {
  var text = String(header || '')
    .replace(/[\u2018\u2019]/g, "'")
    .replace(/[\u201C\u201D]/g, '"')
    .replace(/[\u2013\u2014]/g, '-')
    .replace(/[^\x20-\x7E]/g, ' ')
    .replace(/\s+/g, ' ')
    .replace(/^ | $/g, '');
  if (text.length > ALERT_MAX_CHARS) {
    text = text.slice(0, ALERT_MAX_CHARS - 3) + '...';
  }
  return text;
}

// Whether an alert applies to this row's line, station and direction. An
// alert that names no station or direction covers all of them.
function alertApplies(alert, row) {
  // A merged Green Line row lists several route ids.
  var routes = row.route.split(',');
  return (alert.attributes.informed_entity || []).some(function(e) {
    return routes.indexOf(e.route) >= 0 &&
           (!e.stop || e.stop === row.stop) &&
           (e.direction_id === null || e.direction_id === undefined ||
            String(e.direction_id) === row.direction);
  });
}

// The most severe alert in effect now for the row, as text ('' if none).
function fetchAlert(row, callback) {
  apiGet('/alerts', {
    'filter[route]': row.route,
    'filter[datetime]': 'NOW',
    'fields[alert]': 'header,severity,informed_entity'
  }, function(err, json) {
    if (err) return callback(err);
    var best = null;
    json.data.forEach(function(alert) {
      var severity = alert.attributes.severity || 0;
      if (severity < ALERT_MIN_SEVERITY || !alertApplies(alert, row)) return;
      if (!best || severity > best.attributes.severity) best = alert;
    });
    callback(null, best ? alertText(best.attributes.header) : '');
  });
}

// The station id chosen for each dock slot ('' if unused). A saved value is
// "<station id>|<station name>"; the name is only for the settings page.
function bikeStations(saved) {
  var ids = [];
  for (var i = 0; i < NUM_BIKES; i++) {
    var v = saved['BIKE[' + i + ']'];
    ids.push(v ? String(v).split('|')[0] : '');
  }
  return ids;
}

function hasBikes(ids) {
  return ids.some(function(id) { return id; });
}

// One dock as the watch's two status characters: a letter for how many eighths
// of the dock's spaces hold a rentable bike, then a digit for how many of
// those are e-bikes (at most BIKE_MAX_EBIKES).
// Must match BIKE_CLOSED in src/c/watchface.c: the level letter after full.
var BIKE_CLOSED = String.fromCharCode('a'.charCodeAt(0) + BIKE_LEVELS + 3);

function bikeLetter(station) {
  // A dock that is out of service, or gone from the feed altogether.
  if (!station || !station.is_installed || !station.is_renting) return BIKE_CLOSED + '0';
  var bikes = station.num_bikes_available || 0;
  var total = bikes + (station.num_bikes_disabled || 0) +
              (station.num_docks_available || 0) + (station.num_docks_disabled || 0);
  var level = total > 0 ? Math.round(bikes * BIKE_LEVELS / total) : 0;
  // Never show empty while there is a bike.
  if (bikes > 0 && level < 1) level = 1;
  // Nowhere, or almost nowhere, to return a bike is what matters most at a
  // glance, so it gets its own shapes whatever the share of bikes.
  var free = station.num_docks_available || 0;
  if (free === 0) level = BIKE_FULL;
  else if (free === 1) level = BIKE_ONE_FREE;
  else if (level >= BIKE_LEVELS) level = BIKE_LEVELS - 1;
  var ebikes = Math.min(station.num_ebikes_available || 0, BIKE_MAX_EBIKES);
  return String.fromCharCode('a'.charCodeAt(0) + level) + ebikes;
}

// Calls back with the status string for the given dock slots.
function fetchBikes(ids, callback, attempt) {
  attempt = attempt || 0;
  getJson(BIKE_STATUS_URLS[attempt], function(err, json) {
    if (err || !json || !json.data || !json.data.stations) {
      // Fall back to the next address before giving up.
      if (attempt + 1 < BIKE_STATUS_URLS.length) {
        console.log('Bluebikes feed ' + (attempt + 1) + ' failed, trying the next');
        return fetchBikes(ids, callback, attempt + 1);
      }
      return callback(err || new Error('Unexpected dock feed'));
    }
    var wanted = {};
    ids.forEach(function(id) { if (id) wanted[id] = null; });
    json.data.stations.forEach(function(station) {
      if (wanted.hasOwnProperty(station.station_id)) wanted[station.station_id] = station;
    });
    callback(null, ids.map(function(id) {
      return id ? bikeLetter(wanted[id]) : BIKE_NONE + '0';
    }).join(''));
  });
}

var s_bikes_started = 0;

// The docks refresh every minute, between the two-minute train refreshes.
function refreshBikes() {
  var ids = bikeStations(savedSettings());
  if (!hasBikes(ids)) {
    // The docks were removed in the settings: clear them and settle the indicator.
    var none = {};
    none[keys.BIKES] = new Array(NUM_BIKES + 1).join(BIKE_NONE + '0');
    none[keys.BIKE_FAILED] = 0;
    return sendToWatch(none, 1, 'no docks');
  }
  if (Date.now() - s_bikes_started < FETCH_HUNG_MS) return;
  s_bikes_started = Date.now();
  fetchBikes(ids, function(err, status) {
    s_bikes_started = 0;
    var dict = {};
    if (err) {
      console.log('Bluebikes fetch failed: ' + err.message);
      dict[keys.BIKE_FAILED] = 1;
      return sendToWatch(dict, 1, 'dock fetch failure');
    }
    dict[keys.BIKES] = status;
    dict[keys.BIKE_FAILED] = 0;
    sendToWatch(dict, 1, 'dock status ' + status);
  });
}

// Must match TO_SIZE - 1 in src/c/watchface.c.
var TO_MAX_CHARS = 16;
var DEST_SHORT = [
  ['North Station', 'N Station'], ['South Station', 'S Station'], ['Government', 'Govt'],
  [' Street', ' St'], [' Square', ' Sq'], [' Center', ' Ctr'], [' Junction', ' Jct'],
  [' Heights', ' Hts'], [' Circle', ' Cir'], [' College', ' Coll']
];

// A train's destination, shortened to fit the departure board's column.
function shortDest(headsign) {
  var s = String(headsign || '').split('/')[0].split(' via ')[0];
  DEST_SHORT.forEach(function(pair) { s = s.replace(pair[0], pair[1]); });
  return s.replace(/\|/g, ' ').slice(0, TO_MAX_CHARS).replace(/\s+$/, '');
}

// The departure boards start each destination with a badge for its line. The
// watch draws it from the first character: must match the BADGE_ characters
// in src/c/watchface.c.
var BADGE_SUBWAY = '^';   // then one letter: R, O or B
var BADGE_GREEN = '~';    // then the branch letter (M for the Mattapan Line)
var BADGE_BUS = '#';      // then the route number and a space
var BADGE_RAIL = '=';     // commuter rail
// Bus routes whose number on the street differs from their id.
var BUS_NAMES = { '741': 'SL1', '742': 'SL2', '743': 'SL3', '751': 'SL4', '749': 'SL5',
                  '746': 'SLW', '747': 'CT2', '708': 'CT3' };

function lineBadge(routeId, shortName) {
  routeId = String(routeId || '');
  if (routeId === 'Red') return BADGE_SUBWAY + 'R';
  if (routeId === 'Orange') return BADGE_SUBWAY + 'O';
  if (routeId === 'Blue') return BADGE_SUBWAY + 'B';
  if (routeId === 'Mattapan') return BADGE_GREEN + 'M';
  if (routeId.indexOf('Green-') === 0) return BADGE_GREEN + routeId.charAt(6);
  if (isCommuterRail(routeId)) return BADGE_RAIL;
  if (!routeId) return '';
  return BADGE_BUS + String(shortName || BUS_NAMES[routeId] || routeId).slice(0, 3) + ' ';
}

// A departure's line badge and destination, within the watch's destination column.
function badgedName(routeId, shortName, headsign) {
  return (lineBadge(routeId, shortName) + shortDest(headsign)).slice(0, TO_MAX_CHARS)
    .replace(/\s+$/, '');
}

// ---- Departure board: nearby ----
// Must match LAYOUT_NEARBY in src/c/watchface.c and the Layout options in config.js.
var LAYOUT_NEARBY = 3;
var NEARBY_DEFAULT_RADIUS_M = 400;
// A position this recent is reused, so the phone is asked at most this often.
var NEARBY_POSITION_MS = 5 * 60 * 1000;
// The list of nearby stops is kept until the phone has moved this far.
var NEARBY_MOVE_M = 150;
var METERS_PER_DEGREE = 111000;
var s_position = null;   // { t, lat, lon }
var s_nearStops = null;  // { lat, lon, radius, stops: { id: meters away } }

function isNearby(saved) {
  return parseInt(saved.LAYOUT, 10) === LAYOUT_NEARBY;
}

function nearbyRadius(saved) {
  var r = parseInt(saved.RADIUS, 10);
  return r > 0 ? r : NEARBY_DEFAULT_RADIUS_M;
}

function metersBetween(lat1, lon1, lat2, lon2) {
  var rad = Math.PI / 180;
  var dy = (lat2 - lat1) * METERS_PER_DEGREE;
  var dx = (lon2 - lon1) * METERS_PER_DEGREE * Math.cos(lat1 * rad);
  return Math.sqrt(dx * dx + dy * dy);
}

// The last known position is used straight away, however old, and a newer
// one is asked for on the side: the departures never wait for the phone's
// location service unless there is no position at all yet.
function nearbyPosition(callback) {
  function keep(err, pos) {
    if (pos) s_position = { t: Date.now(), lat: pos.coords.latitude, lon: pos.coords.longitude };
  }
  if (s_position) {
    if (Date.now() - s_position.t >= NEARBY_POSITION_MS) askLocation(keep);
    return callback(null, s_position);
  }
  askLocation(function(err, pos) {
    keep(err, pos);
    if (s_position) callback(null, s_position);
    else callback(new Error('Location: ' + (err ? err.message : 'none')));
  });
}

// Calls back with the stops within radius metres, as { stop id: metres away }.
function nearbyStops(pos, radius, callback) {
  var c = s_nearStops;
  if (c && c.radius === radius && metersBetween(c.lat, c.lon, pos.lat, pos.lon) < NEARBY_MOVE_M) {
    return callback(null, c.stops);
  }
  apiGet('/stops', {
    'filter[latitude]': pos.lat.toFixed(5),
    'filter[longitude]': pos.lon.toFixed(5),
    // The API measures in degrees and its circle is not exact, so ask a
    // little wide and measure properly below.
    'filter[radius]': (radius * 1.5 / METERS_PER_DEGREE).toFixed(5),
    'filter[route_type]': '0,1,2,3',
    'sort': 'distance',
    'page[limit]': 60,
    'fields[stop]': 'latitude,longitude'
  }, function(err, json) {
    if (err) return callback(err);
    var stops = {};
    json.data.forEach(function(stop) {
      var a = stop.attributes;
      var m = metersBetween(pos.lat, pos.lon, a.latitude, a.longitude);
      if (m <= radius) stops[stop.id] = m;
    });
    s_nearStops = { lat: pos.lat, lon: pos.lon, radius: radius, stops: stops };
    callback(null, stops);
  });
}

function nearbyName(routeId, route, headsign) {
  return badgedName(routeId, route ? route.short_name : '', headsign);
}

// Calls back with the next departures from every stop within radius metres,
// spread over the watch's three rows (the board merges them back by time),
// and the text for the board's column bar.
function fetchNearby(radius, callback) {
  var label = radius + 'm';
  var empty = [];
  for (var r = 0; r < NUM_ROWS; r++) empty.push({ mode: MODE_MINUTES, trains: [] });
  nearbyPosition(function(err, pos) {
    if (err) return callback(err, null, 'NO FIX');
    nearbyStops(pos, radius, function(err2, stops) {
      if (err2) return callback(err2, null, label);
      var ids = Object.keys(stops);
      if (ids.length === 0) return callback(null, empty, label);
      var nowMs = Date.now();
      var nowSec = Math.floor(nowMs / 1000);
      var windows = serviceWindows(nowMs, 0);
      var best = {};
      var pendingWindows = windows.length;
      var failure = null;
      windows.forEach(function(win) {
        apiGet('/schedules', {
          'filter[stop]': ids.join(','),
          'filter[date]': win.date,
          'filter[min_time]': win.minTime,
          'include': 'prediction,trip,route',
          'sort': 'departure_time',
          'page[limit]': 80,
          'fields[schedule]': 'departure_time',
          'fields[prediction]': 'departure_time,schedule_relationship',
          'fields[trip]': 'headsign',
          'fields[route]': 'short_name,type'
        }, function(err3, json) {
          if (err3) {
            failure = err3;
          } else {
            var included = indexIncluded(json);
            json.data.forEach(function(sched) {
              var rel = sched.relationships || {};
              var routeId = rel.route && rel.route.data ? rel.route.data.id : '';
              var route = included['route:' + routeId];
              var trip = rel.trip && rel.trip.data ? included['trip:' + rel.trip.data.id] : null;
              var pred = rel.prediction && rel.prediction.data
                ? included['prediction:' + rel.prediction.data.id] : null;
              var p = pred ? pred.attributes : null;
              // No departure time means the train or bus ends its run here.
              if (!sched.attributes.departure_time) return;
              var scheduled = parseIso(sched.attributes.departure_time);
              var predicted = p ? parseIso(p.departure_time) : 0;
              var cancelled = isCancelled(p);
              var rail = isCommuterRail(routeId);
              // As on the saved stations: only commuter rail keeps a canceled
              // train on the board or marks a late one.
              if (cancelled && !rail) return;
              var t = cancelled ? scheduled : (predicted || scheduled);
              if (t <= nowSec) return;
              var flag = !rail ? FLAG_NONE : cancelled ? FLAG_CANCELLED
                       : predicted && predicted - scheduled >= DELAY_SEC ? FLAG_DELAYED : FLAG_NONE;
              var stopId = rel.stop && rel.stop.data ? rel.stop.data.id : '';
              var away = stops[stopId] === undefined ? radius : stops[stopId];
              var tripId = trip ? rel.trip.data.id : stopId + '@' + t;
              // A trip that calls at several nearby stops is listed once, at the closest.
              if (best[tripId] && best[tripId].away <= away) return;
              best[tripId] = { time: t, dest: NO_BRANCH, flag: flag, away: away,
                               to: nearbyName(routeId, route ? route.attributes : null,
                                              trip ? trip.attributes.headsign : '') };
            });
          }
          if (--pendingWindows > 0) return;
          var trains = Object.keys(best).map(function(k) { return best[k]; });
          if (failure && trains.length === 0) return callback(failure, null, label);
          trains.sort(function(x, y) { return x.time - y.time; });
          var result = [];
          for (var row = 0; row < NUM_ROWS; row++) {
            result.push({ mode: MODE_MINUTES,
                          trains: trains.slice(row * MAX_TIMES, (row + 1) * MAX_TIMES) });
          }
          callback(null, result, label);
        });
      });
    });
  });
}

function putRow(dict, row, result) {
  var dest = '';
  var flags = '';
  var to = [];
  for (var i = 0; i < MAX_TIMES; i++) {
    var train = result.trains[i];
    dict[keys.TIMES + row * MAX_TIMES + i] = train ? train.time : 0;
    dest += train ? train.dest : NO_BRANCH;
    flags += train && train.flag ? train.flag : FLAG_NONE;
    if (train) to.push(train.to || '');
  }
  dict[keys.TO + row] = to.join('|');
  dict[keys.DEST + row] = dest;
  dict[keys.FLAGS + row] = flags;
  dict[keys.MODE + row] = result.mode;
}

function sameRow(a, b) {
  return a.route === b.route && a.stop === b.stop && a.direction === b.direction &&
         a.destination === b.destination;
}

// Messages go out one at a time, in order, so a cleared row can't overtake newer data.
var s_outbox = [];
var s_sending = false;

function sendToWatch(dict, retries, what) {
  s_outbox.push({ dict: dict, retries: retries, what: what || 'transit data' });
  pumpOutbox();
}

function pumpOutbox() {
  if (s_sending || s_outbox.length === 0) return;
  s_sending = true;
  var msg = s_outbox[0];
  function next() {
    s_outbox.shift();
    s_sending = false;
    pumpOutbox();
  }
  Pebble.sendAppMessage(msg.dict, function() {
    console.log('Sent ' + msg.what + ' to watch');
    next();
  }, function(e) {
    console.log('Failed to send ' + msg.what + ': ' + JSON.stringify(e));
    if (msg.retries-- > 0) {
      setTimeout(function() {
        s_sending = false;
        pumpOutbox();
      }, 2000);
    } else {
      next();
    }
  });
}

var s_refreshes = 0;

function refresh() {
  // Every tenth refresh (about 20 minutes) the history is logged again, so a
  // log capture attached at any time sees it without the app restarting.
  if (++s_refreshes % 10 === 0) logDiagHistory();
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
  // The location test runs on the side; the nearby board asks for itself.
  if (!isNearby(saved)) testLocation();
  var dict = {};
  var rows = [];
  var nearby = isNearby(saved);
  // The nearby board is one lookup in place of the three saved stations.
  var pending = nearby ? 1 : NUM_ROWS;
  for (var i = 0; i < NUM_ROWS; i++) {
    rows.push(rowConfig(i, saved));
    if (!nearby && rows[i].alerts && rows[i].set) pending++;
  }
  var bikeIds = bikeStations(saved);
  if (hasBikes(bikeIds)) pending++;
  var failed = 0;
  // The docks have their own indicator on the watch, separate from the rows.
  dict[keys.BIKE_FAILED] = 0;
  function done() {
    if (--pending > 0) return;
    s_fetching = false;
    // Rows that failed to load are omitted so the watch keeps its last data;
    // the count tells it to show the fetch as failed.
    dict[keys.FETCH_FAILED] = failed;
    sendToWatch(dict, 2);
    if (s_refetch) refresh();
  }

  if (hasBikes(bikeIds)) {
    fetchBikes(bikeIds, function(err, status) {
      if (fetchId !== s_fetch_id) return;
      if (err) {
        console.log('Bluebikes fetch failed: ' + err.message);
        dict[keys.BIKE_FAILED] = 1;
      } else {
        dict[keys.BIKES] = status;
      }
      done();
    });
  } else {
    dict[keys.BIKES] = new Array(NUM_BIKES + 1).join(BIKE_NONE + '0');
  }

  if (nearby) {
    fetchNearby(nearbyRadius(saved), function(err, result, label) {
      if (fetchId !== s_fetch_id) return;
      dict[keys.NEAR] = label;
      if (err) {
        console.log('Nearby fetch failed: ' + err.message);
        failed++;
      } else {
        for (var row = 0; row < NUM_ROWS; row++) {
          dict[keys.ALERT + row] = '';
          putRow(dict, row, result[row]);
        }
      }
      done();
    });
    return;
  }

  for (var r = 0; r < NUM_ROWS; r++) {
    (function(row) {
      var cfg = rows[row];
      if (!cfg.set) {
        dict[keys.ALERT + row] = '';
        putRow(dict, row, { mode: MODE_UNSET, trains: [] });
        done();
        return;
      }
      if (cfg.alerts) {
        fetchAlert(cfg, function(err, text) {
          if (fetchId !== s_fetch_id) return;
          // On failure the key is left out so the watch keeps the alert it has.
          if (err) {
            console.log('Row ' + (row + 1) + ' alerts fetch failed: ' + err.message);
          } else if (sameRow(cfg, rowConfig(row, savedSettings()))) {
            dict[keys.ALERT + row] = text;
            if (text) console.log('Row ' + (row + 1) + ' alert: ' + text);
          }
          done();
        });
      } else {
        dict[keys.ALERT + row] = '';
      }
      var fetch = isCommuterRail(cfg.route) ? fetchCommuterRail : fetchSubway;
      fetch(cfg, function(err, result) {
        // A late reply from an abandoned fetch must not disturb its replacement.
        if (fetchId !== s_fetch_id) return;
        if (err) {
          console.log('Row ' + (row + 1) + ' (' + cfg.route + ' ' + cfg.stop + ') fetch failed: ' +
                      err.message);
          failed++;
        } else if (sameRow(cfg, rowConfig(row, savedSettings()))) {
          putRow(dict, row, result);
        }
        done();
      });
    })(r);
  }
}

Pebble.addEventListener('showConfiguration', function() {
  // Rebuilt each time so the page includes the saved stations as fallback options.
  clay = new Clay(config.build(savedSettings(), usageLines(), locationLines()), configPage,
                  { autoHandleEvents: false });
  Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener('webviewclosed', function(e) {
  if (!e || !e.response) return;
  var before = savedSettings();
  var settings = clay.getSettings(e.response);
  storeSettings(localStorage.getItem('clay-settings'));
  var after = savedSettings();
  // Rows now pointing somewhere else are blanked until their new data arrives,
  // so the old line's times and branch letters never linger.
  var cleared = {};
  // The watch only needs the labels; the phone uses the stations to fetch.
  for (var row = 0; row < NUM_ROWS; row++) {
    delete settings[keys.ROUTE + row];
    delete settings[keys.STOP + row];
    delete settings[keys.DIR + row];
    delete settings[keys.ALERT_ON + row];
    if (!sameRow(rowConfig(row, before), rowConfig(row, after))) {
      putRow(cleared, row, { mode: MODE_MINUTES, trains: [] });
    }
  }
  for (var slot = 0; slot < NUM_BIKES; slot++) {
    delete settings[keys.BIKE + slot];
  }
  // The radius is only used on the phone; the watch is told it with the departures.
  delete settings[keys.RADIUS];
  sendToWatch(settings, 3, 'settings');
  if (Object.keys(cleared).length > 0) {
    sendToWatch(cleared, 3, 'cleared rows');
  }
  refresh();
});

Pebble.addEventListener('ready', function() {
  console.log('PebbleKit JS ready');
  logDiagHistory();
  refresh();
});

// Breadcrumbs the watch reports about its previous run (see prv_diag_init in
// src/c/watchface.c). Kept on the phone so they survive a watch reset and can
// be read from `pebble logs` whenever it is next attached.
var DIAG_HISTORY_KEY = 'diag-history';
var DIAG_HISTORY_MAX = 20;

function diagHistory() {
  try {
    return JSON.parse(localStorage.getItem(DIAG_HISTORY_KEY)) || [];
  } catch (e) {
    return [];
  }
}

function recordDiag(text) {
  var history = diagHistory();
  history.push({ t: new Date().toISOString(), d: String(text) });
  history = history.slice(-DIAG_HISTORY_MAX);
  localStorage.setItem(DIAG_HISTORY_KEY, JSON.stringify(history));
  console.log('WATCH DIAG ' + text);
}

function logDiagHistory() {
  var history = diagHistory();
  usageLines(true).forEach(function(line) { console.log('USAGE ' + line); });
  // The position itself stays out of the logs.
  locationLines().forEach(function(line) { console.log('LOCATION ' + line.replace(/ Near .*$/, '')); });
  console.log('DIAG HISTORY ' + history.length + ' entries');
  history.forEach(function(entry) {
    console.log('DIAG ' + entry.t + ' ' + entry.d);
  });
}

Pebble.addEventListener('appmessage', function(e) {
  var payload = (e && e.payload) || {};
  if (payload.DIAG) {
    recordDiag(payload.DIAG);
  }
  if (payload.REQUEST === REQUEST_BIKES) {
    refreshBikes();
  } else if (payload.REQUEST || !payload.DIAG) {
    refresh();
  }
});

// Exposed for tests.
if (typeof module !== 'undefined') {
  module.exports = {
    serviceWindows: serviceWindows, bostonOffsetHours: bostonOffsetHours,
    alertText: alertText, alertApplies: alertApplies, bikeLetter: bikeLetter,
    tagBusDestinations: tagBusDestinations, noteUsage: noteUsage, usageLines: usageLines,
    testLocation: testLocation, locationLines: locationLines, nearbyName: nearbyName, badgedName: badgedName,
    metersBetween: metersBetween,
    shortDest: shortDest
  };
}
