const assert = require('node:assert/strict');
const test = require('node:test');
const { readReportPreview } = require('../dist/reports-protocol');

test('decodes the exact report preview when a UTF-8 character crosses frames', async () => {
  const payload = 'x'.repeat(16 * 1024 - 1) + '🗒\n';
  const bytes = Buffer.from(payload, 'utf8');
  const preview = await readReportPreview(async offset => ({
    size: bytes.length, chunk: bytes.subarray(offset, offset + 16 * 1024).toString('base64'), example: false,
  }));
  assert.deepEqual(preview, { text: payload, example: false });
});
