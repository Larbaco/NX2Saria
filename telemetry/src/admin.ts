// The admin panel: one server-rendered HTML page, two CSV exports and the plain-text view of a comment's log.
// Everything in the database came from players, so every value is HTML-escaped on the page, CSV cells are quoted
// and guarded against spreadsheet formulas, and the log is served as text/plain.

import type { Env } from "./env";
import { SUMMARY_COLUMNS } from "./db";
import { VERSION_RE } from "./validate";
import { error, respond } from "./http";

const DAY_S = 86400;
const ALLOWED_DAYS = [7, 30, 180] as const;
const DEFAULT_DAYS = 30;
// The performance and fatal-line sections read at most this many of the newest rows, which bounds the Worker's
// memory and time; the page says when the cap was reached.
export const STATS_ROW_CAP = 50000;
// The CSV exports hold at most this many of the newest rows.
export const CSV_ROW_CAP = 100000;

// ---- Authentication ----

async function sha256(text: string): Promise<ArrayBuffer> {
  return crypto.subtle.digest("SHA-256", new TextEncoder().encode(text));
}

// Compares the Basic password with ADMIN_TOKEN in constant time: both are hashed first, so the comparison never
// depends on where they differ or on their lengths.
async function passwordMatches(given: string, expected: string): Promise<boolean> {
  const [a, b] = await Promise.all([sha256(given), sha256(expected)]);
  return crypto.subtle.timingSafeEqual(a, b);
}

function basicPassword(request: Request): string | null {
  const header = request.headers.get("authorization");
  if (header === null) return null;
  const match = /^Basic\s+([A-Za-z0-9+/=]+)\s*$/i.exec(header);
  if (match === null) return null;
  let decoded: string;
  try {
    const binary = atob(match[1]!);
    decoded = new TextDecoder().decode(Uint8Array.from(binary, (c) => c.charCodeAt(0)));
  } catch {
    return null;
  }
  const colon = decoded.indexOf(":");
  return colon < 0 ? null : decoded.slice(colon + 1);
}

// null when the request may see the panel; otherwise the response to send (503 without ADMIN_TOKEN, 401 without
// or with a wrong password).
export async function checkAdmin(request: Request, env: Env): Promise<Response | null> {
  const token = env.ADMIN_TOKEN;
  if (token === undefined || token.length === 0) return error(503, "admin panel not configured");
  const given = basicPassword(request);
  if (given !== null && (await passwordMatches(given, token))) return null;
  return error(401, "unauthorized", { "WWW-Authenticate": 'Basic realm="NX2Saria telemetry", charset="UTF-8"' });
}

// ---- Filters ----

interface Filters {
  days: number;
  version: string | null;
  since: number;
}

function filtersOf(url: URL, now: number): Filters {
  const daysParam = Number(url.searchParams.get("days"));
  const days = (ALLOWED_DAYS as readonly number[]).includes(daysParam) ? daysParam : DEFAULT_DAYS;
  const versionParam = url.searchParams.get("version") ?? "";
  const version = VERSION_RE.test(versionParam) ? versionParam : null;
  return { days, version, since: now - days * DAY_S };
}

// WHERE clause and its parameters for the sessions table (version is summary.version).
function sessionsWhere(f: Filters): { sql: string; params: (string | number)[] } {
  return f.version === null
    ? { sql: "received_at >= ?", params: [f.since] }
    : { sql: "received_at >= ? AND version = ?", params: [f.since, f.version] };
}

// The same filter on comments: version is the attached summary's, or the launcher's when no summary came with it.
function feedbackWhere(f: Filters): { sql: string; params: (string | number)[] } {
  return f.version === null
    ? { sql: "received_at >= ?", params: [f.since] }
    : { sql: "received_at >= ? AND COALESCE(version, launcher_version) = ?", params: [f.since, f.version] };
}

function query(f: Filters): string {
  const p = new URLSearchParams({ days: String(f.days) });
  if (f.version !== null) p.set("version", f.version);
  return p.toString();
}

// ---- Formatting ----

export function escapeHtml(value: unknown): string {
  if (value === null || value === undefined) return "";
  return String(value)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#39;");
}

function isoDate(seconds: number): string {
  return new Date(seconds * 1000).toISOString().replace(".000Z", "Z");
}

function fmt(value: number | null, digits = 1): string {
  return value === null ? "–" : value.toFixed(digits);
}

// Value at quantile q (0..1) of an ascending list, by the nearest-rank method; null for an empty list.
export function quantile(sorted: number[], q: number): number | null {
  if (sorted.length === 0) return null;
  const rank = Math.ceil(q * sorted.length);
  return sorted[Math.min(sorted.length - 1, Math.max(0, rank - 1))]!;
}

// Median as the mean of the two middle values when the count is even.
export function median(sorted: number[]): number | null {
  const n = sorted.length;
  if (n === 0) return null;
  return n % 2 === 1 ? sorted[(n - 1) / 2]! : (sorted[n / 2 - 1]! + sorted[n / 2]!) / 2;
}

// Groups fatal lines that differ only in addresses and numbers: 0x-prefixed hex, runs of 8 or more hex digits, and
// decimal numbers all become "#".
export function normalizeFatal(line: string): string {
  return line
    .replace(/0x[0-9a-fA-F]+/g, "#")
    .replace(/\b[0-9a-fA-F]{8,}\b/g, "#")
    .replace(/\d+/g, "#")
    .replace(/\s+/g, " ")
    .trim();
}

type Cell = string | number | null;

function table(caption: string, headers: string[], rows: Cell[][], numericFrom = 1): string {
  const head = headers.map((h, i) => `<th${i >= numericFrom ? ' class="n"' : ""}>${escapeHtml(h)}</th>`).join("");
  const body =
    rows.length === 0
      ? `<tr><td colspan="${headers.length}" class="empty">No data.</td></tr>`
      : rows
          .map((r) => `<tr>${r.map((c, i) => `<td${i >= numericFrom ? ' class="n"' : ""}>${c === null ? '<span class="null">null</span>' : escapeHtml(c)}</td>`).join("")}</tr>`)
          .join("");
  return `<table><caption>${escapeHtml(caption)}</caption><thead><tr>${head}</tr></thead><tbody>${body}</tbody></table>`;
}

const STYLE = `
:root{color-scheme:light dark;--bg:#fbfbfa;--fg:#1d1d1f;--muted:#6b6b70;--line:#d9d9dc;--card:#fff;--accent:#2a6f4e}
@media (prefers-color-scheme:dark){:root{--bg:#141416;--fg:#e8e8ea;--muted:#9a9aa1;--line:#34343a;--card:#1c1c20;--accent:#7cc7a0}}
*{box-sizing:border-box}
body{margin:0;padding:16px;background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,sans-serif}
main{max-width:1200px;margin:0 auto}
h1{font-size:20px;margin:0 0 12px}h2{font-size:16px;margin:28px 0 8px;border-bottom:1px solid var(--line);padding-bottom:4px}
a{color:var(--accent)}
form{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-bottom:16px}
input,select,button{font:inherit;padding:4px 8px;border:1px solid var(--line);border-radius:6px;background:var(--card);color:var(--fg)}
.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:8px}
.card{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:10px 12px}
.card b{display:block;font-size:22px;font-variant-numeric:tabular-nums}
.card span{color:var(--muted);font-size:12px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:12px}
.scroll{overflow-x:auto}
table{border-collapse:collapse;width:100%;background:var(--card);border:1px solid var(--line);border-radius:8px}
caption{text-align:left;font-weight:600;padding:4px 0}
th,td{padding:4px 8px;border-bottom:1px solid var(--line);text-align:left;vertical-align:top}
th{font-size:12px;color:var(--muted);font-weight:600}
.n{text-align:right;font-variant-numeric:tabular-nums;white-space:nowrap}
.empty,.null,.note{color:var(--muted)}
.comment{white-space:pre-wrap;overflow-wrap:anywhere;max-width:60ch}
code{overflow-wrap:anywhere}
`;

async function all<T>(db: D1Database, sql: string, params: (string | number)[]): Promise<T[]> {
  return (await db.prepare(sql).bind(...params).all<T>()).results;
}

// Grouped counts of one sessions column (or expression), most frequent first.
async function countBy(db: D1Database, f: Filters, expr: string, limit: number): Promise<Cell[][]> {
  const w = sessionsWhere(f);
  const rows = await all<{ k: string | null; n: number }>(
    db,
    `SELECT ${expr} AS k, COUNT(*) AS n FROM sessions WHERE ${w.sql} GROUP BY k ORDER BY n DESC, k LIMIT ${limit}`,
    w.params,
  );
  return rows.map((r) => [r.k, r.n]);
}

interface PerfRow {
  version: string;
  gpu_name: string | null;
  gpu_vendor: string | null;
  fps_mean: number | null;
  fps_low: number | null;
  worst_ms: number | null;
}

function perfTable(caption: string, label: string, rows: PerfRow[], keyOf: (r: PerfRow) => string, limit: number): string {
  const groups = new Map<string, { n: number; mean: number[]; low: number[]; worst: number[] }>();
  for (const r of rows) {
    const key = keyOf(r);
    let g = groups.get(key);
    if (!g) groups.set(key, (g = { n: 0, mean: [], low: [], worst: [] }));
    g.n++;
    if (r.fps_mean !== null) g.mean.push(r.fps_mean);
    if (r.fps_low !== null) g.low.push(r.fps_low);
    if (r.worst_ms !== null) g.worst.push(r.worst_ms);
  }
  const asc = (a: number, b: number) => a - b;
  const out = [...groups.entries()]
    .sort((a, b) => b[1].n - a[1].n || a[0].localeCompare(b[0]))
    .slice(0, limit)
    .map(([key, g]): Cell[] => [
      key,
      g.n,
      fmt(median(g.mean.sort(asc))),
      fmt(median(g.low.sort(asc))),
      fmt(quantile(g.worst.sort(asc), 0.95)),
    ]);
  return table(caption, [label, "sessions", "median fps_mean", "median fps_low_1pct", "p95 worst_ms"], out);
}

function gpuLabel(name: string | null, vendor: string | null): string {
  return `${name ?? "(unknown)"}${vendor ? ` [${vendor}]` : ""}`;
}

export async function renderPanel(request: Request, env: Env, now: number): Promise<Response> {
  const url = new URL(request.url);
  const f = filtersOf(url, now);
  const db = env.DB;
  const sw = sessionsWhere(f);
  const fw = feedbackWhere(f);

  const [head, comments, versions, perfRows, fatalRows, recent, versionChoices] = await Promise.all([
    db
      .prepare(
        `SELECT COUNT(*) AS sessions,
                SUM(CASE WHEN session_end <> 'normal' THEN 1 ELSE 0 END) AS abnormal
         FROM sessions WHERE ${sw.sql}`,
      )
      .bind(...sw.params)
      .first<{ sessions: number; abnormal: number | null }>(),
    db
      .prepare(`SELECT COUNT(*) AS n FROM feedback WHERE ${fw.sql}`)
      .bind(...fw.params)
      .first<{ n: number }>(),
    countBy(db, f, "version", 50),
    all<PerfRow>(
      db,
      `SELECT version, gpu_name, gpu_vendor, frametime_fps_mean AS fps_mean, frametime_fps_low_1pct AS fps_low,
              frametime_worst_ms AS worst_ms
       FROM sessions WHERE ${sw.sql} ORDER BY received_at DESC LIMIT ${STATS_ROW_CAP}`,
      sw.params,
    ),
    all<{ fatal: string; version: string; received_at: number }>(
      db,
      `SELECT fatal, version, received_at FROM sessions WHERE ${sw.sql} AND fatal IS NOT NULL
       ORDER BY received_at DESC LIMIT ${STATS_ROW_CAP}`,
      sw.params,
    ),
    all<{
      id: number;
      received_at: number;
      version: string | null;
      launcher_version: string;
      gpu_name: string | null;
      gpu_vendor: string | null;
      text: string;
      has_log: number;
    }>(
      db,
      `SELECT id, received_at, version, launcher_version, gpu_name, gpu_vendor, text,
              log_tail IS NOT NULL AS has_log
       FROM feedback WHERE ${fw.sql} ORDER BY received_at DESC, id DESC LIMIT 50`,
      fw.params,
    ),
    all<{ version: string }>(db, "SELECT version FROM releases ORDER BY published_at DESC, version DESC LIMIT 100", []),
  ]);

  const sessions = head?.sessions ?? 0;
  const abnormalPct = sessions === 0 ? 0 : ((head?.abnormal ?? 0) * 100) / sessions;

  const [byGpu, byOs, byPreset, byAspect, byUpscaler, byFramegen, byMods, patchRows] = await Promise.all([
    (async () => {
      const rows = await all<{ name: string | null; vendor: string | null; n: number }>(
        db,
        `SELECT gpu_name AS name, gpu_vendor AS vendor, COUNT(*) AS n FROM sessions WHERE ${sw.sql}
         GROUP BY gpu_name, gpu_vendor ORDER BY n DESC, name LIMIT 20`,
        sw.params,
      );
      return rows.map((r): Cell[] => [r.name, r.vendor, r.n]);
    })(),
    (async () => {
      const rows = await all<{ family: string | null; name: string | null; wine: number | null; n: number }>(
        db,
        `SELECT os_family AS family, os_name AS name, os_wine AS wine, COUNT(*) AS n FROM sessions WHERE ${sw.sql}
         GROUP BY os_family, os_name, os_wine ORDER BY n DESC, name LIMIT 20`,
        sw.params,
      );
      return rows.map((r): Cell[] => [r.family, r.name, r.wine === null ? null : r.wine ? "yes" : "no", r.n]);
    })(),
    countBy(db, f, "settings_preset", 20),
    countBy(db, f, "display_game_aspect", 20),
    countBy(db, f, "settings_upscaler", 20),
    countBy(db, f, "settings_framegen", 20),
    countBy(
      db,
      f,
      `CASE WHEN mods_count IS NULL THEN NULL WHEN mods_count = 0 THEN 'none'
            WHEN mods_code > 0 THEN 'with code' ELSE 'without code' END`,
      10,
    ),
    all<{ patches: string; n: number }>(
      db,
      `SELECT patches, COUNT(*) AS n FROM sessions WHERE ${sw.sql} AND patches IS NOT NULL
       GROUP BY patches ORDER BY n DESC LIMIT ${STATS_ROW_CAP}`,
      sw.params,
    ),
  ]);

  // Sessions per patch set that was on: a session's sets are one comma-separated text.
  const patchCounts = new Map<string, number>();
  for (const r of patchRows) {
    for (const name of r.patches.split(",")) if (name) patchCounts.set(name, (patchCounts.get(name) ?? 0) + r.n);
  }
  const byPatch: Cell[][] = [...patchCounts].sort((a, b) => b[1] - a[1] || a[0].localeCompare(b[0]));

  // Fatal lines grouped by their normalized text.
  const fatalGroups = new Map<string, { n: number; versions: Set<string>; last: number }>();
  for (const r of fatalRows) {
    const key = normalizeFatal(r.fatal);
    let g = fatalGroups.get(key);
    if (!g) fatalGroups.set(key, (g = { n: 0, versions: new Set(), last: 0 }));
    g.n++;
    g.versions.add(r.version);
    g.last = Math.max(g.last, r.received_at);
  }
  const fatalTable = table(
    "Most common fatal lines",
    ["fatal (addresses and numbers as #)", "count", "versions", "last seen"],
    [...fatalGroups.entries()]
      .sort((a, b) => b[1].n - a[1].n || b[1].last - a[1].last)
      .slice(0, 30)
      .map(([line, g]): Cell[] => [line, g.n, [...g.versions].sort().join(", "), isoDate(g.last)]),
  );

  const q = query(f);
  const capNote = (n: number) =>
    n >= STATS_ROW_CAP ? `<p class="note">Computed from the newest ${STATS_ROW_CAP} rows only.</p>` : "";

  const commentRows = recent
    .map(
      (c) => `<tr>
  <td class="n">${escapeHtml(isoDate(c.received_at))}</td>
  <td>${escapeHtml(c.version ?? c.launcher_version)}</td>
  <td>${escapeHtml(c.gpu_name === null ? "" : gpuLabel(c.gpu_name, c.gpu_vendor))}</td>
  <td class="comment">${escapeHtml(c.text)}</td>
  <td>${c.has_log ? `<a href="/v1/admin/feedback/${encodeURIComponent(String(c.id))}/log">log</a>` : ""}</td>
</tr>`,
    )
    .join("");

  const dayOptions = ALLOWED_DAYS.map((d) => `<option value="${d}"${d === f.days ? " selected" : ""}>${d} days</option>`).join("");
  const versionOptions = ['<option value="">all versions</option>']
    .concat(
      versionChoices.map(
        (v) => `<option value="${escapeHtml(v.version)}"${v.version === f.version ? " selected" : ""}>${escapeHtml(v.version)}</option>`,
      ),
    )
    .join("");

  const html = `<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="robots" content="noindex, nofollow">
<title>NX2Saria telemetry</title><style>${STYLE}</style></head>
<body><main>
<h1>NX2Saria telemetry</h1>
<form method="get" action="/v1/admin">
  <label>Period <select name="days">${dayOptions}</select></label>
  <label>Version <select name="version">${versionOptions}</select></label>
  <button type="submit">Show</button>
  <a href="/v1/admin/sessions.csv?${escapeHtml(q)}">sessions.csv</a>
  <a href="/v1/admin/feedback.csv?${escapeHtml(q)}">feedback.csv</a>
</form>

<section class="cards">
  <div class="card"><b id="sessions">${sessions}</b><span>sessions</span></div>
  <div class="card"><b id="comments">${comments?.n ?? 0}</b><span>comments</span></div>
  <div class="card"><b id="abnormal">${abnormalPct.toFixed(1)}%</b><span>sessions with end other than normal</span></div>
</section>

<h2>Counts</h2>
<div class="grid">
<div class="scroll">${table("By version", ["version", "sessions"], versions)}</div>
<div class="scroll">${table("By GPU (top 20)", ["gpu", "vendor", "sessions"], byGpu, 2)}</div>
<div class="scroll">${table("By OS", ["family", "name", "wine", "sessions"], byOs, 3)}</div>
<div class="scroll">${table("By preset", ["preset", "sessions"], byPreset)}</div>
<div class="scroll">${table("By aspect ratio", ["game_aspect", "sessions"], byAspect)}</div>
<div class="scroll">${table("By upscaler", ["upscaler", "sessions"], byUpscaler)}</div>
<div class="scroll">${table("By frame generation", ["framegen", "sessions"], byFramegen)}</div>
<div class="scroll">${table("By mods", ["mods on", "sessions"], byMods)}</div>
<div class="scroll">${table("By patch set on", ["patch set", "sessions"], byPatch)}</div>
</div>

<h2>Performance</h2>
${capNote(perfRows.length)}
<div class="grid">
<div class="scroll">${perfTable("By version", "version", perfRows, (r) => r.version, 50)}</div>
<div class="scroll">${perfTable("By GPU (top 20)", "gpu", perfRows, (r) => gpuLabel(r.gpu_name, r.gpu_vendor), 20)}</div>
</div>

<h2>Fatal lines</h2>
${capNote(fatalRows.length)}
<div class="scroll">${fatalTable}</div>

<h2>Recent comments</h2>
<div class="scroll"><table><caption>The 50 newest</caption>
<thead><tr><th class="n">date (UTC)</th><th>version</th><th>gpu</th><th>text</th><th>log</th></tr></thead>
<tbody>${commentRows || '<tr><td colspan="5" class="empty">No data.</td></tr>'}</tbody></table></div>
</main></body></html>`;

  return respond(html, 200, "text/html; charset=utf-8");
}

// ---- CSV ----

// One CSV cell: always quoted, quotes doubled. A text cell that a spreadsheet would read as a formula (starting with
// = + - @, tab or carriage return) gets a leading apostrophe so it stays text.
export function csvCell(value: unknown): string {
  if (value === null || value === undefined) return "";
  let text = String(value);
  if (typeof value === "string" && /^[=+\-@\t\r]/.test(text)) text = `'${text}`;
  return `"${text.replace(/"/g, '""')}"`;
}

function csvResponse(name: string, headers: readonly string[], rows: Record<string, unknown>[]): Response {
  const lines = [headers.map(csvCell).join(",")];
  for (const row of rows) lines.push(headers.map((h) => csvCell(row[h])).join(","));
  return respond(`${lines.join("\r\n")}\r\n`, 200, "text/csv; charset=utf-8", {
    "Content-Disposition": `attachment; filename="${name}"`,
  });
}

const SESSION_CSV_COLUMNS = ["id", "received_at", "launcher_version", ...SUMMARY_COLUMNS] as const;

export async function sessionsCsv(request: Request, env: Env, now: number): Promise<Response> {
  const f = filtersOf(new URL(request.url), now);
  const w = sessionsWhere(f);
  const rows = await all<Record<string, unknown>>(
    env.DB,
    `SELECT ${SESSION_CSV_COLUMNS.join(", ")} FROM sessions WHERE ${w.sql} ORDER BY received_at DESC, id DESC LIMIT ${CSV_ROW_CAP}`,
    w.params,
  );
  for (const r of rows) r.received_at = isoDate(r.received_at as number);
  return csvResponse("sessions.csv", SESSION_CSV_COLUMNS, rows);
}

const FEEDBACK_CSV_COLUMNS = [
  "id",
  "received_at",
  "launcher_version",
  "version",
  "gpu_name",
  "gpu_vendor",
  "os_family",
  "text",
  "has_log",
  "summary_json",
] as const;

export async function feedbackCsv(request: Request, env: Env, now: number): Promise<Response> {
  const f = filtersOf(new URL(request.url), now);
  const w = feedbackWhere(f);
  const rows = await all<Record<string, unknown>>(
    env.DB,
    `SELECT id, received_at, launcher_version, version, gpu_name, gpu_vendor, os_family, text,
            log_tail IS NOT NULL AS has_log, summary_json
     FROM feedback WHERE ${w.sql} ORDER BY received_at DESC, id DESC LIMIT ${CSV_ROW_CAP}`,
    w.params,
  );
  for (const r of rows) r.received_at = isoDate(r.received_at as number);
  return csvResponse("feedback.csv", FEEDBACK_CSV_COLUMNS, rows);
}

// ---- Log view ----

export async function feedbackLog(env: Env, id: number): Promise<Response> {
  const row = await env.DB.prepare("SELECT log_tail FROM feedback WHERE id = ?").bind(id).first<{ log_tail: string | null }>();
  if (row === null || row.log_tail === null) return error(404, "not found");
  return respond(row.log_tail, 200, "text/plain; charset=utf-8");
}
