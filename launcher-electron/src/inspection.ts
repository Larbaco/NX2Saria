type Recovery = { state: 'alive' | 'interrupted' | 'unverified'; kind: 'install' | 'rebuild' | 'update' | 'other' };
export type Inspection = { status: string; reason?: string; root?: string; game_dir?: string | null; job_marker_present?: boolean; game_running?: true | null; hide_while_playing?: boolean | null; recovery?: Recovery };
const statuses = new Set(['ready', 'not_ready', 'not_configured', 'inspection_error']);
export function parseInspection(data: unknown): Inspection {
  if (!data || typeof data !== 'object') throw new Error();
  const value = data as Record<string, unknown>;
  if (typeof value.status !== 'string' || !statuses.has(value.status) || typeof value.root !== 'string' || typeof value.job_marker_present !== 'boolean' || (value.game_running !== null && value.game_running !== true) || (value.hide_while_playing !== null && typeof value.hide_while_playing !== 'boolean') || (value.game_dir !== null && typeof value.game_dir !== 'string')) throw new Error();
  let recovery: Recovery | undefined;
  if (value.recovery !== undefined) {
    const result = value.recovery as Record<string, unknown> | null;
    if (!result || (result.state !== 'alive' && result.state !== 'interrupted' && result.state !== 'unverified') ||
        (result.kind !== 'install' && result.kind !== 'rebuild' && result.kind !== 'update' && result.kind !== 'other')) {
      throw new Error();
    }
    recovery = { state: result.state, kind: result.kind };
  }
  return { status: value.status, root: value.root, game_dir: value.game_dir as string | null, job_marker_present: value.job_marker_present as boolean, game_running: value.game_running as true | null, hide_while_playing: value.hide_while_playing as boolean | null, ...(typeof value.reason === 'string' ? { reason: value.reason } : {}), ...(recovery ? { recovery } : {}) };
}
