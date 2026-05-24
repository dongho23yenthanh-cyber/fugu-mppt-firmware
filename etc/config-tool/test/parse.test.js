// Spec §2.5 — conf parser; §2.3 — `<not set>` / `0` / `""` distinction; §3 — serialize.
const test = require('node:test');
const assert = require('node:assert/strict');
const { loadEditor } = require('./_setup');

test('parseConf preserves comments, blanks and inline trailing comments', async () => {
  const { window } = await loadEditor();
  const src = '# a header\nkey=1  # inline\n\nfoo=bar\n';
  const m = window.parseConf(src);
  assert.equal(m.lines.length, 5);                    // 4 \n's → 5 split chunks
  assert.equal(m.lines[0].key, null);                 // comment line is structurally a non-key
  assert.equal(m.lines[1].key, 'key');
  assert.equal(m.lines[1].value, '1');
  assert.equal(m.lines[1].comment, 'inline');
  assert.equal(m.lines[2].key, null);                 // blank line preserved
  assert.equal(m.lines[3].key, 'foo');
  assert.equal(m.lines[4].key, null);                 // trailing empty from final \n
  assert.equal(window.serializeConf(m), src);         // round-trip
});

test('rewriteLine keeps prefix and trailing inline comment intact', async () => {
  const { window } = await loadEditor();
  assert.equal(window.rewriteLine('  k = 1  # note', 'k', '42'), '  k = 42  # note');
  assert.equal(window.rewriteLine('k=1\r',            'k', '2'),  'k=2\r');     // CRLF survivor
  assert.equal(window.rewriteLine('mismatch=x',       'k', '2'),  'k=2');       // fallback synth
});

test('setConfValue edits in place; new keys append before any trailing blank', async () => {
  const { window } = await loadEditor();
  const m = window.parseConf('a=1\nb=2\n');           // trailing blank from final \n
  window.setConfValue(m, 'a', '11');
  assert.equal(window.serializeConf(m).split('\n')[0], 'a=11');
  window.setConfValue(m, 'c', '3');
  // appended before the trailing empty line so the file keeps its trailing newline
  assert.equal(window.serializeConf(m), 'a=11\nb=2\nc=3\n');
});

test('removeConfKey drops the key line but keeps standalone comments', async () => {
  const { window } = await loadEditor();
  const m = window.parseConf('# header\na=1\nb=2\n');
  window.removeConfKey(m, 'a');
  assert.equal(window.serializeConf(m), '# header\nb=2\n');
});

test('serializeFile appends user-filled extras that the file omits (synthetic keys)', async () => {
  const { window } = await loadEditor();
  const f = {
    isConf: true,
    model: window.parseConf('a=1\n'),
    extras: { b: '2', c: '' },                        // c is empty → not written
    order: ['a','b','c'],
    original: { a: '1' },
  };
  assert.equal(window.serializeFile(f), 'a=1\nb=2\n');
});

test('empty string vs <not set>: trim()==="" hides extra; literal "" stays', async () => {
  const { window } = await loadEditor();
  // value '' on an existing key is kept by parser, written back as `password=`
  const m = window.parseConf('password=\n');
  assert.equal(m.lines[0].key, 'password');
  assert.equal(m.lines[0].value, '');
  assert.equal(window.serializeConf(m), 'password=\n');
});
