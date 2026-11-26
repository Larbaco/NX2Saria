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
    await assert.rejects(openWorker(root, process.platform === 'win32' ? 'python' : 'python3', path.resolve(__dirname, '../..')), error => error.code === 'busy');
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

test('accepts a completed job followed by a game session', async () => {
  const { spawn } = require('node:child_process');
  const { once } = require('node:events');
  const { WorkerClient } = require('../dist/worker-client.js');
  const frames = [
    { event: 'job_started', sequence: 1, session: 1 },
    { event: 'job_ended', sequence: 2, session: 1, result: 'ok', outcome: 'resume' },
    { event: 'game_started', sequence: 3, session: 2 },
    { event: 'game_ended', sequence: 4, session: 2, code: 0, alarm: false, update_waiting: false }
  ];
  const script = `setTimeout(() => { for (const f of ${JSON.stringify(frames)}) process.stdout.write(JSON.stringify(f)+'\\n'); }, 20); process.stdin.resume()`;
  const worker = new WorkerClient(spawn(process.execPath, ['-e', script]));
  const seen = [];
  worker.on('event', frame => seen.push(frame.event));
  await new Promise((resolve, reject) => {
    worker.on('event', frame => { if (frame.event === 'game_ended') resolve(); });
    worker.once('disconnect', () => reject(new Error('Unexpected disconnect')));
    setTimeout(() => reject(new Error('Events timed out')), 3000).unref();
  });
  assert.deepEqual(seen, ['job_started', 'job_ended', 'game_started', 'game_ended']);
  await worker.close();
});

test('accepts a sequenced recovery snapshot outside a game or job session', async () => {
  const { spawn } = require('node:child_process');
  const { WorkerClient } = require('../dist/worker-client.js');
  const inspection = { status: 'ready', root: '/synthetic', game_dir: '/synthetic/game', job_marker_present: false,
    game_running: null, hide_while_playing: true, recovery: { state: 'interrupted', kind: 'rebuild', pid: 123 } };
  const frame = { event: 'recovery_changed', sequence: 1, session: 0, inspection };
  const script = `setTimeout(() => process.stdout.write(JSON.stringify(${JSON.stringify(frame)})+'\\n'), 20); process.stdin.resume()`;
  const worker = new WorkerClient(spawn(process.execPath, ['-e', script]));
  try {
    const event = await new Promise((resolve, reject) => {
      worker.once('event', resolve);
      worker.once('disconnect', () => reject(new Error('Unexpected disconnect')));
      setTimeout(() => reject(new Error('Events timed out')), 3000).unref();
    });
    assert.deepEqual(event.inspection.recovery, { state: 'interrupted', kind: 'rebuild' });
  } finally { await worker.close(); }
});

test('applies the frame limit to each message when pipes combine messages', async () => {
  const { EventEmitter } = require('node:events');
  const { PassThrough } = require('node:stream');
  const { WorkerClient } = require('../dist/worker-client.js');
  const child = Object.assign(new EventEmitter(), {
    stdin: new PassThrough(), stdout: new PassThrough(), stderr: new PassThrough(),
  });
  const worker = new WorkerClient(child);
  const seen = [];
  worker.on('event', frame => seen.push(frame.sequence));
  const update = {
    status: 'available', version: '9.0.0',
    notes: Array.from({ length: 20 }, () => '漢'.repeat(360)),
  };
  const frame = sequence => JSON.stringify({ event: 'update_checked', sequence, session: 0, update })
    .replaceAll('漢', String.raw`\u6f22`) + '\n';
  const first = frame(1);
  const second = frame(2);
  assert.ok(first.length < 65536 && second.length < 65536 && first.length + second.length > 65536);
  child.stdout.write(first.slice(0, 20000));
  child.stdout.write(first.slice(20000) + second);
  child.emit('close', 0);
  await worker.close();
  assert.deepEqual(seen, [1, 2]);
});
