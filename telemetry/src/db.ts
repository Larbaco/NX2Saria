// Every D1 statement of the public routes and the crons: storing reports and comments, rate counters, deletion by
// install id, retention, and the releases table.

import type { Summary } from "./validate";
import { VERSION_RE } from "./validate";

export const RETENTION_DAYS = 180;
const DAY_S = 86400;

export function nowSeconds(): number {
  return Math.floor(Date.now() / 1000);
}

// SQLite has no boolean: true/false become 1/0, null stays null.
function flag(value: boolean | null): number | null {
  return value === null ? null : value ? 1 : 0;
}

// The sessions columns, in the order flattenSummary returns their values.
export const SUMMARY_COLUMNS = [
  "schema",
  "version",
  "game_version",
  "os_family",
  "os_name",
  "os_wine",
  "cpu_name",
  "cpu_threads",
  "ram_mb",
  "gpu_name",
  "gpu_vendor",
  "gpu_device",
  "gpu_driver",
  "gpu_vram_mb",
  "display_output",
  "display_render_scale",
  "display_fullscreen",
  "display_game_aspect",
  "settings_preset",
  "settings_upscaler",
  "settings_taa",
  "settings_dlss",
  "settings_framegen",
  "settings_fps_limit",
  "settings_vk_present",
  "session_duration_s",
  "session_frames",
  "session_end",
  "frametime_fps_mean",
  "frametime_fps_low_1pct",
  "frametime_worst_ms",
  "frametime_over_25ms",
  "frametime_over_100ms",
  "startup_shader_prewarm_ms",
  "startup_shaders_translated",
  "startup_pipelines_built",
  "startup_pipeline_build_ms",
  "fatal",
] as const;

function flattenSummary(s: Summary): (string | number | null)[] {
  return [
    s.schema,
    s.version,
    s.game_version,
    s.os.family,
    s.os.name,
    flag(s.os.wine),
    s.cpu.name,
    s.cpu.threads,
    s.ram_mb,
    s.gpu.name,
    s.gpu.vendor,
    s.gpu.device,
    s.gpu.driver,
    s.gpu.vram_mb,
    s.display.output,
    s.display.render_scale,
    flag(s.display.fullscreen),
    s.display.game_aspect,
    s.settings.preset,
    s.settings.upscaler,
    s.settings.taa,
    s.settings.dlss,
    s.settings.framegen,
    s.settings.fps_limit,
    s.settings.vk_present,
    s.session.duration_s,
    s.session.frames,
    s.session.end,
    s.frametime.fps_mean,
    s.frametime.fps_low_1pct,
    s.frametime.worst_ms,
    s.frametime.over_25ms,
    s.frametime.over_100ms,
    s.startup.shader_prewarm_ms,
    s.startup.shaders_translated,
    s.startup.pipelines_built,
    s.startup.pipeline_build_ms,
    s.fatal,
  ];
}

const INSERT_SESSION_SQL = `INSERT INTO sessions (install_id, launcher_version, country, received_at, ${SUMMARY_COLUMNS.join(", ")})
  VALUES (${new Array(4 + SUMMARY_COLUMNS.length).fill("?").join(", ")})`;

export function insertSession(db: D1Database, installId: string, launcherVersion: string, country: string, now: number, summary: Summary): D1PreparedStatement {
  return db.prepare(INSERT_SESSION_SQL).bind(installId, launcherVersion, country, now, ...flattenSummary(summary));
}

export function insertFeedback(
  db: D1Database,
  row: { installId: string; launcherVersion: string; text: string; summary: Summary | null; logTail: string | null; country: string; now: number },
): D1PreparedStatement {
  const s = row.summary;
  return db
    .prepare(
      `INSERT INTO feedback (install_id, launcher_version, text, summary_json, log_tail, country, received_at, version, gpu_name, gpu_vendor, os_family)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
    )
    .bind(
      row.installId,
      row.launcherVersion,
      row.text,
      s === null ? null : JSON.stringify(s),
      row.logTail,
      row.country,
      row.now,
      s?.version ?? null,
      s?.gpu.name ?? null,
      s?.gpu.vendor ?? null,
      s?.os.family ?? null,
    );
}

// True when every version in the list is a row of the releases table.
export async function areReleases(db: D1Database, versions: readonly string[]): Promise<boolean> {
  const unique = [...new Set(versions)];
  const placeholders = unique.map(() => "?").join(", ");
  const row = await db
    .prepare(`SELECT COUNT(*) AS n FROM releases WHERE version IN (${placeholders})`)
    .bind(...unique)
    .first<{ n: number }>();
  return (row?.n ?? 0) === unique.length;
}

// Seconds between two on-demand releases refreshes (a report naming a version the table does not hold yet).
export const RELEASES_REFRESH_GAP_S = 300;

// Takes the on-demand refresh's turn: true for the one caller that may call the GitHub API now, false while the last
// refresh is less than RELEASES_REFRESH_GAP_S old. One conditional upsert, so concurrent requests cannot both win.
export async function claimReleasesRefresh(db: D1Database, now: number): Promise<boolean> {
  const result = await db
    .prepare(
      `INSERT INTO meta (key, value) VALUES ('releases_refresh', ?1)
       ON CONFLICT (key) DO UPDATE SET value = excluded.value WHERE meta.value <= ?2`,
    )
    .bind(now, now - RELEASES_REFRESH_GAP_S)
    .run();
  return (result.meta.changes ?? 0) > 0;
}

export type RateKind = "session" | "feedback";

export const RATE_LIMITS: Record<RateKind, { perMinute: number; perDay: number }> = {
  session: { perMinute: 6, perDay: 60 },
  feedback: { perMinute: 2, perDay: 10 },
};

function rateWindows(now: number): { minute: number; day: number } {
  return { minute: now - (now % 60), day: now - (now % DAY_S) };
}

// True when one more POST of this kind would pass the minute or the day limit of this install id. Only accepted
// POSTs are counted (rateIncrements runs in the same batch as the insert), so a client that keeps retrying while
// limited does not use up its own daily budget.
export async function isRateLimited(db: D1Database, installId: string, kind: RateKind, now: number): Promise<boolean> {
  const w = rateWindows(now);
  const { results } = await db
    .prepare(
      `SELECT kind, count FROM rate WHERE install_id = ?
         AND ((kind = ? AND "window" = ?) OR (kind = ? AND "window" = ?))`,
    )
    .bind(installId, `${kind}_min`, w.minute, `${kind}_day`, w.day)
    .all<{ kind: string; count: number }>();
  const limits = RATE_LIMITS[kind];
  for (const row of results) {
    if (row.kind === `${kind}_min` && row.count >= limits.perMinute) return true;
    if (row.kind === `${kind}_day` && row.count >= limits.perDay) return true;
  }
  return false;
}

export function rateIncrements(db: D1Database, installId: string, kind: RateKind, now: number): D1PreparedStatement[] {
  const w = rateWindows(now);
  const sql = `INSERT INTO rate (install_id, kind, "window", count) VALUES (?, ?, ?, 1)
    ON CONFLICT (install_id, kind, "window") DO UPDATE SET count = count + 1`;
  return [db.prepare(sql).bind(installId, `${kind}_min`, w.minute), db.prepare(sql).bind(installId, `${kind}_day`, w.day)];
}

// Removes everything stored under one install id: its sessions, its comments and its rate counters.
export async function deleteInstall(db: D1Database, installId: string): Promise<void> {
  await db.batch([
    db.prepare("DELETE FROM sessions WHERE install_id = ?").bind(installId),
    db.prepare("DELETE FROM feedback WHERE install_id = ?").bind(installId),
    db.prepare("DELETE FROM rate WHERE install_id = ?").bind(installId),
  ]);
}

// Deletes sessions and comments older than RETENTION_DAYS, and rate counters whose window ended over a day ago.
export async function purgeOld(db: D1Database, now: number): Promise<void> {
  const cutoff = now - RETENTION_DAYS * DAY_S;
  await db.batch([
    db.prepare("DELETE FROM sessions WHERE received_at < ?").bind(cutoff),
    db.prepare("DELETE FROM feedback WHERE received_at < ?").bind(cutoff),
    db.prepare('DELETE FROM rate WHERE "window" < ?').bind(now - 2 * DAY_S),
  ]);
}

interface GitHubRelease {
  tag_name?: unknown;
  draft?: unknown;
  published_at?: unknown;
}

// The releases of a GitHub API listing as [version, published_at] pairs: drafts skipped, the tag's leading "v"
// removed, anything that is not a version string dropped.
export function releasesFromListing(listing: unknown): [string, string | null][] {
  if (!Array.isArray(listing)) throw new Error("releases listing is not an array");
  const out: [string, string | null][] = [];
  for (const item of listing as GitHubRelease[]) {
    if (typeof item !== "object" || item === null || item.draft === true) continue;
    if (typeof item.tag_name !== "string") continue;
    const version = item.tag_name.replace(/^v/, "");
    if (!VERSION_RE.test(version)) continue;
    const published = typeof item.published_at === "string" && item.published_at.length <= 40 ? item.published_at : null;
    out.push([version, published]);
  }
  return out;
}

// Fetches the newest 100 releases of RELEASES_REPO and upserts them. Releases missing from the listing (older ones)
// stay in the table, so a version once published keeps being accepted. Returns how many rows were written.
export async function refreshReleases(db: D1Database, repo: string): Promise<number> {
  if (!/^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/.test(repo)) throw new Error("RELEASES_REPO is not owner/repo");
  const response = await fetch(`https://api.github.com/repos/${repo}/releases?per_page=100`, {
    headers: {
      "User-Agent": "NX2Saria-Telemetry",
      Accept: "application/vnd.github+json",
      "X-GitHub-Api-Version": "2022-11-28",
    },
  });
  if (!response.ok) throw new Error(`GitHub releases API answered ${response.status}`);
  const releases = releasesFromListing(await response.json());
  if (releases.length === 0) return 0;
  const sql = `INSERT INTO releases (version, published_at) VALUES (?, ?)
    ON CONFLICT (version) DO UPDATE SET published_at = excluded.published_at`;
  await db.batch(releases.map(([version, published]) => db.prepare(sql).bind(version, published)));
  return releases.length;
}
