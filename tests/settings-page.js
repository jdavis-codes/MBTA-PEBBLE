// Builds the real settings page and checks, in Chromium, that it shows only the
// options each layout uses. Usage: node tests/settings-page.js [ROOT] [SCREENSHOT_PREFIX]
const path = require('path'); const root = (process.argv[2] || require('path').join(__dirname, '..')); const out = process.argv[3];
const store = { 'clay-settings': '{}' };
global.localStorage = { getItem: k => (k in store ? store[k] : null), setItem: (k, v) => { store[k] = String(v); }, removeItem: k => { delete store[k]; } };
global.Pebble = { addEventListener(){}, getActiveWatchInfo: () => ({ platform: 'flint', model: 'x', language: 'en_US', firmware: { major: 4, minor: 44, patch: 0, suffix: '' } }), getAccountToken: () => 'a', getWatchToken: () => 'w', openURL(){}, platform: 'ios' };
const Module = require('module'); const origLoad = Module._load;
const mk = {}; let next = 10000;
require(path.join(root, 'package.json')).pebble.messageKeys.forEach(k => { const m = /^(\w+)(?:\[(\d+)\])?$/.exec(k); mk[m[1]] = next; next += m[2] ? +m[2] : 1; });
Module._load = function(req, ...r) { return req === 'message_keys' ? mk : origLoad.call(this, req, ...r); };
const Clay = require(path.join(root, 'node_modules/@rebble/clay/dist/js/index.js'));
const config = require(path.join(root, 'src/pkjs/config.js'));
const page = require(path.join(root, 'src/pkjs/config-page.js'));
// A location log that is on, with a summary and a trace to show.
const log = { on: true, status: ['2 records held.'], summary: ['Summary line 1', 'Requests: 2 made, 2 answered.'],
              trace: ['t,time,took_ms,result', '1795436045.0,2026-11-23 08:14:05,300,ok,42.356390,-71.062420,10,,2,1.2,0,3,0,place-pktrm,40,1,', '1795436105.0,2026-11-23 08:15:05,,mark,,,,,,,,,,,,,Boarded'] };
const clay = new Clay(config.build({}, ['usage'], ['location'], log), page, { autoHandleEvents: false });
(async () => {
  const { chromium } = require('playwright');
  const browser = await chromium.launch();
  const pg = await (await browser.newContext({ viewport: { width: 390, height: 900 }, deviceScaleFactor: 2 })).newPage();
  const errors = []; pg.on('pageerror', e => errors.push(String(e)));
  await pg.goto(clay.generateUrl()); await pg.waitForTimeout(5000);
  const layout = pg.locator('select').first();
  // What each layout must show (true) or hide (false) on the settings page.
  const want = {
    '0': { 'Trains shown per station': true, 'Departures shown on the board': false, 'Nearby radius (meters)': false, 'Line': true, 'Show service alerts': true },
    '1': { 'Trains shown per station': false, 'Departures shown on the board': true, 'Show each departure as': true, 'Nearby radius (meters)': false, 'Line': true },
    '2': { 'Trains shown per station': false, 'Departures shown on the board': false, 'Line': true, 'Show service alerts': false },
    '3': { 'Nearby radius (meters)': true, 'Departures shown on the board': true, 'Show each departure as': true, 'Line': false, 'Trains shown per station': false }
  };
  const assert = require('assert');
  for (const v of ['0', '1', '2', '3']) {
    await layout.selectOption(v); await pg.waitForTimeout(300);
    const labels = await pg.locator('.component .label, .component-heading').evaluateAll(els => els.filter(e => e.offsetParent).map(e => e.innerText.split('\n')[0].trim()).filter(Boolean));
    for (const [label, shown] of Object.entries(want[v])) {
      assert.strictEqual(labels.includes(label), shown, 'layout ' + v + ': "' + label + '" should be ' + (shown ? 'shown' : 'hidden'));
    }
    if (out) await pg.screenshot({ path: out + v + '.png' });
  }
  // The location log: the boxes hold their text, and markers wait in the hidden field.
  assert.strictEqual(await pg.locator('#log-summary textarea, textarea').first().inputValue(), log.summary.join('\n'));
  assert.strictEqual(await pg.locator('textarea').nth(1).inputValue(), log.trace.join('\n'));
  const before = Date.now();
  await pg.getByRole('button', { name: 'Boarded' }).click();
  await pg.getByRole('button', { name: 'Got off' }).click();
  const marks = await pg.evaluate(() => Array.from(document.querySelectorAll('input')).map(i => i.value).find(v => /@/.test(v)));
  const m = /^3@(\d+),4@(\d+)$/.exec(marks);
  assert.ok(m && +m[1] >= before && +m[2] >= +m[1], 'markers: ' + marks);
  assert.ok(/Marked: Boarded at [\d:]+, Got off at [\d:]+\. Save to keep them\./.test(await pg.locator('body').innerText()));
  assert.strictEqual(await pg.getByText('Markers', { exact: true }).isVisible(), false);
  if (out) await pg.screenshot({ path: out + 'log.png', fullPage: true });
  assert.deepStrictEqual(errors, []);
  console.log('settings page OK');
  await browser.close(); process.exit(0);
})();
