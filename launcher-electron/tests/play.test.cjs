const { test } = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');
const os = require('node:os');
const fs = require('node:fs/promises');
const { startPlay } = require('../dist/play.js');

test('uses the installed version and interpreter with arguments kept separate', { skip: process.platform === 'win32' }, async () => {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'nx2saria play '));
  try {
    const python = path.join(root, 'python', 'bin', 'python3');
    const script = path.join(root, 'app', 'v1', 'launcher', 'start.pyw');
    const output = path.join(root, 'arguments.json');
    await fs.mkdir(path.dirname(python), { recursive: true });
    await fs.mkdir(path.dirname(script), { recursive: true });
    await fs.writeFile(path.join(root, 'app', 'current.txt'), 'v1');
    await fs.writeFile(script, '');
    await fs.writeFile(python, `#!${process.execPath}\nrequire('node:fs').writeFileSync(${JSON.stringify(output)}, JSON.stringify(process.argv.slice(2)));\n`, { mode: 0o755 });
    assert.equal((await startPlay(root).done).code, 0);
    assert.deepEqual(JSON.parse(await fs.readFile(output, 'utf8')), ['-I', script, '--root', root, '--play']);
  } finally { await fs.rm(root, { recursive: true, force: true }); }
});

test('refuses an incomplete installation', async () => {
  assert.deepEqual(await startPlay('/nonexistent/nx2saria').done, { error: 'installation_incomplete' });
});

test('refuses a version path that escapes the installation', async () => {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'nx2saria-invalid-'));
  try {
    await fs.mkdir(path.join(root, 'app'));
    await fs.writeFile(path.join(root, 'app', 'current.txt'), '../outside');
    assert.deepEqual(await startPlay(root).done, { error: 'installation_incomplete' });
  } finally { await fs.rm(root, { recursive: true, force: true }); }
});

test('reports a launch error before saying the process started', { skip: process.platform === 'win32' }, async () => {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'nx2saria-noexec-'));
  try {
    const python = path.join(root, 'python', 'bin', 'python3');
    const script = path.join(root, 'app', 'v1', 'launcher', 'start.pyw');
    await fs.mkdir(path.dirname(python), { recursive: true });
    await fs.mkdir(path.dirname(script), { recursive: true });
    await fs.writeFile(path.join(root, 'app', 'current.txt'), 'v1');
    await fs.writeFile(script, '');
    await fs.writeFile(python, 'not executable', { mode: 0o600 });
    const session = startPlay(root);
    assert.deepEqual(await session.started, { error: 'start_failed' });
    assert.deepEqual(await session.done, { error: 'start_failed' });
  } finally { await fs.rm(root, { recursive: true, force: true }); }
});
