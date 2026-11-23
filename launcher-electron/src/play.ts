import { spawn } from 'node:child_process';
import { readFileSync, statSync } from 'node:fs';
import path from 'node:path';

export type PlayResult = { code: number | null; signal: NodeJS.Signals | null } | { error: 'start_failed' | 'installation_incomplete' };
export type StartResult = { started: true } | { error: 'start_failed' | 'installation_incomplete' };
export function startPlay(root: string): { started: Promise<StartResult>; done: Promise<PlayResult>; pid?: number } {
  let python: string;
  let script: string;
  try {
    const version = readFileSync(path.join(root, 'app', 'current.txt'), 'utf8').trim();
    if (!/^[a-zA-Z0-9][a-zA-Z0-9._+-]*$/.test(version) || version === '.' || version === '..') throw new Error();
    python = process.platform === 'win32' ? path.join(root, 'python', 'pythonw.exe') : path.join(root, 'python', 'bin', 'python3');
    script = path.join(root, 'app', version, 'launcher', 'start.pyw');
    if (!statSync(python).isFile() || !statSync(script).isFile()) throw new Error();
  } catch { return { started: Promise.resolve({ error: 'installation_incomplete' }), done: Promise.resolve({ error: 'installation_incomplete' }) }; }
  const child = spawn(python, ['-I', script, '--root', root, '--play'], { cwd: root, stdio: 'ignore', windowsHide: true });
  const started = new Promise<StartResult>(resolve => {
    child.once('spawn', () => resolve({ started: true }));
    child.once('error', () => resolve({ error: 'start_failed' }));
  });
  const done = new Promise<PlayResult>(resolve => {
    child.once('error', () => resolve({ error: 'start_failed' }));
    child.once('exit', (code, signal) => resolve({ code, signal }));
  });
  return { started, done, pid: child.pid };
}
