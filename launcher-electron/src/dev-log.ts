import { appendFileSync, statSync, writeFileSync } from 'node:fs';
import path from 'node:path';

type Event = 'appearance' | 'inspection' | 'install' | 'exit' | 'mods' | 'play' | 'rebuild' | 'recovery' | 'reports' | 'root' | 'saves' | 'settings' | 'steam' | 'update' | 'window';
export type DevelopmentLog = { write(event: Event, outcome: string): void };

function timestamp(now: Date): string {
  return [now.getHours(), now.getMinutes(), now.getSeconds()].map(n => String(n).padStart(2, '0')).join(':');
}
function safe(value: string): string {
  return /^[A-Za-z0-9_.+= -]{1,120}$/.test(value) ? value : 'invalid';
}
export function createDevelopmentLog(root: string, platform: string, build: string, startedAt: Date): DevelopmentLog | null {
  const folder = path.join(root, 'data', 'logs');
  const file = path.join(folder, 'launcher-electron.log');
  const identity = `instance=${process.pid}`;
  const buildId = /^[a-f0-9]{8,40}$/.test(build) ? build : 'unknown';
  let enabled = true;
  try {
    if (!statSync(folder).isDirectory()) return null;
    if ((statSync(file, { throwIfNoEntry: false })?.size ?? 0) > 1024 * 1024) writeFileSync(file, '');
    appendFileSync(file, `${timestamp(startedAt)} electron: start build=${buildId} platform=${safe(platform)} ${identity}\n`);
  } catch { return null; }
  return {
    write(event, outcome) {
      if (!enabled) return;
      try { appendFileSync(file, `${timestamp(new Date())} electron: ${event} ${safe(outcome)} ${identity}\n`); }
      catch { enabled = false; }
    }
  };
}
