const Module = require('module'); const path = require('path'); const assert = require('assert');
const orig = Module._load;
Module._load = function(req, ...r) {
  if (req === '@rebble/clay') return function(){};
  if (req === 'message_keys') return {};
  if (req === './config-page') return function(){};
  return orig.call(this, req, ...r);
};
global.localStorage = { getItem: () => null }; global.Pebble = { addEventListener(){} };
const m = require(path.join((process.argv[2] || require('path').join(__dirname, '..')), 'src/pkjs/index.js'));
const fmt = new Intl.DateTimeFormat('en-US', { timeZone: 'America/New_York', timeZoneName: 'shortOffset' });
// offset matches Intl for every hour of 2026-2028
for (let t = Date.UTC(2026,0,1); t < Date.UTC(2029,0,1); t += 3600000) {
  const want = +/GMT([+-]\d+)/.exec(fmt.format(t))[1];
  assert.equal(m.bostonOffsetHours(t), want, new Date(t).toISOString());
}
const w = iso => JSON.stringify(m.serviceWindows(Date.parse(iso)));
console.log('19:33 EST ', w('2026-11-18T00:33:00Z'));
console.log('01:30 EST ', w('2026-11-18T06:30:00Z'));
console.log('03:25 EST ', w('2026-11-18T08:25:00Z'));
console.log('00:10 EDT ', w('2026-07-10T04:10:00Z'));
console.log('tz OK');
