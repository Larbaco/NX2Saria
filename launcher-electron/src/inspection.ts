import { execFile } from 'node:child_process';

export type Inspection = { status: string; reason?: string; root?: string; game_dir?: string | null; job_marker_present?: boolean; game_running?: true | null; hide_while_playing?: boolean | null };
const statuses = new Set(['ready', 'not_ready', 'not_configured', 'inspection_error']);
export function parseInspection(data: unknown): Inspection {
  if (!data || typeof data !== 'object') throw new Error();
  const value = data as Record<string, unknown>;
  if (typeof value.status !== 'string' || !statuses.has(value.status) || typeof value.root !== 'string' || typeof value.job_marker_present !== 'boolean' || (value.game_running !== null && value.game_running !== true) || (value.hide_while_playing !== null && typeof value.hide_while_playing !== 'boolean') || (value.game_dir !== null && typeof value.game_dir !== 'string')) throw new Error();
  return { status: value.status, root: value.root, game_dir: value.game_dir as string | null, job_marker_present: value.job_marker_present as boolean, game_running: value.game_running as true | null, hide_while_playing: value.hide_while_playing as boolean | null, ...(typeof value.reason === 'string' ? { reason: value.reason } : {}) };
}
export function inspect(root: string, python: string, source: string, ownerPid?: number): Promise<Inspection> {
  return new Promise(resolve => {
    execFile(python, ['-m', 'launcher.inspect_installation', root, ...(ownerPid === undefined ? [] : ['--owner-pid', String(ownerPid)])], { cwd: source, timeout: 10000, maxBuffer: 65536 }, (error, stdout) => {
      if (error) return resolve({ status: 'inspection_error', reason: 'worker_failed' });
      try {
        const data: unknown = JSON.parse(stdout);
        resolve(parseInspection(data));
      } catch { resolve({ status: 'inspection_error', reason: 'invalid_worker_response' }); }
    });
  });
}
