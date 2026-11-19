// POST /v1/feedback and DELETE /v1/install/<id>.
import { env } from "cloudflare:workers";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { BASE, RELEASE, call, count, feedbackBody, newInstallId, postJson, resetDb, sessionBody, validSummary } from "./helpers";

beforeEach(resetDb);

describe("feedback", () => {
  it("stores text, summary and log tail", async () => {
    const id = newInstallId();
    const logTail = "line one\nline two\twith tab\r\nline three\n";
    const response = await postJson(
      "/v1/feedback",
      feedbackBody(id, { text: "Olá! Water flickers.\nSecond line.", summary: validSummary(), log_tail: logTail }),
      { country: "PT" },
    );
    expect(response.status).toBe(201);
    const row = await env.DB.prepare("SELECT * FROM feedback WHERE install_id = ?").bind(id).first<Record<string, any>>();
    expect(row).toMatchObject({
      launcher_version: RELEASE,
      text: "Olá! Water flickers.\nSecond line.",
      log_tail: logTail,
      country: "PT",
      version: RELEASE,
      gpu_name: "NVIDIA GeForce RTX 4070",
      gpu_vendor: "0x10de",
      os_family: "windows",
    });
    expect(JSON.parse(row!.summary_json)).toEqual(validSummary());
  });

  it("accepts null summary and null log tail", async () => {
    expect((await postJson("/v1/feedback", feedbackBody())).status).toBe(201);
    expect(await count("feedback", "summary_json IS NULL AND log_tail IS NULL AND version IS NULL")).toBe(1);
  });

  it("rejects bad bodies", async () => {
    const cases: [Record<string, any>, string][] = [
      [{ ...feedbackBody(), email: "ana.souza@example.com" }, "unknown key email"],
      [feedbackBody(undefined, { text: "" }), "text is empty"],
      [feedbackBody(undefined, { text: "   \n " }), "text is empty"],
      [feedbackBody(undefined, { text: "x".repeat(4001) }), "text is too long"],
      [feedbackBody(undefined, { text: 42 }), "text must be a string"],
      [feedbackBody(undefined, { text: "bell\u0007" }), "control character"],
      [feedbackBody(undefined, { log_tail: "a\u0000b" }), "control character"],
      [feedbackBody(undefined, { log_tail: "line\n".repeat(201) }), "too many lines"],
      [feedbackBody(undefined, { log_tail: "y".repeat(24 * 1024 + 1) }), "log_tail is too long"],
      [feedbackBody(undefined, { log_tail: ["a"] }), "log_tail must be a string"],
      [feedbackBody(undefined, { summary: { ...validSummary(), extra: 1 } }), "unknown key summary.extra"],
      [feedbackBody(undefined, { summary: { ...validSummary(), game_version: "1.0.0" } }), "game_version"],
      [feedbackBody(undefined, { summary: { ...validSummary(), version: "0.0.1" } }), "unknown version"],
    ];
    const withoutSummary = feedbackBody();
    delete withoutSummary.summary;
    cases.push([withoutSummary, "missing key summary"]);
    for (const [body, message] of cases) {
      const response = await postJson("/v1/feedback", body);
      expect(response.status, message).toBe(400);
      expect((await response.json<{ error: string }>()).error).toContain(message);
    }
    expect(await count("feedback")).toBe(0);
  });

  it("accepts exactly 4000 characters and 200 lines", async () => {
    const body = feedbackBody(undefined, { text: "z".repeat(4000), log_tail: "l\n".repeat(200) });
    expect((await postJson("/v1/feedback", body)).status).toBe(201);
  });

  it("413 over 40 KB", async () => {
    const raw = JSON.stringify(feedbackBody()) + " ".repeat(40 * 1024);
    expect((await postJson("/v1/feedback", null, { raw })).status).toBe(413);
  });

  it("415 without application/json", async () => {
    expect((await postJson("/v1/feedback", feedbackBody(), { contentType: "application/x-www-form-urlencoded" })).status).toBe(415);
  });

  describe("rate limits", () => {
    beforeEach(() => {
      vi.useFakeTimers({ toFake: ["Date"] });
      vi.setSystemTime(new Date("2026-11-19T12:00:30Z"));
    });
    afterEach(() => vi.useRealTimers());

    it("2 comments per minute", async () => {
      const id = newInstallId();
      expect((await postJson("/v1/feedback", feedbackBody(id))).status).toBe(201);
      expect((await postJson("/v1/feedback", feedbackBody(id))).status).toBe(201);
      expect((await postJson("/v1/feedback", feedbackBody(id))).status).toBe(429);
      // Comments and sessions have separate counters.
      expect((await postJson("/v1/session", sessionBody(id))).status).toBe(201);
    });

    it("10 comments per day", async () => {
      const id = newInstallId();
      for (let minute = 0; minute < 5; minute++) {
        vi.setSystemTime(new Date(Date.UTC(2026, 10, 19, 12, minute, 30)));
        expect((await postJson("/v1/feedback", feedbackBody(id))).status).toBe(201);
        expect((await postJson("/v1/feedback", feedbackBody(id))).status).toBe(201);
      }
      vi.setSystemTime(new Date(Date.UTC(2026, 10, 19, 12, 10, 30)));
      expect((await postJson("/v1/feedback", feedbackBody(id))).status).toBe(429);
      // The next UTC day starts a new count.
      vi.setSystemTime(new Date(Date.UTC(2026, 10, 20, 0, 0, 30)));
      expect((await postJson("/v1/feedback", feedbackBody(id))).status).toBe(201);
    });
  });
});

describe("delete an install", () => {
  it("removes sessions, comments and rate counters of that id only", async () => {
    const id = newInstallId();
    const other = newInstallId();
    await postJson("/v1/session", sessionBody(id));
    await postJson("/v1/session", sessionBody(id));
    await postJson("/v1/feedback", feedbackBody(id, { log_tail: "x\n" }));
    await postJson("/v1/session", sessionBody(other));
    expect(await count("rate", "install_id = ?", id)).toBeGreaterThan(0);

    const response = await call(new Request(`${BASE}/v1/install/${id}`, { method: "DELETE" }));
    expect(response.status).toBe(200);
    expect(await response.json()).toEqual({ ok: true });
    expect(await count("sessions", "install_id = ?", id)).toBe(0);
    expect(await count("feedback", "install_id = ?", id)).toBe(0);
    expect(await count("rate", "install_id = ?", id)).toBe(0);
    expect(await count("sessions", "install_id = ?", other)).toBe(1);
  });

  it("answers 200 for an id that has no data", async () => {
    const response = await call(new Request(`${BASE}/v1/install/${newInstallId()}`, { method: "DELETE" }));
    expect(response.status).toBe(200);
    expect(await response.json()).toEqual({ ok: true });
  });

  it("works while the switch is off", async () => {
    const id = newInstallId();
    await postJson("/v1/session", sessionBody(id));
    const response = await call(new Request(`${BASE}/v1/install/${id}`, { method: "DELETE" }), { TELEMETRY_OPEN: "0" });
    expect(response.status).toBe(200);
    expect(await count("sessions")).toBe(0);
  });

  it("400 for a malformed id, 405 for other methods", async () => {
    for (const bad of ["123", "6F9619FF-8B86-4D01-B42D-00CF4FC964FF", "6f9619ff-8b86-1d01-b42d-00cf4fc964ff", "%27%20OR%201%3D1"]) {
      expect((await call(new Request(`${BASE}/v1/install/${bad}`, { method: "DELETE" }))).status, bad).toBe(400);
    }
    expect((await call(new Request(`${BASE}/v1/install/${newInstallId()}`))).status).toBe(405);
  });
});
