const { test } = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');
const { runtimePaths } = require('../dist/runtime-paths.js');
test('packaged runtime uses its own Python and backend', () => {
  const r = runtimePaths(true, '/portable/resources', '/ignored', {}, 'win32');
  assert.equal(r.source, '/portable/resources');
  assert.equal(r.python, path.join('/portable/resources', 'python', 'python.exe'));
});
test('development continues to use an explicit Python override', () => {
  const r = runtimePaths(false, '/ignored', '/repo/launcher-electron/dist', { NX2SARIA_PYTHON: '/python' }, 'linux');
  assert.equal(r.source, '/repo');
  assert.equal(r.python, '/python');
});
