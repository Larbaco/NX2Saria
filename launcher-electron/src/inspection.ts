type Recovery = { state: 'alive' | 'interrupted' | 'unverified'; kind: 'install' | 'rebuild' | 'update' | 'other' };
type FirstRunPreferences = { channel: 'stable' | 'preview'; auto_check: boolean; desktop_shortcut: boolean | null };
type FirstRunRequirements = { status: string; message?: string };
export type Inspection = { status: string; reason?: string; rebuild_required?: true; root?: string; game_dir?: string | null; job_marker_present?: boolean; game_running?: true | null; hide_while_playing?: boolean | null; recovery?: Recovery; first_run_preferences?: FirstRunPreferences; first_run_requirements?: FirstRunRequirements };
const statuses = new Set(['ready', 'not_ready', 'not_configured', 'inspection_error']);
export function parseInspection(data: unknown): Inspection {
  if (!data || typeof data !== 'object') throw new Error();
  const value = data as Record<string, unknown>;
  if (typeof value.status !== 'string' || !statuses.has(value.status) || typeof value.root !== 'string' || typeof value.job_marker_present !== 'boolean' || (value.game_running !== null && value.game_running !== true) || (value.hide_while_playing !== null && typeof value.hide_while_playing !== 'boolean') || (value.game_dir !== null && typeof value.game_dir !== 'string')) throw new Error();
  if (value.rebuild_required !== undefined && (value.rebuild_required !== true || value.status !== 'ready')) throw new Error();
  let recovery: Recovery | undefined;
  if (value.recovery !== undefined) {
    const result = value.recovery as Record<string, unknown> | null;
    if (!result || (result.state !== 'alive' && result.state !== 'interrupted' && result.state !== 'unverified') ||
        (result.kind !== 'install' && result.kind !== 'rebuild' && result.kind !== 'update' && result.kind !== 'other')) {
      throw new Error();
    }
    recovery = { state: result.state, kind: result.kind };
  }
  let preferences: FirstRunPreferences | undefined;
  if (value.first_run_preferences !== undefined) {
    const result = value.first_run_preferences as Record<string, unknown> | null;
    if (!result || (result.channel !== 'stable' && result.channel !== 'preview') ||
        typeof result.auto_check !== 'boolean' || (result.desktop_shortcut !== null && typeof result.desktop_shortcut !== 'boolean')) {
      throw new Error();
    }
    preferences = { channel: result.channel, auto_check: result.auto_check, desktop_shortcut: result.desktop_shortcut };
  }
  let requirements: FirstRunRequirements | undefined;
  if (value.first_run_requirements !== undefined) {
    const result = value.first_run_requirements as Record<string, unknown> | null;
    if (!result || typeof result.status !== 'string' || !['ready', 'runtime_zstd_missing', 'linux_tools_missing',
        'linux_check_failed'].includes(result.status)) throw new Error();
    if (result.status === 'linux_tools_missing' && (typeof result.message !== 'string' || result.message.length > 4096)) {
      throw new Error();
    }
    requirements = { status: result.status, ...(result.status === 'linux_tools_missing' && typeof result.message === 'string'
      ? { message: result.message } : {}) };
  }
  return { status: value.status, root: value.root, game_dir: value.game_dir as string | null, job_marker_present: value.job_marker_present as boolean, game_running: value.game_running as true | null, hide_while_playing: value.hide_while_playing as boolean | null, ...(typeof value.reason === 'string' ? { reason: value.reason } : {}), ...(value.rebuild_required === true ? { rebuild_required: true as const } : {}), ...(recovery ? { recovery } : {}), ...(preferences ? { first_run_preferences: preferences } : {}), ...(requirements ? { first_run_requirements: requirements } : {}) };
}
