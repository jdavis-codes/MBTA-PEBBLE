// The location log: records, the seven day limit, markers, the summary and
// the trace. Positions are made up around Park Street station.
var Module = require('module'), path = require('path'), assert = require('assert');
var orig = Module._load;
Module._load = function(n) {
  if (n === '@rebble/clay') {
    // Enough of Clay for the settings page to open and close.
    var Clay = function(config) { this.config = config; };
    Clay.prototype.generateUrl = function() { return JSON.stringify(this.config); };
    Clay.prototype.getSettings = function(response) {
      localStorage.setItem('clay-settings', response);
      return JSON.parse(response);
    };
    return Clay;
  }
  if (n === 'message_keys') return new Proxy({}, { get: function(t, k) { return k; } });
  return orig.apply(this, arguments);
};
var store = {}, refuse = false;
global.localStorage = {
  getItem(k) { return k in store ? store[k] : null; },
  setItem(k, v) { if (refuse && /^loclog-c/.test(k)) throw new Error('full'); store[k] = String(v); },
  removeItem(k) { delete store[k]; }
};
var handlers = {}, sent = [], opened = [];
global.Pebble = { addEventListener(n, f) { handlers[n] = f; }, sendAppMessage(d, ok) { sent.push(d); if (ok) ok(); }, openURL(u) { opened.push(u); } };
var calls = [];
Object.defineProperty(globalThis, 'navigator', { value: { geolocation: { getCurrentPosition(ok, er) { calls.push([ok, er]); } } }, configurable: true, writable: true });
var realNow = Date.now, skew = 0; Date.now = function() { return realNow() + skew; };
var m = require(path.join(__dirname, '..', 'src/pkjs/index.js'));
var log = m.loclog;
var LAT = 42.35639, LON = -71.06242;   // Park Street

function answer(north, east, extra) {
  m.testLocation();
  var call = calls.pop();
  skew += 300;
  call[0]({ coords: Object.assign({ latitude: LAT + north / 111000, longitude: LON + east / 82000, accuracy: 10 }, extra),
            timestamp: Date.now() - 2000 });
}

// Off by default: nothing is kept.
answer(0, 0);
assert.strictEqual(log.isOn(), false);
assert.deepStrictEqual(log.summaryLines(), ['Nothing has been recorded.']);
assert.strictEqual(Object.keys(store).filter(k => /^loclog-c/.test(k)).length, 0);

log.start();
var began = Date.now();
answer(0, 0, { speed: -1, heading: -1 });            // the iPhone's "none"
skew += 60000; answer(60, 0, { speed: 1.2, heading: 0 });
skew += 60000; answer(120, 0, { speed: 1.0, heading: 0, accuracy: 65 });
skew += 60000; m.testLocation(); calls.pop()[1]({ code: 3 });       // timed out
skew += 60000; m.testLocation(); calls.pop(); skew += 25000; m.testLocation(); m.testLocation(); calls.pop()[1]({ code: 1 });  // no answer, then denied
assert.strictEqual(log.addMarks('3@' + (began + 90000) + ',9@5,junk,1@' + (began + 1000)), 2);

var summary = log.summaryLines().join('\n');
assert.ok(/Requests: 6 made, 3 answered, 0 failed, 1 timed out, 1 unanswered, 1 denied\./.test(summary), summary);
assert.ok(/Accuracy: median 10 m, 90th percentile 65 m, worst 65 m\./.test(summary), summary);
assert.ok(/Speed reported in 67% of answers, heading in 67%\./.test(summary), summary);
assert.ok(/reported in 100% of answers, median 2 s/.test(summary), summary);
assert.ok(/Speed worked out from two positions in a row: median 1 m\/s/.test(summary), summary);
assert.ok(/In a station\nRequests: 1 made/.test(summary), summary);
assert.ok(/On a train\nRequests: 4 made/.test(summary), summary);

// The trace is CSV under a header, records and markers in time order, with the real position.
var trace = log.traceLines(0);
assert.strictEqual(trace[0], log.CSV_HEADER);
var COLS = log.CSV_HEADER.split(',').length;
assert.strictEqual(trace.length, 1 + 6 + 2);
trace.forEach(function(line) { assert.strictEqual(line.split(',').length, COLS, line); });
assert.ok(/^\d+\.\d,[\d-]+ [\d:]+,,mark,,,,,,,,,,,,,Entered station$/.test(trace[2]), trace[2]);
assert.ok(/^\d+\.\d,[\d-]+ [\d:]+,30\d,ok,42\.35693\d,-71\.06242,10,,2,1\.2,0,,0,,,1,$/.test(trace[3]), trace[3]);
assert.ok(/,timeout,,,,,,,,,,,,1,$/.test(trace[6]), trace[6]);
assert.strictEqual(log.traceLines(0.005).length, 1 + 1);   // only the newest record

// A ring: the oldest records go, and a write the phone refuses is counted.
for (var i = 0; i < 3100; i++) log.record(Date.now(), 'timed out');
assert.strictEqual(log.traceLines(0).length - 1 - 2, 2906);   // 29 full chunks and the newest
assert.strictEqual(store['loclog-c0'], undefined);
refuse = true; log.record(Date.now(), null, { coords: { latitude: LAT, longitude: LON } }); refuse = false;
assert.ok(/refused to store 1 record\./.test(log.summaryLines().join('\n')));

// Seven days on, it has turned itself off and keeps what it has.
skew += 7 * 24 * 3600 * 1000;
assert.strictEqual(log.isOn(), false);
var held = log.traceLines(0).length;
answer(0, 0);
assert.strictEqual(log.traceLines(0).length, held);
assert.ok(/turned itself off after seven days/.test(log.statusLines().join(' ')));
log.start();
assert.strictEqual(log.traceLines(0).length, 1);

// The settings page: saving with the switch on starts the log, markers are
// kept, none of it is sent to the watch, and the trace is shown once.
sent = [];
function save(settings) {

  handlers.showConfiguration();
  handlers.webviewclosed({ response: JSON.stringify(settings) });
  return opened[opened.length - 1];
}
log.stop();
save({ LAYOUT: '3', LOG_ON: true });
assert.strictEqual(log.isOn(), true);
answer(0, 0);
var page = save({ LAYOUT: '3', LOG_ON: true, LOG_MARKS: '2@' + Date.now(), LOG_TRACE: '3' });
assert.ok(/Mark where you are/.test(page) && !/log-trace/.test(page));
assert.ok(sent.length && sent.every(d => !('LOG_ON' in d) && !('LOG_MARKS' in d) && !('LOG_TRACE' in d)), JSON.stringify(sent));
handlers.showConfiguration();
assert.ok(/log-trace/.test(opened.pop()), 'the trace is shown after it was asked for');
assert.strictEqual(log.traceLines(0).filter(l => /,mark,.*,On platform$/.test(l)).length, 1);
handlers.showConfiguration();
assert.ok(!/log-trace/.test(opened.pop()), 'the trace is shown once');
save({ LAYOUT: '3', LOG_ON: false });
assert.strictEqual(log.isOn(), false);
handlers.showConfiguration();
assert.ok(!/Mark where you are/.test(opened.pop()));

// Uploads: a new CSV file in the repository once ten records wait, and at
// once when the settings are saved. A failure is tried again with the next.
var puts = [], answerWith = 201;
global.XMLHttpRequest = function() {
  var x = this; x.headers = {};
  x.open = function(method, url) { x.method = method; x.url = url; };
  x.setRequestHeader = function(k, v) { x.headers[k] = v; };
  x.send = function(body) { if (x.method !== 'PUT') return; x.body = JSON.parse(body); x.status = answerWith; puts.push(x); if (answerWith) x.onload(); };
};
function text(x) { return Buffer.from(x.body.content, 'base64').toString(); }
var cfg = { LAYOUT: '3', LOG_ON: true, LOG_REPO: 'https://github.com/example/commute-data/', LOG_TOKEN: 'tok' };
save(cfg);
assert.strictEqual(puts.length, 0, 'nothing to send yet');
for (var k = 0; k < 9; k++) { skew += 60000; answer(k * 10, 0); }
assert.strictEqual(puts.length, 0, 'nine records wait');
skew += 60000; answer(90, 0);
assert.strictEqual(puts.length, 1);
assert.ok(/^https:\/\/api\.github\.com\/repos\/example\/commute-data\/contents\/loclog\/\d{4}-\d\d-\d\d\/\d{6}-0\.csv$/.test(puts[0].url), puts[0].url);
assert.strictEqual(puts[0].method, 'PUT');
assert.strictEqual(puts[0].headers.Authorization, 'Bearer tok');
var lines = text(puts[0]).trim().split('\n');
assert.strictEqual(lines[0], log.CSV_HEADER);
assert.strictEqual(lines.length, 1 + 10);
// A refused upload keeps its records waiting, and says why in the settings.
answerWith = 401;
for (k = 0; k < 10; k++) { skew += 60000; answer(0, k * 10); }
assert.strictEqual(puts.length, 2);
assert.ok(/Last upload failed: GitHub did not accept the token/.test(log.statusLines().join(' ')));
answerWith = 201;
save(Object.assign({ LOG_MARKS: '3@' + Date.now() }, cfg));
assert.strictEqual(puts.length, 3);
lines = text(puts[2]).trim().split('\n');
assert.strictEqual(lines.length, 1 + 10 + 1, 'the ten that waited and the marker');
assert.ok(/-10\.csv$/.test(puts[2].url) && /,Boarded$/.test(lines[lines.length - 1]));
assert.ok(/Uploaded 20, last at .*; 0 waiting\./.test(log.statusLines().join(' ')), log.statusLines().join(' '));
// An upload the phone never answers is given up on by the clock.
answerWith = 0;
for (k = 0; k < 10; k++) { skew += 60000; answer(0, 0); }
assert.strictEqual(puts.length, 4);
answerWith = 201; skew += 60000; answer(0, 0);
assert.strictEqual(puts.length, 5);
assert.strictEqual(text(puts[4]).trim().split('\n').length, 1 + 11);
// The token and the repository stay on the phone.
assert.ok(sent.every(d => !('LOG_REPO' in d) && !('LOG_TOKEN' in d)));
console.log('location log OK');
process.exit(0);
