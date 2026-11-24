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
