// Shared test fixtures: a valid summary, request builders and database resets.
import { env } from "cloudflare:workers";
import worker from "../src/index";
import type { Env } from "../src/env";

export const BASE = "https://telemetry.example";
export const RELEASE = "0.19.0-rc.2";
export const OLD_RELEASE = "0.18.7";
export const ADMIN_TOKEN = "test-admin-token";

// A fresh lowercase uuid4 for each call, so tests never share rate counters by accident.
export function newInstallId(): string {
  return crypto.randomUUID();
}

export function validSummary(): Record<string, any> {
  return {
    schema: "nx2saria.session/1",
    version: RELEASE,
    game_version: "1.1.1",
    os: { family: "windows", name: "Windows 11 (build 26100)", wine: false },
    cpu: { name: "AMD Ryzen 7 7800X3D 8-Core Processor", threads: 16 },
    ram_mb: 32768,
    gpu: { name: "NVIDIA GeForce RTX 4070", vendor: "0x10de", device: "0x2786", driver: "NVIDIA 581.57", vram_mb: 12282 },
    display: { output: "2560x1440", render_scale: "game", fullscreen: true, game_aspect: "16:9" },
    settings: { preset: null, upscaler: "fsr", taa: "dlss", dlss: "dlaa", framegen: "off", fps_limit: 60, vk_present: "fifo" },
    session: { duration_s: 1834, frames: 109800, end: "normal" },
    frametime: { fps_mean: 59.9, fps_low_1pct: 52.1, worst_ms: 88.4, over_25ms: 14, over_100ms: 0 },
    startup: { shader_prewarm_ms: 5400, shaders_translated: 120, pipelines_built: 900, pipeline_build_ms: 3100 },
    fatal: null,
  };
}

// The summary the launcher builds after a hard crash: every leaf null except the required ones and fatal.
export function crashSummary(): Record<string, any> {
  return {
    schema: "nx2saria.session/1",
    version: RELEASE,
    game_version: "1.1.1",
    os: { family: "linux", name: null, wine: null },
    cpu: { name: null, threads: null },
    ram_mb: null,
    gpu: { name: null, vendor: null, device: null, driver: null, vram_mb: null },
    display: { output: null, render_scale: null, fullscreen: null, game_aspect: null },
    settings: { preset: null, upscaler: null, taa: null, dlss: null, framegen: null, fps_limit: null, vk_present: null },
    session: { duration_s: null, frames: null, end: "crash" },
    frametime: { fps_mean: null, fps_low_1pct: null, worst_ms: null, over_25ms: null, over_100ms: null },
    startup: { shader_prewarm_ms: null, shaders_translated: null, pipelines_built: null, pipeline_build_ms: null },
    fatal: "access violation at 0x00007ff6a1b2c3d4 in module main+0x1234",
  };
}

export function sessionBody(installId = newInstallId(), summary: Record<string, any> = validSummary()): Record<string, any> {
  return { install_id: installId, launcher_version: RELEASE, summary };
}

export function feedbackBody(installId = newInstallId(), extra: Record<string, any> = {}): Record<string, any> {
  return { install_id: installId, launcher_version: RELEASE, text: "Runs well on my PC.", summary: null, log_tail: null, ...extra };
}

export function testEnv(overrides: Partial<Env> = {}): Env {
  return { ...(env as unknown as Env), ...overrides };
}

// A made-up client address per request unless the test gives one, so the per-IP limit never joins unrelated tests.
let ipCounter = 0;
export function newIp(): string {
  ipCounter++;
  return `198.51.${(ipCounter >> 8) & 255}.${ipCounter & 255}`;
}

export async function call(request: Request, overrides: Partial<Env> = {}): Promise<Response> {
  if (!request.headers.has("CF-Connecting-IP")) {
    request = new Request(request, { headers: new Headers(request.headers) });
    request.headers.set("CF-Connecting-IP", newIp());
  }
  return worker.fetch(request, testEnv(overrides));
}

export function postJson(
  path: string,
  body: unknown,
  options: { country?: string; contentType?: string; raw?: string; overrides?: Partial<Env>; ip?: string } = {},
): Promise<Response> {
  const text = options.raw ?? JSON.stringify(body);
  const request = new Request(`${BASE}${path}`, {
    method: "POST",
    headers: {
      "Content-Type": options.contentType ?? "application/json",
      ...(options.ip === undefined ? {} : { "CF-Connecting-IP": options.ip }),
    },
    body: text,
    cf: options.country === undefined ? undefined : ({ country: options.country } as unknown as IncomingRequestCfProperties),
  });
  return call(request, options.overrides);
}

export function adminGet(path: string, password: string | null = ADMIN_TOKEN, overrides: Partial<Env> = {}): Promise<Response> {
  const headers: Record<string, string> = {};
  if (password !== null) headers.Authorization = `Basic ${btoa(`admin:${password}`)}`;
  return call(new Request(`${BASE}${path}`, { headers }), overrides);
}

export async function resetDb(): Promise<void> {
  await env.DB.batch([
    env.DB.prepare("DELETE FROM sessions"),
    env.DB.prepare("DELETE FROM feedback"),
    env.DB.prepare("DELETE FROM rate"),
    env.DB.prepare("DELETE FROM releases"),
    // the on-demand releases refresh stays off (a refresh "just now" far in the future); test/cron.test.ts clears it
    env.DB.prepare("INSERT OR REPLACE INTO meta (key, value) VALUES ('releases_refresh', 4000000000)"),
    env.DB.prepare("INSERT INTO releases (version, published_at) VALUES (?, ?), (?, ?)").bind(
      RELEASE,
      "2026-11-18T12:00:00Z",
      OLD_RELEASE,
      "2026-10-01T12:00:00Z",
    ),
  ]);
}

export async function count(table: string, where = "1 = 1", ...params: unknown[]): Promise<number> {
  const row = await env.DB.prepare(`SELECT COUNT(*) AS n FROM ${table} WHERE ${where}`)
    .bind(...params)
    .first<{ n: number }>();
  return row?.n ?? 0;
}
