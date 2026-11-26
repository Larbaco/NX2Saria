export type ReportsView = {
  available: boolean; send_enabled: boolean; question: boolean; seconds: number | null;
  consent: 'on' | 'off'; notice: boolean; closed: boolean; text_limit: number;
};

export function parseReportsView(value: unknown): ReportsView {
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw new Error('Invalid reports state');
  const view = value as ReportsView;
  if (typeof view.available !== 'boolean' || typeof view.send_enabled !== 'boolean' ||
      typeof view.question !== 'boolean' || typeof view.notice !== 'boolean' || typeof view.closed !== 'boolean' ||
      (view.consent !== 'on' && view.consent !== 'off') || !Number.isInteger(view.text_limit) ||
      view.text_limit < 1 || view.text_limit > 4000 || (view.seconds !== null &&
        (!view.question || !Number.isInteger(view.seconds) || view.seconds < 0 || view.seconds > 30))) {
    throw new Error('Invalid reports state');
  }
  const { available, send_enabled, question, seconds, consent, notice, closed, text_limit } = view;
  return { available, send_enabled, question, seconds, consent, notice, closed, text_limit };
}

export async function readReportPreview(get: (offset: number) => Promise<unknown>) {
  let size = 0;
  let example = false;
  let offset = 0;
  const chunks: Buffer[] = [];
  do {
    const result = await get(offset);
    if (!result || typeof result !== 'object') throw new Error('Invalid preview');
    const frame = result as { size: number; chunk: string; example: boolean };
    if (!Number.isSafeInteger(frame.size) || frame.size < 1 || frame.size > 4 * 1024 * 1024 ||
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
  return { text: new TextDecoder('utf-8', { fatal: true }).decode(Buffer.concat(chunks, size)), example };
}
