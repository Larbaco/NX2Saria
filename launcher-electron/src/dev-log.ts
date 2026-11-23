import { promises as fs } from 'node:fs';
import path from 'node:path';

export type DevelopmentLog = { write(event: string, outcome: string): Promise<void> };
const events = new Set(['inspection', 'play', 'window']);
function timestamp(): string {
  const now = new Date();
  return [now.getHours(), now.getMinutes(), now.getSeconds()].map(n => String(n).padStart(2, '0')).join(':');
}
function safe(value: string): string {
  return /^[A-Za-z0-9_.+= -]{1,80}$/.test(value) ? value : 'invalid';
}
export async function createDevelopmentLog(root: string, origin: 'default' | 'manual', brand: 'NX2Saria' | 'NX2navi' | 'other', version: string, platform: string): Promise<DevelopmentLog> {
  const folder = path.join(root, 'data', 'logs');
  const file = path.join(folder, 'launcher-electron.log');
  let enabled = false;
  let queue = Promise.resolve();
  try {
    if ((await fs.stat(folder)).isDirectory()) {
      try { if ((await fs.stat(file)).size > 1024 * 1024) await fs.writeFile(file, ''); }
      catch (error) { if ((error as NodeJS.ErrnoException).code !== 'ENOENT') throw error; }
      enabled = true;
      await fs.appendFile(file, `${timestamp()} electron: development ${safe(version)}, platform ${safe(platform)}\n${timestamp()} electron: root ${origin} (${brand})\n`);
    }
  } catch { enabled = false; }
  return {
    write(event: string, outcome: string) {
      if (!enabled || !events.has(event)) return Promise.resolve();
      queue = queue.then(() => fs.appendFile(file, `${timestamp()} electron: ${event} ${safe(outcome)}\n`)).catch(() => { enabled = false; });
      return queue;
    }
  };
}
