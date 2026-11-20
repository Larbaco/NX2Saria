// POST /v1/session: the strict schema, body limits, content type, release lookup, rate limits and the kill switch.
import { env } from "cloudflare:workers";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { BASE, OLD_RELEASE, RELEASE, call, count, crashSummary, newInstallId, postJson, resetDb, sessionBody, validSummary } from "./helpers";

beforeEach(resetDb);

async function expect400(body: unknown, messagePart?: string): Promise<void> {
  const response = await postJson("/v1/session", body);
  expect(response.status).toBe(400);
  const json = await response.json<{ error: string }>();
  if (messagePart) expect(json.error).toContain(messagePart);
  expect(await count("sessions")).toBe(0);
}

// Returns a valid session body with one change applied to its summary.
function withSummary(change: (s: Record<string, any>) => void): Record<string, any> {
  const s = validSummary();
  change(s);
  return sessionBody(newInstallId(), s);
}

describe("accepted reports", () => {
  it("stores a valid summary with every leaf in its column", async () => {
    const id = newInstallId();
    const response = await postJson("/v1/session", sessionBody(id), { country: "BR" });
    expect(response.status).toBe(201);
    expect(await response.json()).toEqual({ ok: true });
    const row = await env.DB.prepare("SELECT * FROM sessions WHERE install_id = ?").bind(id).first<Record<string, unknown>>();
    expect(row).toMatchObject({
      install_id: id,
      launcher_version: RELEASE,
      country: "BR",
      schema: "nx2saria.session/1",
      version: RELEASE,
      game_version: "1.1.1",
      os_family: "windows",
      os_wine: 0,
      cpu_threads: 16,
      gpu_vendor: "0x10de",
      gpu_device: "0x2786",
      display_output: "2560x1440",
      display_fullscreen: 1,
      display_game_aspect: "16:10",
      display_picture: "2304x1440",
      settings_preset: "high",
      settings_fps_limit: 60,
      session_end: "normal",
      frametime_fps_mean: 59.9,
      frametime_worst_ms: 88.4,
      startup_pipeline_build_ms: 3100,
      fatal: null,
    });
    expect(typeof row!.received_at).toBe("number");
    // The row has no column that could hold an address or a header.
    expect(Object.keys(row!).some((k) => /(^|_)ip(_|$)|addr|header|agent/i.test(k))).toBe(false);
  });

  it("accepts the launcher's crash summary with null leaves", async () => {
    const response = await postJson("/v1/session", sessionBody(newInstallId(), crashSummary()));
    expect(response.status).toBe(201);
    expect(await count("sessions", "session_end = 'crash' AND gpu_name IS NULL")).toBe(1);
  });

  it("records XX when Cloudflare gives no usable country", async () => {
    await postJson("/v1/session", sessionBody(), {});
    await postJson("/v1/session", sessionBody(), { country: "T1" });
    await postJson("/v1/session", sessionBody(), { country: "br" });
    expect(await count("sessions", "country = 'XX'")).toBe(3);
  });

  it("accepts an older published release and Content-Type parameters", async () => {
    const body = sessionBody();
    body.launcher_version = OLD_RELEASE;
    const response = await postJson("/v1/session", body, { contentType: "application/json; charset=utf-8" });
    expect(response.status).toBe(201);
  });
});

describe("schema rejections", () => {
  it("unknown top-level key", () => expect400({ ...sessionBody(), extra: 1 }, "unknown key extra"));
  it("unknown nested key", () => expect400(withSummary((s) => (s.gpu.serial = "x")), "unknown key summary.gpu.serial"));
  it("unknown summary key", () => expect400(withSummary((s) => (s.hostname = "pc")), "unknown key summary.hostname"));
  it("missing key", () => expect400(withSummary((s) => delete s.startup), "missing key summary.startup"));
  it("missing nested key", () => expect400(withSummary((s) => delete s.os.wine), "missing key summary.os.wine"));
  it("object replaced by null", () => expect400(withSummary((s) => (s.gpu = null)), "summary.gpu must be an object"));
  it("array instead of object", () => expect400(withSummary((s) => (s.cpu = [])), "summary.cpu must be an object"));
  it("body that is an array", () => expect400([sessionBody()], "JSON object"));
  it("body that is not JSON", async () => {
    const response = await postJson("/v1/session", null, { raw: "{not json" });
    expect(response.status).toBe(400);
  });

  it("required leaves may not be null", async () => {
    await expect400(withSummary((s) => (s.session.end = null)), "summary.session.end");
    await expect400(withSummary((s) => (s.version = null)), "summary.version");
    await expect400(withSummary((s) => (s.game_version = null)), "summary.game_version");
  });

  it("wrong types", async () => {
    await expect400(withSummary((s) => (s.cpu.threads = "16")), "summary.cpu.threads");
    await expect400(withSummary((s) => (s.cpu.threads = 16.5)), "summary.cpu.threads");
    await expect400(withSummary((s) => (s.ram_mb = -1)), "summary.ram_mb");
    await expect400(withSummary((s) => (s.os.wine = "no")), "summary.os.wine");
    await expect400(withSummary((s) => (s.gpu.name = 4070)), "summary.gpu.name");
    await expect400(withSummary((s) => (s.frametime.fps_mean = "59.9")), "summary.frametime.fps_mean");
    await expect400({ ...sessionBody(), install_id: 5 }, "install_id");
  });

  it("number ranges and decimals", async () => {
    await expect400(withSummary((s) => (s.frametime.fps_mean = 59.95)), "more than one decimal");
    await expect400(withSummary((s) => (s.frametime.fps_mean = 2000.1)), "out of range");
    await expect400(withSummary((s) => (s.frametime.worst_ms = 600000.5)), "out of range");
    await expect400(withSummary((s) => (s.session.duration_s = 30 * 86400 + 1)), "out of range");
    await expect400(withSummary((s) => (s.cpu.threads = 1025)), "out of range");
    await expect400(withSummary((s) => (s.gpu.vram_mb = 4194305)), "out of range");
  });

  it("strings over their caps", async () => {
    await expect400(withSummary((s) => (s.gpu.name = "N".repeat(65))), "summary.gpu.name is too long");
    await expect400(withSummary((s) => (s.os.name = "W".repeat(65))), "summary.os.name is too long");
    await expect400(withSummary((s) => (s.fatal = "f".repeat(301))), "summary.fatal is too long");
    await expect400(withSummary((s) => (s.settings.upscaler = "a".repeat(33))), "summary.settings.upscaler is too long");
  });

  it("caps count characters, not bytes", async () => {
    const response = await postJson("/v1/session", withSummary((s) => (s.gpu.name = "é".repeat(64))));
    expect(response.status).toBe(201);
  });

  it("characters outside a field's set", async () => {
    await expect400(withSummary((s) => (s.settings.upscaler = "fsr 3")), "outside its set");
    await expect400(withSummary((s) => (s.display.output = "1920×1080")), "outside its set");
    await expect400(withSummary((s) => (s.gpu.vendor = "0x10DE")), "outside its set");
    await expect400(withSummary((s) => (s.gpu.device = "10de")), "outside its set");
    await expect400(withSummary((s) => (s.cpu.name = "AMD\nRyzen")), "control character");
    await expect400(withSummary((s) => (s.fatal = "bad\u0007bell")), "control character");
    await expect400(withSummary((s) => (s.gpu.driver = "x\ud800")), "control character");
    await expect400({ ...sessionBody(), install_id: "6F9619FF-8B86-4D01-B42D-00CF4FC964FF" }, "uuid4");
    await expect400({ ...sessionBody(), install_id: "6f9619ff-8b86-1d01-b42d-00cf4fc964ff" }, "uuid4");
  });

  it("values outside their lists", async () => {
    await expect400(withSummary((s) => (s.os.family = "macos")), "summary.os.family");
    await expect400(withSummary((s) => (s.session.end = "quit")), "summary.session.end");
    await expect400(withSummary((s) => (s.schema = "nx2saria.session/2")), "summary.schema");
  });

  it("wrong game_version", () => expect400(withSummary((s) => (s.game_version = "1.0.0")), "game_version is not supported"));

  it("unknown versions", async () => {
    await expect400(withSummary((s) => (s.version = "9.9.9")), "unknown version");
    await expect400({ ...sessionBody(), launcher_version: "9.9.9" }, "unknown version");
    await expect400({ ...sessionBody(), launcher_version: "v0.19.0-rc.2" }, "unknown version");
  });

  it("GAME_VERSIONS is a comma list", async () => {
    const body = withSummary((s) => (s.game_version = "1.2.0"));
    expect((await postJson("/v1/session", body)).status).toBe(400);
    const open = await postJson("/v1/session", body, { overrides: { GAME_VERSIONS: "1.1.1, 1.2.0" } });
    expect(open.status).toBe(201);
  });
});

describe("transport", () => {
  it("415 without application/json", async () => {
    expect((await postJson("/v1/session", sessionBody(), { contentType: "text/plain" })).status).toBe(415);
    const noType = new Request(`${BASE}/v1/session`, { method: "POST", body: new Blob([JSON.stringify(sessionBody())]) });
    expect((await call(noType)).status).toBe(415);
  });

  it("413 for a body over 8 KB, read from the bytes", async () => {
    const body = sessionBody();
    const raw = JSON.stringify(body) + " ".repeat(8 * 1024);
    const response = await postJson("/v1/session", null, { raw });
    expect(response.status).toBe(413);
  });

  it("413 from Content-Length before reading", async () => {
    const request = new Request(`${BASE}/v1/session`, {
      method: "POST",
      headers: { "Content-Type": "application/json", "Content-Length": "999999" },
      body: "{}",
    });
    expect((await call(request)).status).toBe(413);
  });

  it("413 for a streamed body without Content-Length", async () => {
    const chunk = new TextEncoder().encode("x".repeat(4096));
    let sent = 0;
    const stream = new ReadableStream<Uint8Array>({
      pull(controller) {
        if (sent++ < 4) controller.enqueue(chunk);
        else controller.close();
      },
    });
    const request = new Request(`${BASE}/v1/session`, { method: "POST", headers: { "Content-Type": "application/json" }, body: stream });
    expect((await call(request)).status).toBe(413);
  });

  it("400 for invalid UTF-8", async () => {
    const request = new Request(`${BASE}/v1/session`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: new Uint8Array([0x7b, 0xff, 0x7d]),
    });
    expect((await call(request)).status).toBe(400);
  });

  it("405 for other methods and 404 for unknown routes", async () => {
    const get = await call(new Request(`${BASE}/v1/session`));
    expect(get.status).toBe(405);
    expect(get.headers.get("Allow")).toBe("POST");
    expect((await call(new Request(`${BASE}/v1/sessions`, { method: "POST" }))).status).toBe(404);
    expect((await call(new Request(`${BASE}/`))).status).toBe(404);
  });
});

describe("rate limits", () => {
  // Pin the clock to the middle of a minute so a test never straddles two minute windows.
  beforeEach(() => {
    vi.useFakeTimers({ toFake: ["Date"] });
    vi.setSystemTime(new Date("2026-11-19T12:00:30Z"));
  });
  afterEach(() => vi.useRealTimers());

  it("6 sessions per minute per install id", async () => {
    const id = newInstallId();
    for (let i = 0; i < 6; i++) expect((await postJson("/v1/session", sessionBody(id))).status).toBe(201);
    expect((await postJson("/v1/session", sessionBody(id))).status).toBe(429);
    // Another install id is not affected.
    expect((await postJson("/v1/session", sessionBody())).status).toBe(201);
    expect(await count("sessions", "install_id = ?", id)).toBe(6);
  });

  it("60 sessions per day per install id", async () => {
    const id = newInstallId();
    const now = Math.floor(Date.now() / 1000);
    const day = now - (now % 86400);
    await env.DB.prepare(`INSERT INTO rate (install_id, kind, "window", count) VALUES (?, 'session_day', ?, 60)`).bind(id, day).run();
    expect((await postJson("/v1/session", sessionBody(id))).status).toBe(429);
    await env.DB.prepare(`UPDATE rate SET count = 59 WHERE install_id = ?`).bind(id).run();
    expect((await postJson("/v1/session", sessionBody(id))).status).toBe(201);
    expect((await postJson("/v1/session", sessionBody(id))).status).toBe(429);
  });

  it("a rejected body does not count", async () => {
    const id = newInstallId();
    for (let i = 0; i < 10; i++) await postJson("/v1/session", { ...sessionBody(id), extra: true });
    expect(await count("rate", "install_id = ?", id)).toBe(0);
  });
});

describe("kill switch", () => {
  it("410 closed on POST when TELEMETRY_OPEN is 0", async () => {
    const response = await postJson("/v1/session", sessionBody(), { overrides: { TELEMETRY_OPEN: "0" } });
    expect(response.status).toBe(410);
    expect(await response.json()).toEqual({ error: "closed" });
    const feedback = await postJson("/v1/feedback", {}, { overrides: { TELEMETRY_OPEN: "0" } });
    expect(feedback.status).toBe(410);
    expect(await count("sessions")).toBe(0);
  });
});

describe("per-IP limit", () => {
  it("answers 429 past 20 requests a minute from one address, whatever the install ids, and stores nothing then", async () => {
    const ip = "203.0.113.7";
    for (let i = 0; i < 20; i++) {
      expect((await postJson("/v1/session", sessionBody(), { ip })).status).toBe(201);
    }
    const id = newInstallId();
    const limited = await postJson("/v1/session", sessionBody(id), { ip });
    expect(limited.status).toBe(429);
    expect(await limited.json()).toEqual({ error: "rate limited" });
    expect(await count("sessions", "install_id = ?", id)).toBe(0);
    // DELETE counts against the same address; another address is not affected
    const del = new Request(`${BASE}/v1/install/${newInstallId()}`, { method: "DELETE", headers: { "CF-Connecting-IP": ip } });
    expect((await call(del)).status).toBe(429);
    expect((await postJson("/v1/session", sessionBody(), { ip: "203.0.113.8" })).status).toBe(201);
    // the address is not written anywhere
    const rows = await env.DB.prepare("SELECT * FROM sessions").all();
    expect(JSON.stringify(rows.results)).not.toContain("203.0.113");
  });
});
