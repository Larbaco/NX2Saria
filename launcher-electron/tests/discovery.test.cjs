const { test } = require('node:test');
const assert = require('node:assert/strict');
const { defaultRoots } = require('../dist/discovery.js');
test('uses XDG data home and falls back for relative XDG paths', () => {
  assert.deepEqual(defaultRoots('linux', '/home/ana', { XDG_DATA_HOME: '/data/ana' }), ['/data/ana/nx2saria', '/data/ana/nx2navi']);
  assert.deepEqual(defaultRoots('linux', '/home/ana', { XDG_DATA_HOME: 'relative' }), ['/home/ana/.local/share/nx2saria', '/home/ana/.local/share/nx2navi']);
});
test('prefers current Windows installation to legacy installation', () => {
  assert.deepEqual(defaultRoots('win32', 'C:\\Users\\Ana', { LOCALAPPDATA: 'C:\\Users\\Ana\\AppData\\Local' }), ['C:\\Users\\Ana\\AppData\\Local\\Programs\\NX2Saria', 'C:\\Users\\Ana\\AppData\\Local\\Programs\\NX2navi']);
});
test('selects the first existing installation and falls back to the legacy root', async () => {
  const fs = require('node:fs/promises');
  const os = require('node:os');
  const path = require('node:path');
  const { findDefaultRoot } = require('../dist/discovery.js');
  const base = await fs.mkdtemp(path.join(os.tmpdir(), 'nx2saria-discovery-'));
  const current = path.join(base, 'nx2saria');
  const legacy = path.join(base, 'nx2navi');
  async function make(root) {
    await fs.mkdir(path.join(root, 'app'), { recursive: true });
    await fs.mkdir(path.join(root, 'data'), { recursive: true });
    await fs.writeFile(path.join(root, 'app', 'current.txt'), 'v1');
    await fs.writeFile(path.join(root, 'data', 'launcher.json'), '{}');
  }
  try {
    assert.equal(findDefaultRoot([current, legacy]), null);
    await make(legacy);
    await fs.rm(path.join(legacy, 'data', 'launcher.json'));
    assert.equal(findDefaultRoot([current, legacy]), legacy);
    await make(current);
    assert.equal(findDefaultRoot([current, legacy]), current);
  } finally { await fs.rm(base, { recursive: true, force: true }); }
});
