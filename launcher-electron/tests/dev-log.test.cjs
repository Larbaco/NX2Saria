const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const { createDevelopmentLog } = require('../dist/dev-log.js');

test('records approved events without exposing the selected path', async () => {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'nx2saria-log-private-'));
  try {
    await fs.mkdir(path.join(root, 'data', 'logs'), { recursive: true });
    const log = createDevelopmentLog(root, '0.0.1', 'linux', 'aabbcc12', new Date());
    log.write('root', 'picked origin=manual brand=other');
    log.write('inspection', 'result=ready');
    log.write('window', root);
    const text = await fs.readFile(path.join(root, 'data', 'logs', 'launcher-electron.log'), 'utf8');
    assert.match(text, /\d\d:\d\d:\d\d electron: start version=0\.0\.1 build=aabbcc12 platform=linux instance=\d+/);
    assert.match(text, /electron: root picked origin=manual brand=other/);
    assert.match(text, /electron: inspection result=ready/);
    assert.match(text, /electron: window invalid /);
    assert.equal(text.includes(root), false);
  } finally { await fs.rm(root, { recursive: true, force: true }); }
});

test('does not create log directories and resets an oversized development log', async () => {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'nx2saria-log-'));
  try {
    assert.equal(createDevelopmentLog(root, '0.0.1', 'linux', 'aabbcc12', new Date()), null);
    assert.deepEqual(await fs.readdir(root), []);
    const directory = path.join(root, 'data', 'logs');
    await fs.mkdir(directory, { recursive: true });
    const file = path.join(directory, 'launcher-electron.log');
    await fs.writeFile(file, 'x'.repeat(1024 * 1024 + 1));
    createDevelopmentLog(root, '0.0.1', 'linux', 'aabbcc12', new Date());
    const text = await fs.readFile(file, 'utf8');
    assert.ok(text.length < 1000);
    assert.match(text, /electron: start version=0\.0\.1 build=aabbcc12/);
  } finally { await fs.rm(root, { recursive: true, force: true }); }
});
