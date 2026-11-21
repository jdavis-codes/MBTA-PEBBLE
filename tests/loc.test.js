var Module = require('module'), path = require('path');
var orig = Module._load;
Module._load = function(n) {
  if (n === '@rebble/clay') return function() {};
  if (n === 'message_keys') return new Proxy({}, { get: function(t, k) { return k; } });
  return orig.apply(this, arguments);
};
global.localStorage = { d: {}, getItem(k) { return this.d[k] || null; }, setItem(k, v) { this.d[k] = v; } };
global.Pebble = { addEventListener() {}, sendAppMessage() {} };
var calls = [];
Object.defineProperty(globalThis, "navigator", { value: { geolocation: { getCurrentPosition(ok, er) { calls.push([ok, er]); } } }, configurable: true, writable: true });
var realNow = Date.now, skew = 0; Date.now = function() { return realNow() + skew; };
var m = require(path.join(__dirname, '..', 'src/pkjs/index.js'));
var assert = require('assert');
m.testLocation(); m.testLocation(); m.testLocation();
assert.strictEqual(calls.length, 1, 'single flight');
console.log(m.locationLines()[0]);
skew += 25000; m.testLocation();            // hung: given up on by the clock
console.log(m.locationLines().join(' | '));
m.testLocation(); assert.strictEqual(calls.length, 2);
calls[0][0]({ coords: { latitude: 1, longitude: 2, accuracy: 5 } });   // late answer to the old one is ignored
calls[1][0]({ coords: { latitude: 42.3561, longitude: -71.0623, accuracy: 5 }, timestamp: Date.now() });
console.log(m.locationLines().join(' | '));
global.navigator.geolocation.getCurrentPosition = function() { throw new Error('boom'); };
m.testLocation();
console.log(m.locationLines().join(' | '));
process.exit(0);
