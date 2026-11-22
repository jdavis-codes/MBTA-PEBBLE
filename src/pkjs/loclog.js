// Location log (docs/location-logging-spec.md). A development tool: while it
// is switched on in the settings, one record is kept for every location
// request the phone makes, so the nearby board's options can be judged against
// a real commute.
//
// - Records hold the real position and time. The owner asked for that: the
//   log is only ever their own data, and it has to be easy to analyze.
// - The log is kept in the phone's localStorage. When a GitHub repository and
//   a token are given in the settings, new records are also uploaded there as
//   small CSV files, about every ten minutes.
// - Nothing from the log goes to console.log.
// - It is off by default and turns itself off after seven days.

var META_KEY = 'loclog-meta';
var CHUNK_KEY = 'loclog-c';
// The log is a ring of chunks so that a write only rewrites the newest one.
// A record's number is its chunk's number times CHUNK_RECORDS plus its place.
var CHUNK_RECORDS = 100;
var MAX_CHUNKS = 30;          // 3,000 records: about two days at one a minute
var MAX_MARKS = 200;
var MAX_DAYS = 7;
var DAY_MS = 24 * 60 * 60 * 1000;
var METERS_PER_DEGREE = 111000;

// Uploads. A file is sent once this many records are waiting, or at once
// when the settings are saved. Each file is new: nothing is ever rewritten.
var UPLOAD_MIN = 10;
var UPLOAD_MAX = 400;
// The iPhone can leave a request unanswered for ever; after this long it is
// given up on, checked against the clock when the next record is kept.
var UPLOAD_HUNG_MS = 60 * 1000;
var GITHUB_API = 'https://api.github.com';

// Record layout. Each record is an array to keep the log small.
var T = 0, TOOK = 1, RESULT = 2, LAT = 3, LON = 4, ACC = 5, ALT = 6, AGE = 7, SPEED = 8,
    HEADING = 9, STOPS = 10, REFETCH = 11, STOP = 12, STOP_DIST = 13, SESSION = 14;
var RESULTS = ['ok', 'failed', 'denied', 'timeout', 'no answer'];
var R_OK = 0;
// The columns of the CSV, for records and markers alike. A marker is a row
// whose result is "mark". t is Unix time in seconds; time is the phone's
// local time, for reading by eye.
var CSV_HEADER = 't,time,took_ms,result,lat,lon,acc,alt,age,speed,heading,stops,refetch,' +
                 'stop,stop_dist,session,mark';

// Must match the marker buttons in config.js.
var MARKS = {
  1: { name: 'Entered station', state: 'In a station' },
  2: { name: 'On platform', state: 'On a platform' },
  3: { name: 'Boarded', state: 'On a train' },
  4: { name: 'Got off', state: 'On a platform' },
  5: { name: 'Back at street level', state: 'At street level' }
};
var STATES = ['At street level', 'In a station', 'On a platform', 'On a train'];
// A marker describes where the owner is until the next marker, but not for
// ever: a forgotten "Back at street level" must not count a day at a desk.
var STATE_LASTS_MS = 2 * 60 * 60 * 1000;
var STREET_LASTS_MS = 20 * 60 * 1000;

var s_meta = null;
var s_chunk = null;        // the newest chunk, kept in memory between writes
var s_newSession = true;   // the first record after the phone side starts
var s_uploading = 0;       // when the upload in flight was started, or 0
var s_uploadId = 0;

function read(key) {
  try {
    return JSON.parse(localStorage.getItem(key));
  } catch (e) {
    return null;
  }
}

function write(key, value) {
  try {
    localStorage.setItem(key, JSON.stringify(value));
    return true;
  } catch (e) {
    return false;
  }
}

function remove(key) {
  try {
    if (localStorage.removeItem) localStorage.removeItem(key);
    else localStorage.setItem(key, '');
  } catch (e) { /* nothing to do */ }
}

function meta() {
  if (!s_meta) {
    s_meta = read(META_KEY) ||
             { running: false, first: 0, last: 0, lost: 0, sessions: 0, marks: [], sent: 0 };
  }
  return s_meta;
}

function saveMeta() {
  write(META_KEY, s_meta);
}

// Whether records are being kept right now. Stops the log once it is seven days old.
function isOn() {
  var m = meta();
  if (!m.running) return false;
  if (Date.now() - m.started >= MAX_DAYS * DAY_MS) {
    m.running = false;
    m.ended = 'It turned itself off after seven days.';
    saveMeta();
    return false;
  }
  return true;
}

// Starts a new log. The records of the previous one are removed from the phone.
function start() {
  var old = meta();
  for (var i = old.first; i <= old.last; i++) remove(CHUNK_KEY + i);
  s_meta = { running: true, started: Date.now(), first: 0, last: 0, lost: 0, sessions: 0,
             marks: [], sent: 0 };
  s_chunk = [];
  s_newSession = true;
  saveMeta();
}

// Stops recording. The records are kept until the next start.
function stop() {
  var m = meta();
  if (!m.running) return;
  m.running = false;
  m.ended = 'Switched off.';
  saveMeta();
}

function newestChunk() {
  if (!s_chunk) s_chunk = read(CHUNK_KEY + meta().last) || [];
  return s_chunk;
}

function append(rec) {
  var m = meta();
  var chunk = newestChunk();
  if (chunk.length >= CHUNK_RECORDS) {
    m.last++;
    chunk = s_chunk = [];
    while (m.last - m.first >= MAX_CHUNKS) remove(CHUNK_KEY + m.first++);
  }
  chunk.push(rec);
  if (!write(CHUNK_KEY + m.last, chunk)) {
    // The phone would not store it. Count it, so the summary can say so.
    chunk.pop();
    m.lost++;
  }
  saveMeta();
}

function round1(x) {
  return Math.round(x * 10) / 10;
}

function number(x) {
  return typeof x === 'number' && isFinite(x) ? x : null;
}

// Speed and heading: the iPhone reports a negative number when it has none.
function reported(x) {
  return number(x) !== null && x >= 0 ? round1(x) : null;
}

function resultCode(err) {
  if (!err) return R_OK;
  if (/denied/.test(err)) return 2;
  if (/timed out/.test(err)) return 3;
  if (/no answer/.test(err)) return 4;
  return 1;
}

// Keeps one record. started: when the request was made (ms). err: why it
// failed, or null. pos: the phone's answer. near: what is known about the
// stops around, { stops, stop, stopDist }, any of it missing. refetch: whether
// this position made the phone fetch the departures again.
// Never throws: the log must not get in the way of a refresh.
function record(started, err, pos, near, refetch) {
  try {
    if (!isOn()) return;
    var m = meta();
    var now = Date.now();
    if (s_newSession) {
      s_newSession = false;
      m.sessions++;
    }
    var rec = [started, now - started, resultCode(err)];
    if (pos && pos.coords) {
      var c = pos.coords;
      near = near || {};
      rec[LAT] = Math.round(c.latitude * 1e6) / 1e6;
      rec[LON] = Math.round(c.longitude * 1e6) / 1e6;
      rec[ACC] = reported(c.accuracy);
      rec[ALT] = number(c.altitude) === null ? null : round1(c.altitude);
      rec[AGE] = pos.timestamp ? Math.max(0, round1((now - pos.timestamp) / 1000)) : null;
      rec[SPEED] = reported(c.speed);
      rec[HEADING] = reported(c.heading);
      rec[STOPS] = typeof near.stops === 'number' ? near.stops : null;
      rec[REFETCH] = refetch ? 1 : 0;
      rec[STOP] = near.stop || null;
      rec[STOP_DIST] = near.stop ? Math.round(near.stopDist) : null;
    }
    rec[SESSION] = m.sessions;
    append(rec);
  } catch (e) { /* the log is only a diagnostic */ }
}

// Adds the markers set in the settings page: text such as "3@1700000000000"
// (marker number @ the time the button was pressed), separated by commas.
function addMarks(text) {
  if (!text || !isOn()) return 0;
  var m = meta();
  var added = 0;
  String(text).split(',').forEach(function(part) {
    var match = /^(\d)@(\d+)$/.exec(part.trim());
    if (!match || !MARKS[match[1]]) return;
    var t = parseInt(match[2], 10);
    if (t < m.started) return;
    // [when, which, uploaded]
    m.marks.push([t, parseInt(match[1], 10), 0]);
    added++;
  });
  if (added) {
    m.marks.sort(function(a, b) { return a[0] - b[0]; });
    m.marks = m.marks.slice(-MAX_MARKS);
    saveMeta();
  }
  return added;
}

// Records numbered from..to-1 that are still held.
function recordsFrom(from, limit) {
  var m = meta();
  var out = [];
  var i = Math.max(m.first, Math.floor(from / CHUNK_RECORDS));
  for (; i <= m.last && out.length < limit; i++) {
    var chunk = i === m.last ? newestChunk() : read(CHUNK_KEY + i);
    if (!chunk) continue;
    var skip = Math.max(0, from - i * CHUNK_RECORDS);
    out = out.concat(chunk.slice(skip, skip + limit - out.length));
  }
  return out;
}

function allRecords() {
  return recordsFrom(0, MAX_CHUNKS * CHUNK_RECORDS);
}

function held() {
  var m = meta();
  return (m.last - m.first) * CHUNK_RECORDS + newestChunk().length;
}

function pad(n) {
  return n < 10 ? '0' + n : String(n);
}

function dateTime(ms) {
  var d = new Date(ms);
  return d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate()) + ' ' +
         pad(d.getHours()) + ':' + pad(d.getMinutes()) + ':' + pad(d.getSeconds());
}

function csvField(v) {
  return v === null || v === undefined ? '' : String(v).replace(/[,\n"]/g, ' ');
}

function recordRow(r) {
  var fields = [(r[T] / 1000).toFixed(1), dateTime(r[T]), r[TOOK], RESULTS[r[RESULT]]];
  for (var i = LAT; i <= SESSION; i++) fields.push(csvField(r[i]));
  fields.push('');
  return fields.join(',');
}

function markRow(mark) {
  return (mark[0] / 1000).toFixed(1) + ',' + dateTime(mark[0]) + ',,mark,,,,,,,,,,,,,' +
         MARKS[mark[1]].name;
}

// Records and markers as CSV lines in time order, under the header.
function csvLines(records, marks) {
  var rows = records.map(function(r) { return [r[T], recordRow(r)]; })
    .concat(marks.map(function(mark) { return [mark[0], markRow(mark)]; }));
  rows.sort(function(a, b) { return a[0] - b[0]; });
  return [CSV_HEADER].concat(rows.map(function(row) { return row[1]; }));
}

// The log as CSV, from the last `hours` only (0 for all of it), to copy out
// of the settings page.
function traceLines(hours) {
  var m = meta();
  if (!m.started) return [];
  var from = hours ? Date.now() - hours * 3600 * 1000 : 0;
  return csvLines(allRecords().filter(function(r) { return r[T] >= from; }),
                  m.marks.filter(function(mark) { return mark[0] >= from; }));
}

// ---- Upload ----

// Base64 of text that is plain ASCII, which a CSV of numbers is. The phone
// side has no btoa on every phone.
function base64(text) {
  var chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  var out = '';
  for (var i = 0; i < text.length; i += 3) {
    var a = text.charCodeAt(i) & 255;
    var b = i + 1 < text.length ? text.charCodeAt(i + 1) & 255 : 0;
    var c = i + 2 < text.length ? text.charCodeAt(i + 2) & 255 : 0;
    out += chars[a >> 2] + chars[((a & 3) << 4) | (b >> 4)] +
           (i + 1 < text.length ? chars[((b & 15) << 2) | (c >> 6)] : '=') +
           (i + 2 < text.length ? chars[c & 63] : '=');
  }
  return out;
}

// Sends the records and markers not yet uploaded to the repository named in
// the settings, as one new CSV file. force: send however few are waiting.
// Never throws and never waits: a failed upload is tried again with the next.
function upload(saved, force) {
  try {
    var m = meta();
    var repo = String((saved && saved.LOG_REPO) || '').trim().replace(/^https:\/\/github\.com\//, '')
      .replace(/\.git$|\/$/g, '');
    var token = String((saved && saved.LOG_TOKEN) || '').trim();
    if (!m.started || !/^[\w.-]+\/[\w.-]+$/.test(repo) || !token) return;
    var now = Date.now();
    if (s_uploading && now - s_uploading < UPLOAD_HUNG_MS) return;
    // Records the ring dropped before they could be sent are gone.
    var from = Math.max(m.sent || 0, m.first * CHUNK_RECORDS);
    var records = recordsFrom(from, UPLOAD_MAX);
    var marks = m.marks.filter(function(mark) { return !mark[2]; });
    if (!records.length && !marks.length) return;
    if (!force && records.length < UPLOAD_MIN) return;
    var id = ++s_uploadId;
    s_uploading = now;
    var d = new Date(now);
    var path = 'loclog/' + d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate()) +
               '/' + pad(d.getHours()) + pad(d.getMinutes()) + pad(d.getSeconds()) + '-' + from + '.csv';
    var xhr = new XMLHttpRequest();
    xhr.open('PUT', GITHUB_API + '/repos/' + repo + '/contents/' + path);
    xhr.setRequestHeader('Authorization', 'Bearer ' + token);
    xhr.setRequestHeader('Accept', 'application/vnd.github+json');
    xhr.setRequestHeader('Content-Type', 'application/json');
    var finish = function(ok, why) {
      if (id !== s_uploadId || !s_uploading) return;
      s_uploading = 0;
      var mm = meta();
      if (ok) {
        // Unless a new log was started while this was on its way.
        if (mm.started === m.started) {
          mm.sent = from + records.length;
          marks.forEach(function(mark) { mark[2] = 1; });
          mm.uploaded = (mm.uploaded || 0) + records.length;
        }
        mm.uploadedAt = Date.now();
        mm.uploadError = '';
      } else {
        mm.uploadError = why + ' at ' + dateTime(Date.now());
      }
      saveMeta();
    };
    xhr.onload = function() {
      finish(xhr.status === 201 || xhr.status === 200,
             xhr.status === 401 ? 'GitHub did not accept the token'
             : xhr.status === 404 || xhr.status === 403 ? 'the token cannot write to ' + repo
             : 'GitHub answered ' + xhr.status);
    };
    xhr.onerror = function() { finish(false, 'no connection to GitHub'); };
    xhr.send(JSON.stringify({
      message: 'Location log: ' + records.length + ' records',
      content: base64(csvLines(records, marks).join('\n') + '\n')
    }));
  } catch (e) {
    s_uploading = 0;
  }
}

// ---- Summary ----

function quantile(sorted, q) {
  if (!sorted.length) return null;
  return sorted[Math.min(sorted.length - 1, Math.floor(q * sorted.length))];
}

function sortedNumbers(list) {
  return list.filter(function(x) { return typeof x === 'number'; })
             .sort(function(a, b) { return a - b; });
}

function percent(part, whole) {
  return whole ? Math.round(100 * part / whole) + '%' : 'none';
}

function duration(seconds) {
  if (seconds < 120) return Math.round(seconds) + ' s';
  if (seconds < 2 * 3600) return Math.round(seconds / 60) + ' min';
  return round1(seconds / 3600) + ' h';
}

function metersBetween(a, b) {
  var dy = (b[LAT] - a[LAT]) * METERS_PER_DEGREE;
  var dx = (b[LON] - a[LON]) * METERS_PER_DEGREE * Math.cos(a[LAT] * Math.PI / 180);
  return Math.sqrt(dx * dx + dy * dy);
}

// Where the owner said they were at time t (ms), or null.
function stateAt(marks, t) {
  var state = null;
  for (var i = 0; i < marks.length && marks[i][0] <= t; i++) {
    var s = MARKS[marks[i][1]].state;
    var lasts = s === 'At street level' ? STREET_LASTS_MS : STATE_LASTS_MS;
    state = t - marks[i][0] <= lasts ? s : null;
  }
  return state;
}

// The figures for one set of records, as lines of text.
function figures(records) {
  var lines = [];
  var counts = [0, 0, 0, 0, 0];
  var ok = [];
  records.forEach(function(r) {
    counts[r[RESULT]]++;
    if (r[RESULT] === R_OK) ok.push(r);
  });
  lines.push('Requests: ' + records.length + ' made, ' + counts[0] + ' answered, ' +
             counts[1] + ' failed, ' + counts[3] + ' timed out, ' + counts[4] +
             ' unanswered' + (counts[2] ? ', ' + counts[2] + ' denied' : '') + '.');
  if (!ok.length) return lines;
  var acc = sortedNumbers(ok.map(function(r) { return r[ACC]; }));
  if (acc.length) {
    lines.push('Accuracy: median ' + quantile(acc, 0.5) + ' m, 90th percentile ' +
               quantile(acc, 0.9) + ' m, worst ' + acc[acc.length - 1] + ' m.');
  }
  var took = sortedNumbers(ok.map(function(r) { return r[TOOK]; }));
  lines.push('Time to answer: median ' + round1(quantile(took, 0.5) / 1000) +
             ' s, 90th percentile ' + round1(quantile(took, 0.9) / 1000) + ' s.');
  var gap = 0;
  for (var i = 1; i < ok.length; i++) gap = Math.max(gap, ok[i][T] - ok[i - 1][T]);
  if (ok.length > 1) lines.push('Longest gap between answers: ' + duration(gap / 1000) + '.');
  var age = sortedNumbers(ok.map(function(r) { return r[AGE]; }));
  lines.push('Age of the position when handed over: ' + (age.length
    ? 'reported in ' + percent(age.length, ok.length) + ' of answers, median ' +
      quantile(age, 0.5) + ' s, 90th percentile ' + quantile(age, 0.9) + ' s.'
    : 'never reported.'));
  var speed = ok.filter(function(r) { return typeof r[SPEED] === 'number'; });
  var heading = ok.filter(function(r) { return typeof r[HEADING] === 'number'; });
  lines.push('Speed reported in ' + percent(speed.length, ok.length) + ' of answers, heading in ' +
             percent(heading.length, ok.length) + '.');
  // Speed worked out from two positions in a row, against what the phone said.
  var diffs = [];
  var moved = [];
  for (var j = 1; j < ok.length; j++) {
    var a = ok[j - 1], b = ok[j];
    var dt = (b[T] - a[T]) / 1000;
    if (dt < 20 || dt > 130) continue;
    var worked = metersBetween(a, b) / dt;
    moved.push(round1(worked));
    if (typeof b[SPEED] === 'number') diffs.push(round1(Math.abs(worked - b[SPEED])));
  }
  moved = sortedNumbers(moved);
  if (moved.length) {
    lines.push('Speed worked out from two positions in a row: median ' + quantile(moved, 0.5) +
               ' m/s, 90th percentile ' + quantile(moved, 0.9) + ' m/s, over ' + moved.length + ' pairs.');
  }
  diffs = sortedNumbers(diffs);
  if (diffs.length) {
    lines.push('Difference between reported and worked out speed: median ' + quantile(diffs, 0.5) +
               ' m/s, 90th percentile ' + quantile(diffs, 0.9) + ' m/s, over ' + diffs.length + ' pairs.');
  }
  var refetches = ok.filter(function(r) { return r[REFETCH]; }).length;
  if (refetches) lines.push('Positions that made the departures load again: ' + refetches + '.');
  return lines;
}

// Lines for the top of the settings section.
function statusLines() {
  var m = meta();
  var on = isOn();
  if (!m.started) return ['Off. Nothing has been recorded.'];
  var n = held();
  var lines = [n + (n === 1 ? ' record' : ' records') + ' held, started ' + dateTime(m.started) + '. ' +
               (on ? 'On until ' + dateTime(m.started + MAX_DAYS * DAY_MS) + '.' : 'Off. ' + (m.ended || ''))];
  if (m.uploadedAt) {
    var waiting = Math.max(0, m.last * CHUNK_RECORDS + newestChunk().length - (m.sent || 0));
    lines.push('Uploaded ' + (m.uploaded || 0) + ', last at ' + dateTime(m.uploadedAt) + '; ' +
               waiting + ' waiting.');
  }
  if (m.uploadError) lines.push('Last upload failed: ' + m.uploadError + '.');
  return lines;
}

function summaryLines() {
  var m = meta();
  if (!m.started) return ['Nothing has been recorded.'];
  var records = allRecords();
  var lines = ['MBTA Pebble location log summary, made ' + dateTime(Date.now()) + '.']
    .concat(statusLines());
  if (!records.length) return lines;
  lines.push('Covers ' + duration((records[records.length - 1][T] - records[0][T]) / 1000) +
             ', ending ' + dateTime(records[records.length - 1][T]) + '.');
  if (m.lost) lines.push('The phone refused to store ' + m.lost + (m.lost === 1 ? ' record.' : ' records.'));
  if (m.sessions > 1) {
    lines.push('The phone side restarted ' + (m.sessions - 1) + (m.sessions === 2 ? ' time.' : ' times.'));
  }
  lines.push('');
  lines.push('All records');
  lines = lines.concat(figures(records));
  if (m.marks.length) {
    lines.push('');
    lines.push('Markers: ' + m.marks.length + '.');
    STATES.forEach(function(state) {
      var subset = records.filter(function(r) { return stateAt(m.marks, r[T]) === state; });
      if (!subset.length) return;
      lines.push('');
      lines.push(state);
      lines = lines.concat(figures(subset));
    });
  }
  return lines;
}

module.exports = {
  isOn: isOn, start: start, stop: stop, record: record, addMarks: addMarks, upload: upload,
  statusLines: statusLines, summaryLines: summaryLines, traceLines: traceLines,
  MARKS: MARKS, MAX_DAYS: MAX_DAYS, CSV_HEADER: CSV_HEADER
};
