import { spawn, type ChildProcessWithoutNullStreams } from 'node:child_process';
import { EventEmitter } from 'node:events';
import { parseHandshake, PROTOCOL_VERSION, type WorkerHandshake } from './worker-protocol';

type Pending = { resolve: (value: any) => void; reject: (error: Error) => void; timer: NodeJS.Timeout };

export class WorkerClient extends EventEmitter {
  handshake!: WorkerHandshake;
  private nextId = 1;
  private lastSequence = 0;
  private activeSession: number | null = null;
  private buffer = '';
  private pending = new Map<number, Pending>();
  private ended = false;
  private done: Promise<void>;

  constructor(private child: ChildProcessWithoutNullStreams, private timeoutMs = 15000) {
    super();
    this.done = new Promise(resolve => child.once('close', () => { this.fail(); resolve(); }));
    child.once('error', () => this.fail());
    child.stderr.resume();
    child.stdout.setEncoding('utf8');
    child.stdout.on('data', (data: string) => this.receive(data));
  }

  private fail() {
    if (this.ended) return;
    this.ended = true;
    for (const pending of this.pending.values()) {
      clearTimeout(pending.timer);
      pending.reject(new Error('Worker disconnected'));
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
          if (frame.error) pending.reject(new Error('Worker request failed'));
          else if ('result' in frame) pending.resolve(frame.result);
          else throw new Error();
        } else if (typeof frame.event === 'string') {
          if (!['game_started', 'game_ended'].includes(frame.event) ||
              !Number.isSafeInteger(frame.sequence) || frame.sequence !== this.lastSequence + 1 ||
              !Number.isSafeInteger(frame.session) || frame.session < 1 ||
              (frame.event === 'game_started' && this.activeSession !== null) ||
              (frame.event === 'game_ended' && (typeof frame.update_waiting !== 'boolean' ||
                                                typeof frame.alarm !== 'boolean' ||
                                                !(frame.code === null || Number.isSafeInteger(frame.code)) ||
                                                this.activeSession !== frame.session))) throw new Error();
          this.activeSession = frame.event === 'game_started' ? frame.session : null;
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

export async function openWorker(root: string, python: string, source: string): Promise<WorkerClient> {
  const worker = new WorkerClient(spawn(python, ['-m', 'launcher.worker', '--root', root], { cwd: source, detached: process.platform === 'win32' }));
  try {
    worker.handshake = parseHandshake(await worker.request('hello', { protocol: PROTOCOL_VERSION }));
    return worker;
  } catch (error) {
    await worker.close();
    throw error;
  }
}
