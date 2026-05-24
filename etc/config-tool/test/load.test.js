// Spec §1 (loading clears prior state) and §2.1 (one tab per known file; synthetic
// tabs for files not present in the source; fixed TAB_ORDER for known files).
const test = require('node:test');
const assert = require('node:assert/strict');
const { loadEditor } = require('./_setup');

function mkFiles(map){
  const enc = new TextEncoder(), out = {};
  for (const k in map) out[k] = enc.encode(map[k]);
  return out;
}
function tabLabels(window){
  return [...window.document.querySelectorAll('#tabs .tab')].map(t => t.firstChild.textContent.trim());
}

test('load() reveals the app, sets the source label, and shows a tab per known file', async () => {
  const { window } = await loadEditor();
  window.load(mkFiles({ 'config/conf/board.conf': 'mcu=esp32s3\n' }), 'demo');

  assert.equal(window.document.getElementById('app').style.display, 'block');
  assert.equal(window.document.getElementById('drop').style.display, 'none');
  assert.equal(window.document.getElementById('srcname').textContent, 'demo');

  // every key in FILE_KEYS should produce a tab
  const expected = new Set(Object.keys(window._tables.FILE_KEYS).map(b => b.replace(/\.conf$/, '')));
  const got      = new Set(tabLabels(window).map(s => s.replace(/\.conf$/, '')));
  for (const want of expected) assert.ok(got.has(want), 'missing tab for ' + want);
});

test('tab order honours TAB_ORDER (board, sensor, limits, …)', async () => {
  const { window } = await loadEditor();
  window.load(mkFiles({
    'config/conf/limits.conf': '',
    'config/conf/board.conf':  '',
    'config/conf/sensor.conf': '',
  }), 'demo');
  const labels = tabLabels(window);
  // first three in fixed order regardless of input order
  assert.deepEqual(labels.slice(0, 3), ['board.conf', 'sensor.conf', 'limits.conf']);
});

test('synthetic tabs are pushed to the right of all real (loaded) tabs', async () => {
  const { window } = await loadEditor();
  // load three files that are NOT first in TAB_ORDER, so a naive sort would
  // interleave coil/converter (synthetic) between them.
  window.load(mkFiles({
    'config/conf/board.conf':   '',
    'config/conf/charger.conf': '',
    'config/conf/wifi.conf':    '',
  }), 'demo');
  const tabs = [...window.document.querySelectorAll('#tabs .tab')];
  const realLabels = tabs.filter(t => !t.classList.contains('new'))
                         .map(t => t.firstChild.textContent.trim());
  const synthIndex = tabs.findIndex(t => t.classList.contains('new'));
  const lastRealIndex = tabs.map((t,i) => t.classList.contains('new') ? -1 : i)
                            .filter(i => i >= 0).pop();
  // every real tab comes before any synthetic tab
  assert.ok(synthIndex > lastRealIndex, 'synthetic tabs should follow all real tabs');
  // real tabs retained TAB_ORDER ranking
  assert.deepEqual(realLabels, ['board.conf', 'charger.conf', 'wifi.conf']);
});

test('synthetic tabs (files absent from the source) get the .new class', async () => {
  const { window } = await loadEditor();
  window.load(mkFiles({ 'config/conf/board.conf': 'mcu=esp32s3\n' }), 'demo');
  const tabs = [...window.document.querySelectorAll('#tabs .tab')];
  const board = tabs.find(t => t.firstChild.textContent.trim() === 'board.conf');
  const wifi  = tabs.find(t => t.firstChild.textContent.trim() === 'wifi.conf');
  assert.ok(!board.classList.contains('new'),  'real tab should not be .new');
  assert.ok(wifi.classList.contains('new'),    'synthetic tab should be .new');
});

test('untouched synthetic files are dropped on download', async () => {
  const { window } = await loadEditor();
  window.load(mkFiles({ 'config/conf/board.conf': 'mcu=esp32s3\n' }), 'demo');
  // pluck filtered file map the way download() does — drop synthetic & untouched
  const out = {};
  for (const p of Object.keys(window._state.files)) {
    const f = window._state.files[p];
    if (f.isNew && !window.isFileDirty(f)) continue;
    out[p] = p;
  }
  assert.deepEqual(Object.keys(out), ['config/conf/board.conf']);
});

test('loading a second source clears the first', async () => {
  const { window } = await loadEditor();
  window.load(mkFiles({ 'a/conf/board.conf': 'mcu=esp32s3\n' }), 'first');
  window.load(mkFiles({ 'b/conf/board.conf': 'mcu=esp32\n'   }), 'second');
  assert.equal(window.document.getElementById('srcname').textContent, 'second');
  // confPaths from "first" must be gone
  for (const p of Object.keys(window._state.files)) assert.ok(!p.startsWith('a/'), p);
});
