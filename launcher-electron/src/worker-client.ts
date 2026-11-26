import { spawn, type ChildProcessWithoutNullStreams } from 'node:child_process';
import { EventEmitter } from 'node:events';
import { parseInspection } from './inspection';
import { parseHandshake, parseUpdateCheck, PROTOCOL_VERSION, type WorkerHandshake } from './worker-protocol';

export class WorkerRequestError extends Error {
  constructor(readonly code: 'busy' | 'request_failed' | 'runtime_incompatible') { super('Worker request failed'); }
}

type Pending = { resolve: (value: any) => void; reject: (error: Error) => void; timer: NodeJS.Timeout };

export class WorkerClient extends EventEmitter {
  handshake!: WorkerHandshake;
  private nextId = 1;
  private lastSequence = 0;
  private activeSession: number | null = null;
  private windowReady = false;
  private activeJob: number | null = null;
  private buffer = '';
  private pending = new Map<number, Pending>();
  private ended = false;
  private done: Promise<void>;

  constructor(private child: ChildProcessWithoutNullStreams, private timeoutMs = 15000) {
    super();
    this.done = new Promise(resolve => child.once('close', code => {
      this.fail(code === 78 ? new WorkerRequestError('runtime_incompatible') : undefined);
      resolve();
    }));
    child.once('error', () => this.fail());
    child.stderr.resume();
    child.stdout.setEncoding('utf8');
    child.stdout.on('data', (data: string) => this.receive(data));
  }

  private fail(error = new Error('Worker disconnected')) {
    if (this.ended) return;
    this.ended = true;
    for (const pending of this.pending.values()) {
      clearTimeout(pending.timer);
      pending.reject(error);
    }
    this.pending.clear();
    this.emit('disconnect');
  }

  private receive(data: string) {
    this.buffer += data;
    if (Buffer.byteLength(this.buffer) > 65536) { this.fail(); this.child.stdin.end(); return; }
    let index;
    while ((index = this.buffer.indexOf('\n')) >= 0) {
      const line = this.buffer.slice(0, index);
      this.buffer = this.buffer.slice(index + 1);
      try {
        const frame = JSON.parse(line);
        if (!frame || typeof frame !== 'object' || Array.isArray(frame)) throw new Error();
        if (typeof frame.id === 'number' && this.pending.has(frame.id)) {
          if (!frame.error && !('result' in frame)) throw new Error();
          const pending = this.pending.get(frame.id)!;
          this.pending.delete(frame.id);
          clearTimeout(pending.timer);
          if (frame.error) pending.reject(new WorkerRequestError(
            frame.error.code === 'busy' || frame.error.code === 'runtime_incompatible' ? frame.error.code : 'request_failed'));
          else if ('result' in frame) pending.resolve(frame.result);
          else throw new Error();
        } else if (typeof frame.event === 'string') {
          const recovery = frame.event === 'recovery_changed';
          const update = frame.event === 'update_checked';
          if (!['game_started', 'game_window_ready', 'game_ended', 'job_started', 'job_progress', 'job_ended', 'memory_required', 'play_failed', 'recovery_changed', 'update_checked'].includes(frame.event) ||
              !Number.isSafeInteger(frame.sequence) || frame.sequence !== this.lastSequence + 1 ||
              !Number.isSafeInteger(frame.session) || ((recovery || update) ? frame.session !== 0 : frame.session < 1)) throw new Error();
          if (update) {
            frame.update = parseUpdateCheck(frame.update);
          } else if (recovery) {
            if (this.activeJob !== null || this.activeSession !== null) throw new Error();
            frame.inspection = parseInspection(frame.inspection);
          } else if (frame.event === 'job_started') {
            if (this.activeJob !== null || this.activeSession !== null) throw new Error();
            this.activeJob = frame.session;
          } else if (frame.event === 'job_progress') {
            if (this.activeJob !== frame.session || typeof frame.fraction !== 'number' || !Number.isFinite(frame.fraction) ||
                frame.fraction < 0 || frame.fraction > 1 || typeof frame.step !== 'string' || !/^[a-z_]{1,32}$/.test(frame.step)) throw new Error();
          } else if (frame.event === 'job_ended') {
            if (this.activeJob !== frame.session || !['ok', 'failed', 'cancelled'].includes(frame.result) ||
                !['resume', 'complete', 'failed', 'cancelled'].includes(frame.outcome)) throw new Error();
            this.activeJob = null;
          } else if (frame.event === 'game_started') {
            if (this.activeSession !== null || this.activeJob !== null) throw new Error();
            this.activeSession = frame.session;
            this.windowReady = false;
          } else if (frame.event === 'game_window_ready') {
            if (this.activeSession !== frame.session || this.windowReady) throw new Error();
            this.windowReady = true;
          } else if (frame.event === 'game_ended') {
            if (this.activeSession !== frame.session || typeof frame.update_waiting !== 'boolean' ||
                typeof frame.alarm !== 'boolean' || !(frame.code === null || Number.isSafeInteger(frame.code)) ||
                (frame.message !== undefined && (typeof frame.message !== 'string' || frame.message.length > 1024))) throw new Error();
            this.activeSession = null;
          } else if (this.activeJob !== null || this.activeSession !== null) throw new Error();
          this.lastSequence = frame.sequence;
          this.emit('event', frame);
        }
        else throw new Error();
      } catch { this.fail(); this.child.stdin.end(); return; }
    }
  }

  request(method: string, args: Record<string, unknown> = {}): Promise<any> {
    if (this.ended) return Promise.reject(new Error('Worker disconnected'));
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error('Worker request timed out'));
        this.fail();
        this.child.stdin.end();
      }, this.timeoutMs);
      this.pending.set(id, { resolve, reject, timer });
      this.child.stdin.write(JSON.stringify({ ...args, id, method }) + '\n', error => {
        if (error) this.fail();
      });
    });
  }

  async close(): Promise<void> {
    if (!this.child.stdin.destroyed) this.child.stdin.end();
    await this.done;
  }
}

const installedBootstrap = 'import os, runpy, sys; sys.version_info[:2] == (3, 14) or sys.exit(78); ' +
  'source, app = sys.argv[1:3]; del sys.argv[1:3]; sys.path[:0] = [source, app, os.path.join(app, "tools")]; ' +
  'runpy.run_module("launcher.worker", run_name="__main__")';

export async function openWorker(root: string, python: string, source: string, installed?: { version: string; app: string }): Promise<WorkerClient> {
  const windows = process.platform === 'win32';
  const args = installed
    ? ['-I', '-c', installedBootstrap, source, installed.app, '--app-version', installed.version]
    : ['-m', 'launcher.worker'];
  args.push('--root', root);
  if (windows) args.push('--parent-pid', String(process.pid));
  const worker = new WorkerClient(spawn(python, args, { cwd: source, detached: windows }));
  try {
    worker.handshake = parseHandshake(await worker.request('hello', { protocol: PROTOCOL_VERSION }));
    return worker;
  } catch (error) {
    await worker.close();
    throw error;
  }
}
