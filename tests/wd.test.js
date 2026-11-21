// Harness: first round of XHRs hangs; a request 31s later must start a new fetch and deliver.
const Module = require('module'); const path = require('path');
const root = (process.argv[2] || require('path').join(__dirname, '..'));
const orig = Module._load;
Module._load = function(req, parent, ...r) {
  if (req === '@rebble/clay') return function(){ this.generateUrl=()=>''; this.getSettings=()=>({}); };
  if (req === 'message_keys') return { TIMES: 100, DEST: 200, MODE: 300, ROUTE: 400, STOP: 500, DIR: 600, FETCH_FAILED: 700, FLAGS: 800, DIAG: 990, ALERT: 900, ALERT_ON: 950, BIKE: 960, BIKES: 970, BIKE_FAILED: 980, LAYOUT: 985, TO: 1000, LABEL: 1100, WARN_ON: 1200, WARN_START: 1300, WARN_END: 1400, DEPARTURES: 1500, REQUEST: 1 };
  if (req === './config-page') return function(){};
  return orig.call(this, req, parent, ...r);
};
let now = 1e12; Date.now = () => now;
const xhrs = []; let hang = true;
global.XMLHttpRequest = function(){ this.open=()=>{}; this.setRequestHeader=()=>{};
  this.send = () => { xhrs.push(this); if (!hang) { this.status=200; this.responseText='{"data":[]}'; this.onload(); } }; };
const SAVED = JSON.stringify({'ROUTE[0]':'CR-Worcester','STOP[0]':'place-sstat','DIR[0]':'0','ROUTE[1]':'Red','STOP[1]':'place-pktrm','DIR[1]':'0','ROUTE[2]':'66','STOP[2]':'1148','DIR[2]':'0'}); global.localStorage = { getItem: k => k === 'clay-settings' ? SAVED : null, setItem(){} };
const handlers = {}; const sent = [];
global.Pebble = { addEventListener: (n,f)=>handlers[n]=f, sendAppMessage: (d,ok)=>{ sent.push(d); ok(); } };
require(path.join(root, 'src/pkjs/index.js'));
const assert = require('assert');
handlers.ready();                 assert.equal(xhrs.length, 3);  // first fetch hangs
now += 10000; handlers.appmessage(); assert.equal(xhrs.length, 3, 'within 30s: still waiting');
now += 21000; hang = false; handlers.appmessage();
assert.equal(xhrs.length, 6, 'after 30s: new fetch started'); assert.equal(sent.length, 1, 'data sent');
// late replies from the abandoned fetch are ignored
xhrs.slice(0,3).forEach(x => { x.status=200; x.responseText='{"data":[]}'; x.onload(); });
assert.equal(sent.length, 1, 'late replies ignored');
now += 120000; handlers.appmessage(); assert.equal(sent.length, 2, 'normal refresh still works');
console.log('watchdog OK');
