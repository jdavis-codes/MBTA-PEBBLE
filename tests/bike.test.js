const Module = require('module'); const path = require('path'); const assert = require('assert');
const orig = Module._load;
Module._load = function(req, ...r) { if (req === '@rebble/clay') return function(){}; if (req === 'message_keys') return {}; if (req === './config-page') return function(){}; return orig.call(this, req, ...r); };
global.localStorage = { getItem: () => null }; global.Pebble = { addEventListener(){} };
const m = require(path.join((process.argv[2] || require('path').join(__dirname, '..')), 'src/pkjs/index.js'));
const st = (b, d, e, extra) => Object.assign({ is_installed: 1, is_renting: 1, num_bikes_available: b, num_docks_available: d, num_ebikes_available: e || 0 }, extra);
assert.equal(m.bikeLetter(st(0, 15)), 'a0');            // empty
assert.equal(m.bikeLetter(st(15, 0)), 'k0');            // full -> solid square
assert.equal(m.bikeLetter(st(8, 8)), 'e0');             // half
assert.equal(m.bikeLetter(st(1, 30)), 'b0');            // one bike is never "empty"
assert.equal(m.bikeLetter(st(18, 1)), 'j0');            // one free -> hollow square
assert.equal(m.bikeLetter(st(18, 2)), 'h0');            // two free is a pie again
assert.equal(m.bikeLetter(st(14, 0, 3, { num_docks_disabled: 2 })), 'k3');  // broken docks don't count as free
assert.equal(m.bikeLetter(st(8, 8, 2)), 'e2');          // e-bikes counted
assert.equal(m.bikeLetter(st(5, 5, 1, { is_renting: 0 })), 'l0');  // a dock that is not renting is crossed out
assert.equal(m.bikeLetter(undefined), 'l0');
assert.equal(m.bikeLetter(st(12, 3, 11)), 'g8');  // e-bike dots are capped
console.log('bikes OK');
