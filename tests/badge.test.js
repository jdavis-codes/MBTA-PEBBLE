const Module = require('module'); const path = require('path');
const orig = Module._load;
Module._load = function(req, ...r) { if (req === '@rebble/clay') return function(){}; if (req === 'message_keys') return {}; if (req === './config-page') return function(){}; return orig.call(this, req, ...r); };
global.localStorage = { getItem: () => null }; global.Pebble = { addEventListener(){} };
const m = require(path.join((process.argv[2] || require('path').join(__dirname, '..')), 'src/pkjs/index.js'));
const assert = require('assert');
// [route id, short name, headsign] -> what the watch is sent: a badge character, its code, the name.
[[['Red', '', 'Alewife'], '^RAlewife'],
 [['Blue', '', 'Wonderland'], '^BWonderland'],
 [['Green-B', '', 'Boston College'], '~BBoston Coll'],
 [['Mattapan', '', 'Ashmont'], '~MAshmont'],
 [['749', 'SL5', 'Nubian Station'], '#SL5 Nubian Stat'],
 [['741', '', 'South Station'], '#SL1 S Station'],
 [['66', '', 'Harvard Square'], '#66 Harvard Sq'],
 [['CR-Worcester', '', 'Worcester'], '=Worcester']
].forEach(([args, want]) => assert.strictEqual(m.badgedName(...args), want));
console.log('badges OK');
