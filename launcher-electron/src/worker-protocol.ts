export const PROTOCOL_VERSION = 1;

export type WorkerHandshake = {
  protocol: number;
  version: string;
  lock: 'held' | 'unavailable';
  capabilities: string[];
  update_waiting: boolean;
};

export function parseHandshake(input: unknown): WorkerHandshake {
  if (!input || typeof input !== 'object' || Array.isArray(input)) throw new Error('Invalid worker handshake');
  const value = input as Record<string, unknown>;
  if (value.protocol !== PROTOCOL_VERSION || typeof value.version !== 'string' || !value.version ||
      (value.lock !== 'held' && value.lock !== 'unavailable') || !Array.isArray(value.capabilities) ||
      !value.capabilities.every(item => typeof item === 'string') || typeof value.update_waiting !== 'boolean') {
    throw new Error('Invalid worker handshake');
  }
  return { protocol: PROTOCOL_VERSION, version: value.version, lock: value.lock,
    capabilities: value.capabilities, update_waiting: value.update_waiting };
}

export type PlayResult = { status: 'started' | 'not_built' | 'own_game' | 'external_game' | 'rebuild' | 'declined' } |
  { status: 'memory'; attempt: number; memory: { remaining: number; need: number | null; unbounded: boolean } };

export function parsePlayResult(input: unknown): PlayResult {
  if (!input || typeof input !== 'object' || Array.isArray(input)) throw new Error('Invalid Play result');
  const value = input as Record<string, unknown>;
  if (['started', 'not_built', 'own_game', 'external_game', 'rebuild', 'declined'].includes(value.status as string)) {
    return { status: value.status } as PlayResult;
  }
  if (value.status !== 'memory' || !Number.isSafeInteger(value.attempt) || (value.attempt as number) < 1 ||
      !value.memory || typeof value.memory !== 'object' || Array.isArray(value.memory)) throw new Error('Invalid Play result');
  const memory = value.memory as Record<string, unknown>;
  if (typeof memory.remaining !== 'number' || !Number.isFinite(memory.remaining) || memory.remaining < 0 ||
      typeof memory.unbounded !== 'boolean' ||
      (memory.unbounded ? memory.need !== null : typeof memory.need !== 'number' || !Number.isFinite(memory.need))) {
    throw new Error('Invalid Play result');
  }
  return { status: 'memory', attempt: value.attempt as number,
    memory: { remaining: memory.remaining, need: memory.need as number | null, unbounded: memory.unbounded } };
}
