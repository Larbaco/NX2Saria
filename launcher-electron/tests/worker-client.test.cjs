const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { openWorker } = require('../dist/worker-client.js');
const { parseInspection } = require('../dist/inspection.js');

test('worker stays open for inspection and releases its lock on close', async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'nx2-worker-client-'));
  fs.mkdirSync(path.join(root, 'app'));
  fs.mkdirSync(path.join(root, 'data'));
  fs.writeFileSync(path.join(root, 'app', 'current.txt'), '0.0.1');
  fs.writeFileSync(path.join(root, 'data', 'launcher.json'), '{}');
  let worker;
  try {
    worker = await openWorker(root, process.platform === 'win32' ? 'python' : 'python3', path.resolve(__dirname, '../..'));
    assert.equal(worker.handshake.lock, 'held');
    assert.equal(parseInspection(await worker.request('inspect')).status, 'not_configured');
    await worker.close();
    worker = await openWorker(root, process.platform === 'win32' ? 'python' : 'python3', path.resolve(__dirname, '../..'));
    assert.equal(worker.handshake.lock, 'held');
  } finally {
    if (worker) await worker.close();
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test('rejects out-of-order events and disables requests', async () => {
  const { spawn } = require('node:child_process');
  const { once } = require('node:events');
  const { WorkerClient } = require('../dist/worker-client.js');
  const script = "setTimeout(() => process.stdout.write(JSON.stringify({event:'game_started',sequence:2,session:1})+'\\n'), 20); process.stdin.resume()";
  const worker = new WorkerClient(spawn(process.execPath, ['-e', script]));
  await once(worker, 'disconnect');
  await assert.rejects(worker.request('inspect'), /disconnected/);
  await worker.close();
});

test('rejects an end event for a different session', async () => {
  const { spawn } = require('node:child_process');
  const { once } = require('node:events');
  const { WorkerClient } = require('../dist/worker-client.js');
  const script = "setTimeout(() => { process.stdout.write(JSON.stringify({event:'game_started',sequence:1,session:1})+'\\n'); process.stdout.write(JSON.stringify({event:'game_ended',sequence:2,session:2,update_waiting:false,code:0,alarm:false})+'\\n'); }, 20); process.stdin.resume()";
  const worker = new WorkerClient(spawn(process.execPath, ['-e', script]));
  await once(worker, 'disconnect');
  await assert.rejects(worker.request('inspect'), /disconnected/);
  await worker.close();
});

test('a timed-out request disables all further requests', async () => {
  const { spawn } = require('node:child_process');
  const { WorkerClient } = require('../dist/worker-client.js');
  const worker = new WorkerClient(spawn(process.execPath, ['-e', 'process.stdin.resume()']), 100);
  try {
    await assert.rejects(worker.request('play'), /timed out/);
    await assert.rejects(worker.request('inspect'), /disconnected/);
  } finally { await worker.close(); }
});
