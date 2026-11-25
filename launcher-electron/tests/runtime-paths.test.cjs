const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { runtimePaths } = require('../dist/runtime-paths.js');

test('packaged worker uses the selected installation interpreter and the current app tools', t => {
  const base = fs.mkdtempSync(path.join(os.tmpdir(), 'nx2-runtime-'));
  t.after(() => fs.rmSync(base, { recursive: true, force: true }));
  const root = path.join(base, 'installation');
  const resources = path.join(base, 'portable');
  const embedded = path.join(resources, 'python', 'python.exe');
  const python = path.join(root, 'python', 'python.exe');
  const active = path.join(root, 'app', '0.25.0');
  for (const file of [embedded, python, path.join(active, 'VERSION'), path.join(active, 'tools', 'make_game.py')]) {
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, '');
  }
  const resolve = () => runtimePaths(true, resources, '/unused', { NX2SARIA_PYTHON: embedded }, 'win32', root);
  assert.throws(resolve, { code: 'installation_required' });
  fs.writeFileSync(path.join(root, 'app', 'current.txt'), '0.25.0\n');
  assert.deepEqual(resolve(), { source: resources, python, installed: { version: '0.25.0', app: active } });
  fs.unlinkSync(python);
  assert.throws(resolve, { code: 'runtime_incompatible' });
});

test('development uses an explicit Python override', () => {
  const checkout = path.resolve('repo');
  const r = runtimePaths(false, '/ignored', path.join(checkout, 'launcher-electron', 'dist'), { NX2SARIA_PYTHON: '/python' }, 'linux', '');
  assert.equal(r.source, checkout);
  assert.equal(r.python, '/python');
});
