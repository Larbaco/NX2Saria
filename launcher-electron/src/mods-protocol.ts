type Conflict = { name: string; files: number; wins: boolean };
type ModRow = {
  name: string; enabled: boolean; usable: boolean; waiting: boolean; files: number;
  patches: number; cheats: boolean; other_version: number; problem: string; overlaps: Conflict[];
};

function record(value: unknown): Record<string, unknown> {
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw new Error('Invalid mod data');
  return value as Record<string, unknown>;
}

function count(value: unknown): number {
  if (typeof value !== 'number' || !Number.isSafeInteger(value) || value < 0) throw new Error('Invalid mod count');
  return value;
}

function flag(value: unknown): boolean {
  if (typeof value !== 'boolean') throw new Error('Invalid mod flag');
  return value;
}

function text(value: unknown, limit: number): string {
  if (typeof value !== 'string' || value.length > limit || value.includes('\0')) throw new Error('Invalid mod text');
  return value;
}

export function parseModsPage(value: unknown): { page: number; pages: number; count: number; items: ModRow[] } {
  const result = record(value);
  const page = count(result.page);
  const pages = count(result.pages);
  if (page >= pages || !Array.isArray(result.items) || result.items.length > 6) throw new Error('Invalid mods page');
  const items = result.items.map(value => {
    const mod = record(value);
    if (!Array.isArray(mod.overlaps) || mod.overlaps.length > 3) throw new Error('Invalid mod conflicts');
    const overlaps = mod.overlaps.map(value => {
      const conflict = record(value);
      return { name: text(conflict.name, 255), files: count(conflict.files), wins: flag(conflict.wins) };
    });
    return { name: text(mod.name, 255), enabled: flag(mod.enabled), usable: flag(mod.usable),
      waiting: flag(mod.waiting), files: count(mod.files), patches: count(mod.patches), cheats: flag(mod.cheats),
      other_version: count(mod.other_version), problem: text(mod.problem, 256), overlaps };
  });
  return { page, pages, count: count(result.count), items };
}

export function parseModAction(value: unknown) {
  const result = record(value);
  if (result.status === 'ok') {
    return { status: result.status, page: result.page === undefined ? undefined : count(result.page),
      name: result.name === undefined ? undefined : text(result.name, 255),
      problem: result.problem === undefined ? undefined : text(result.problem, 256) };
  }
  if (result.status === 'invalid_mod' || result.status === 'failed' || result.status === 'game_running') {
    return { status: result.status, detail: result.detail === undefined ? undefined : text(result.detail, 256) };
  }
  throw new Error('Invalid mod result');
}
