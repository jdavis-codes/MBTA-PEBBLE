var Clay = require('@rebble/clay');
var clayConfig = require('./config');
var clay = new Clay(clayConfig);

var keys = require('message_keys');

var API_BASE = 'https://api-v3.mbta.com';
// Optional: a free key from https://api-v3.mbta.com/ raises the rate limit.
var MBTA_API_KEY = '';
var MAX_TIMES = 4;
// Look back this far when querying schedules so delayed trains are still found.
var LOOKBACK_MS = 20 * 60 * 1000;

var CR_ROUTE = 'CR-Newburyport';
var NORTH_STATION = 'place-north';
var SALEM = 'place-ER-0168';
var CENTRAL = 'place-cntsq';

var s_fetching = false;

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

// MBTA service days run past midnight (e.g. "25:10"), so early-morning
// queries need yesterday's date with hours >= 24.
function serviceWindow(nowMs) {
  var start = new Date(nowMs - LOOKBACK_MS);
  var hours = start.getHours();
  var day = start;
  if (hours < 3) {
    day = new Date(start.getTime() - 24 * 60 * 60 * 1000);
    hours += 24;
  }
  return {
    date: day.getFullYear() + '-' + pad2(day.getMonth() + 1) + '-' + pad2(day.getDate()),
    minTime: pad2(hours) + ':' + pad2(start.getMinutes())
  };
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

// Commuter rail: scheduled departures, overridden by live predictions when available.
function fetchCommuterRail(stop, directionId, callback) {
  var nowMs = Date.now();
  var win = serviceWindow(nowMs);
  apiGet('/schedules', {
    'filter[stop]': stop,
    'filter[route]': CR_ROUTE,
    'filter[direction_id]': directionId,
    'filter[date]': win.date,
    'filter[min_time]': win.minTime,
    'include': 'prediction',
    'sort': 'departure_time',
    'page[limit]': 12,
    'fields[schedule]': 'departure_time,arrival_time',
    'fields[prediction]': 'departure_time,arrival_time,schedule_relationship'
  }, function(err, json) {
    if (err) return callback(err);
    var included = indexIncluded(json);
    var nowSec = Math.floor(nowMs / 1000);
    var times = [];
    json.data.forEach(function(sched) {
      var a = sched.attributes;
      var rel = sched.relationships && sched.relationships.prediction;
      var pred = rel && rel.data ? included['prediction:' + rel.data.id] : null;
      var p = pred ? pred.attributes : null;
      if (isCancelled(p)) return;
      var t = parseIso((p && (p.departure_time || p.arrival_time)) ||
                       a.departure_time || a.arrival_time);
      if (t > nowSec) times.push(t);
    });
    times.sort(function(x, y) { return x - y; });
    callback(null, times.slice(0, MAX_TIMES));
  });
}

// Red Line southbound at Central: live predictions, tagged with the
// first letter of the destination (A = Ashmont, B = Braintree).
function fetchCentral(callback) {
  apiGet('/predictions', {
    'filter[stop]': CENTRAL,
    'filter[route]': 'Red',
    'filter[direction_id]': 0,
    'include': 'trip',
    'sort': 'departure_time',
    'page[limit]': 10,
    'fields[prediction]': 'departure_time,arrival_time,schedule_relationship',
    'fields[trip]': 'headsign'
  }, function(err, json) {
    if (err) return callback(err);
    var included = indexIncluded(json);
    var nowSec = Math.floor(Date.now() / 1000);
    var trains = [];
    json.data.forEach(function(pred) {
      var a = pred.attributes;
      if (isCancelled(a)) return;
      var t = parseIso(a.departure_time || a.arrival_time);
      if (t <= nowSec) return;
      var tripRel = pred.relationships && pred.relationships.trip;
      var trip = tripRel && tripRel.data ? included['trip:' + tripRel.data.id] : null;
      var headsign = trip && trip.attributes.headsign ? trip.attributes.headsign : '?';
      trains.push({ time: t, dest: headsign.charAt(0).toUpperCase() });
    });
    trains.sort(function(x, y) { return x.time - y.time; });
    callback(null, trains.slice(0, MAX_TIMES));
  });
}

function putTimes(dict, baseKey, times) {
  for (var i = 0; i < MAX_TIMES; i++) {
    dict[baseKey + i] = i < times.length ? times[i] : 0;
  }
}

function sendToWatch(dict, retries) {
  Pebble.sendAppMessage(dict, function() {
    console.log('Sent transit data to watch');
  }, function(e) {
    console.log('Failed to send transit data: ' + JSON.stringify(e));
    if (retries > 0) {
      setTimeout(function() { sendToWatch(dict, retries - 1); }, 2000);
    }
  });
}

function refresh() {
  if (s_fetching) return;
  s_fetching = true;

  var dict = {};
  var pending = 3;
  function done() {
    if (--pending > 0) return;
    s_fetching = false;
    // Stations that failed to load are omitted so the watch keeps its last data.
    if (Object.keys(dict).length > 0) {
      sendToWatch(dict, 2);
    }
  }

  fetchCommuterRail(NORTH_STATION, 0, function(err, times) {
    if (err) console.log('North Station fetch failed: ' + err.message);
    else putTimes(dict, keys.N_TIMES, times);
    done();
  });

  fetchCommuterRail(SALEM, 1, function(err, times) {
    if (err) console.log('Salem fetch failed: ' + err.message);
    else putTimes(dict, keys.S_TIMES, times);
    done();
  });

  fetchCentral(function(err, trains) {
    if (err) {
      console.log('Central fetch failed: ' + err.message);
    } else {
      putTimes(dict, keys.C_TIMES, trains.map(function(t) { return t.time; }));
      var dest = '';
      for (var i = 0; i < MAX_TIMES; i++) {
        dest += i < trains.length ? trains[i].dest : ' ';
      }
      dict[keys.C_DEST] = dest;
    }
    done();
  });
}

Pebble.addEventListener('ready', function() {
  console.log('PebbleKit JS ready');
  refresh();
});

Pebble.addEventListener('appmessage', function() {
  refresh();
});
