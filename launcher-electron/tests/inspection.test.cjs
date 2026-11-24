const { test } = require('node:test');
const assert = require('node:assert/strict');
const { inspect } = require('../dist/inspection.js');
test('reports a worker failure without exposing its output', async () => {
  const result = await inspect('/unused', '/nonexistent/nx2saria-python', '/unused');
  assert.deepEqual(result, { status: 'inspection_error', reason: 'worker_failed' });
});
test('inspects a temporary root without changing it', async () => {
  const fs = require('node:fs/promises');
  const os = require('node:os');
  const path = require('node:path');
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'nx2saria-inspect-'));
  try {
    const result = await inspect(root, process.platform === 'win32' ? 'python' : 'python3', path.resolve(__dirname, '../..'));
    assert.equal(result.status, 'not_configured');
    assert.deepEqual(await fs.readdir(root), []);
  } finally { await fs.rm(root, { recursive: true, force: true }); }
});
test('preserves the job marker signal from the Python inspection', async () => {
  const fs = require('node:fs/promises');
  const os = require('node:os');
  const path = require('node:path');
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'nx2saria-marker-'));
  try {
    await fs.mkdir(path.join(root, 'data'));
    await fs.writeFile(path.join(root, 'data', 'job-running.json'), '{}');
    const result = await inspect(root, process.platform === 'win32' ? 'python' : 'python3', path.resolve(__dirname, '../..'));
    assert.equal(result.job_marker_present, true);
  } finally { await fs.rm(root, { recursive: true, force: true }); }
});
test('forwards confirmed running state without inventing a negative result', async () => {
  const fs = require('node:fs/promises');
  const os = require('node:os');
  const path = require('node:path');
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'nx2saria-status-'));
  try {
    const result = await inspect(root, process.platform === 'win32' ? 'python' : 'python3', path.resolve(__dirname, '../..'));
    assert.equal(result.game_running, null);
  } finally { await fs.rm(root, { recursive: true, force: true }); }
});
test('forwards hide preference without leaking other settings', async () => {
  const fs = require('node:fs/promises');
  const os = require('node:os');
  const path = require('node:path');
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'nx2saria-preference-'));
  try {
    await fs.mkdir(path.join(root, 'data'));
    await fs.writeFile(path.join(root, 'data', 'launcher.json'), JSON.stringify({ hide_while_playing: false, github_token: 'secret' }));
    const result = await inspect(root, process.platform === 'win32' ? 'python' : 'python3', path.resolve(__dirname, '../..'));
    assert.equal(result.hide_while_playing, false);
    assert.equal(JSON.stringify(result).includes('secret'), false);
  } finally { await fs.rm(root, { recursive: true, force: true }); }
});
test('validates persistent worker inspection and removes unrelated fields', () => {
  const { parseInspection } = require('../dist/inspection.js');
  const valid = { status: 'ready', root: '/synthetic', game_dir: '/synthetic/game', job_marker_present: false,
    game_running: null, hide_while_playing: true, secret: 'private' };
  assert.equal(parseInspection(valid).status, 'ready');
  assert.equal('secret' in parseInspection(valid), false);
  assert.throws(() => parseInspection({ ...valid, game_running: false }));
  assert.throws(() => parseInspection({ ...valid, status: 'invented' }));
});
