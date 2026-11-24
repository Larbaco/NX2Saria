const test = require('node:test');
const assert = require('node:assert/strict');
const { parseHandshake } = require('../dist/worker-protocol.js');

test('accepts an exact supported protocol and validated capabilities', () => {
  assert.deepEqual(parseHandshake({ protocol: 1, version: '0.25.0', lock: 'held', capabilities: ['inspect'], update_waiting: false }),
    { protocol: 1, version: '0.25.0', lock: 'held', capabilities: ['inspect'], update_waiting: false });
  assert.equal(parseHandshake({ protocol: 1, version: '0.25.0', lock: 'unavailable', capabilities: ['inspect'], update_waiting: true }).lock, 'unavailable');
});

test('rejects mismatched or malformed worker handshakes', () => {
  for (const value of [null, {}, { protocol: 2, version: '0.25.0', lock: 'held', capabilities: ['inspect'], update_waiting: false },
    { protocol: 1, version: '0.25.0', lock: 'held', capabilities: ['inspect'], update_waiting: 'false' },
    { protocol: 1, version: '0.25.0', lock: 'held', capabilities: [42], update_waiting: false }]) {
    assert.throws(() => parseHandshake(value));
  }
});

test('validates a real worker handshake using the Electron protocol version', () => {
  const fs = require('node:fs');
  const os = require('node:os');
  const path = require('node:path');
  const { spawnSync } = require('node:child_process');
  const { PROTOCOL_VERSION } = require('../dist/worker-protocol.js');
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'nx2-worker-node-'));
  try {
    fs.mkdirSync(path.join(root, 'app'));
    fs.mkdirSync(path.join(root, 'data'));
    fs.writeFileSync(path.join(root, 'app', 'current.txt'), '0.0.1');
    fs.writeFileSync(path.join(root, 'data', 'launcher.json'), '{}');
    const result = spawnSync(process.platform === 'win32' ? 'python' : 'python3',
      ['-m', 'launcher.worker', '--root', root], { cwd: path.resolve(__dirname, '../..'), encoding: 'utf8',
        input: JSON.stringify({ id: 1, method: 'hello', protocol: PROTOCOL_VERSION }) + '\n', timeout: 15000 });
    assert.equal(result.status, 0, result.stderr);
    assert.equal(parseHandshake(JSON.parse(result.stdout).result).protocol, PROTOCOL_VERSION);
  } finally { fs.rmSync(root, { recursive: true, force: true }); }
});
