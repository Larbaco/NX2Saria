export type ReportsView = {
  available: boolean; send_enabled: boolean; question: boolean; seconds: number | null;
  consent: 'on' | 'off'; notice: boolean; closed: boolean; text_limit: number; crash_ask: boolean | null;
};

export function parseReportsView(value: unknown): ReportsView {
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw new Error('Invalid reports state');
  const view = value as ReportsView;
  if (typeof view.available !== 'boolean' || typeof view.send_enabled !== 'boolean' ||
      typeof view.question !== 'boolean' || (view.crash_ask !== null && typeof view.crash_ask !== 'boolean') ||
      typeof view.notice !== 'boolean' || typeof view.closed !== 'boolean' ||
      (view.consent !== 'on' && view.consent !== 'off') || !Number.isInteger(view.text_limit) ||
      view.text_limit < 1 || view.text_limit > 4000 || (view.seconds !== null &&
        (!view.question || !Number.isInteger(view.seconds) || view.seconds < 0 || view.seconds > 30))) {
    throw new Error('Invalid reports state');
  }
  const { available, send_enabled, question, seconds, consent, notice, closed, text_limit, crash_ask } = view;
  return { available, send_enabled, question, seconds, consent, notice, closed, text_limit, crash_ask };
}

type CrashView = {
  status: 'shown'; kind: string; message: string; head_lines: number; tail_lines: number;
  bytes: number; summary: boolean; whole: boolean; guard_ms: number; can_send: boolean;
};

export function parseCrashOffer(value: unknown): CrashView | null {
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw new Error('Invalid crash offer');
  const offer = value as CrashView | { status: 'none' };
  if (offer.status === 'none') return null;
  if (offer.status !== 'shown' || typeof offer.kind !== 'string' || offer.kind.length > 96 ||
      typeof offer.message !== 'string' || offer.message.length > 1800 ||
      !Number.isInteger(offer.head_lines) || offer.head_lines < 0 || offer.head_lines > 100 ||
      !Number.isInteger(offer.tail_lines) || offer.tail_lines < 1 || offer.tail_lines > 200 ||
      !Number.isInteger(offer.bytes) || offer.bytes < 1 || offer.bytes > 72 * 1024 ||
      typeof offer.summary !== 'boolean' || typeof offer.whole !== 'boolean' ||
      !Number.isInteger(offer.guard_ms) || offer.guard_ms < 0 || offer.guard_ms > 1000 ||
      typeof offer.can_send !== 'boolean') throw new Error('Invalid crash offer');
  const { status, kind, message, head_lines, tail_lines, bytes, summary, whole, guard_ms, can_send } = offer;
  return { status, kind, message, head_lines, tail_lines, bytes, summary, whole, guard_ms, can_send };
}

export async function readWorkerBytes(get: (offset: number) => Promise<unknown>, maxBytes = 4 * 1024 * 1024) {
  let size = 0;
  let example = false;
  let offset = 0;
  const chunks: Buffer[] = [];
  do {
    const result = await get(offset);
    if (!result || typeof result !== 'object') throw new Error('Invalid preview');
    const frame = result as { size: number; chunk: string; example: boolean };
    if (!Number.isSafeInteger(frame.size) || frame.size < 1 || frame.size > maxBytes ||
        typeof frame.chunk !== 'string' || frame.chunk.length > 22000 ||
        typeof frame.example !== 'boolean') throw new Error('Invalid preview');
    if (offset === 0) {
      size = frame.size;
      example = frame.example;
    } else if (frame.size !== size || frame.example !== example) throw new Error('Preview changed');
    const chunk = Buffer.from(frame.chunk, 'base64');
    if (!chunk.length || chunk.toString('base64') !== frame.chunk || offset + chunk.length > size) {
      throw new Error('Invalid preview chunk');
    }
    chunks.push(chunk);
    offset += chunk.length;
  } while (offset < size);
  return { bytes: Buffer.concat(chunks, size), example };
}

export async function readReportPreview(get: (offset: number) => Promise<unknown>) {
  const { bytes, example } = await readWorkerBytes(get);
  return { text: new TextDecoder('utf-8', { fatal: true }).decode(bytes), example };
}
