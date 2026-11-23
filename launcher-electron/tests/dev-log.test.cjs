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
    const log = await createDevelopmentLog(root, 'manual', 'other', '0.0.1', 'linux');
    await log.write('inspection', 'ready');
    const text = await fs.readFile(path.join(root, 'data', 'logs', 'launcher-electron.log'), 'utf8');
    assert.match(text, /\d\d:\d\d:\d\d electron: development 0\.0\.1, platform linux/);
    assert.match(text, /electron: root manual \(other\)/);
    assert.match(text, /electron: inspection ready/);
    assert.equal(text.includes(root), false);
  } finally { await fs.rm(root, { recursive: true, force: true }); }
});

test('does not create log directories and resets an oversized development log', async () => {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'nx2saria-log-'));
  try {
    const absent = await createDevelopmentLog(root, 'default', 'NX2Saria', '0.0.1', 'linux');
    await absent.write('inspection', 'ready');
    assert.deepEqual(await fs.readdir(root), []);
    const directory = path.join(root, 'data', 'logs');
    await fs.mkdir(directory, { recursive: true });
    const file = path.join(directory, 'launcher-electron.log');
    await fs.writeFile(file, 'x'.repeat(1024 * 1024 + 1));
    await createDevelopmentLog(root, 'default', 'NX2Saria', '0.0.1', 'linux');
    const text = await fs.readFile(file, 'utf8');
    assert.ok(text.length < 1000);
    assert.match(text, /root default \(NX2Saria\)/);
  } finally { await fs.rm(root, { recursive: true, force: true }); }
});
