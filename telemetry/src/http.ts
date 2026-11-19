// Response helpers and the capped body reader shared by the public and admin routes.

// Headers every response carries: no caching anywhere, no content sniffing, nothing loadable from the page.
const BASE_HEADERS: Record<string, string> = {
  "Cache-Control": "no-store",
  "X-Content-Type-Options": "nosniff",
  "Content-Security-Policy": "default-src 'none'; style-src 'unsafe-inline'",
  "Referrer-Policy": "no-referrer",
};

export function respond(body: BodyInit | null, status: number, contentType: string, extra: Record<string, string> = {}): Response {
  const headers = new Headers(BASE_HEADERS);
  headers.set("Content-Type", contentType);
  for (const [name, value] of Object.entries(extra)) headers.set(name, value);
  return new Response(body, { status, headers });
}

export function json(status: number, value: unknown, extra: Record<string, string> = {}): Response {
  return respond(JSON.stringify(value), status, "application/json; charset=utf-8", extra);
}

export function error(status: number, message: string, extra: Record<string, string> = {}): Response {
  return json(status, { error: message }, extra);
}

export function methodNotAllowed(allow: string): Response {
  return error(405, "method not allowed", { Allow: allow });
}

// Thrown by readCappedBody and by the validators; the router turns it into an error response.
export class HttpError extends Error {
  constructor(
    readonly status: number,
    message: string,
  ) {
    super(message);
  }
}

// True when the Content-Type is application/json, with or without parameters such as charset=utf-8.
export function isJsonContentType(request: Request): boolean {
  const value = request.headers.get("content-type");
  if (value === null) return false;
  return value.split(";", 1)[0]!.trim().toLowerCase() === "application/json";
}

// Reads the request body as UTF-8 text, refusing it with 413 as soon as it passes `cap` bytes: first from
// Content-Length when the client sent one, then while reading, so a body without a length (or with a false one) is
// never held in memory past the cap. Invalid UTF-8 is a 400.
export async function readCappedBody(request: Request, cap: number): Promise<string> {
  const declared = request.headers.get("content-length");
  if (declared !== null) {
    const length = Number(declared);
    if (!Number.isFinite(length) || length < 0) throw new HttpError(400, "bad content-length");
    if (length > cap) throw new HttpError(413, "body too large");
  }
  if (request.body === null) return "";
  const reader = request.body.getReader();
  const chunks: Uint8Array[] = [];
  let total = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    total += value.byteLength;
    if (total > cap) {
      await reader.cancel();
      throw new HttpError(413, "body too large");
    }
    chunks.push(value);
  }
  const bytes = new Uint8Array(total);
  let offset = 0;
  for (const chunk of chunks) {
    bytes.set(chunk, offset);
    offset += chunk.byteLength;
  }
  try {
    return new TextDecoder("utf-8", { fatal: true, ignoreBOM: false }).decode(bytes);
  } catch {
    throw new HttpError(400, "body is not UTF-8");
  }
}

// The two-letter country Cloudflare attaches to the request, or "XX" when it is missing or anything other than two
// uppercase letters (Cloudflare's "T1" for Tor becomes "XX" too).
export function countryOf(request: Request): string {
  const cf = (request as { cf?: { country?: unknown } }).cf;
  const country = cf?.country;
  return typeof country === "string" && /^[A-Z]{2}$/.test(country) ? country : "XX";
}
