// Spec §1 — built-in ZIP reader (stored + deflate) and writer (stored).
const test = require('node:test');
const assert = require('node:assert/strict');
const { loadEditor } = require('./_setup');

test('crc32 matches the well-known value for "123456789"', async () => {
  const { window } = await loadEditor();
  const data = new TextEncoder().encode('123456789');
  // Canonical CRC-32 (ISO 3309) of "123456789" is 0xCBF43926
  assert.equal(window.crc32(data), 0xcbf43926);
});

test('writeZip + readZip round-trip preserves file names and bytes', async () => {
  const { window } = await loadEditor();
  const enc = new TextEncoder();
  const input = {
    'config/conf/board.conf'  : enc.encode('mcu=esp32s3\n'),
    'config/conf/limits.conf' : enc.encode('vin_max=80\nvout_max=60\n'),
    'config/raw.bin'          : new Uint8Array([0,1,2,3,4,5]),
  };
  const blob = window.writeZip(input);
  const buf  = await blob.arrayBuffer();
  const out  = await window.readZip(buf);

  assert.deepEqual(Object.keys(out).sort(), Object.keys(input).sort());
  for (const name of Object.keys(input)) {
    assert.deepEqual(Array.from(out[name]), Array.from(input[name]),
      'bytes for ' + name + ' should round-trip');
  }
});

test('readZip rejects non-zip input', async () => {
  const { window } = await loadEditor();
  await assert.rejects(window.readZip(new ArrayBuffer(16)), /not a zip/);
});
