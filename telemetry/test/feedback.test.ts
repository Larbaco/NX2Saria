// POST /v1/feedback, and the path /v1/install/<id>, which is not a route.
import { env } from "cloudflare:workers";
import { beforeEach, describe, expect, it } from "vitest";
import { BASE, RELEASE, call, count, crashLogBody, crashSummary, feedbackBody, postJson, resetDb, sessionBody, validSummary } from "./helpers";

beforeEach(resetDb);

describe("feedback", () => {
  it("stores text, summary and log tail", async () => {
    const logTail = "line one\nline two\twith tab\r\nline three\n";
    const response = await postJson(
      "/v1/feedback",
      feedbackBody({ text: "Olá! Water flickers.\nSecond line.", summary: validSummary(), log_tail: logTail }),
      { country: "PT" },
    );
    expect(response.status).toBe(201);
    const row = await env.DB.prepare("SELECT * FROM feedback ORDER BY id DESC LIMIT 1").first<Record<string, any>>();
    expect(row).toMatchObject({
      launcher_version: RELEASE,
      text: "Olá! Water flickers.\nSecond line.",
      log_tail: logTail,
      version: RELEASE,
      gpu_name: "NVIDIA GeForce RTX 4070",
      gpu_vendor: "0x10de",
      os_family: "windows",
    });
    expect(JSON.parse(row!.summary_json)).toEqual(validSummary());
    expect(Object.keys(row!)).not.toContain("install_id");
  });

  it("accepts null summary and null log tail", async () => {
    expect((await postJson("/v1/feedback", feedbackBody())).status).toBe(201);
    expect(await count("feedback", "summary_json IS NULL AND log_tail IS NULL AND version IS NULL")).toBe(1);
  });

  it("rejects bad bodies", async () => {
    const cases: [Record<string, any>, string][] = [
      [{ ...feedbackBody(), email: "ana.souza@example.com" }, "unknown key email"],
      [{ ...feedbackBody(), install_id: "3b241101-e2bb-4255-8caf-4136c566a962" }, "unknown key install_id"],
      [feedbackBody({ text: "" }), "text is empty"],
      [feedbackBody({ text: "   \n " }), "text is empty"],
      [feedbackBody({ text: "x".repeat(4001) }), "text is too long"],
      [feedbackBody({ text: 42 }), "text must be a string"],
      [feedbackBody({ text: "bell\u0007" }), "control character"],
      [feedbackBody({ log_tail: "a\u0000b" }), "control character"],
      [feedbackBody({ log_tail: "line\n".repeat(201) }), "too many lines"],
      [feedbackBody({ log_tail: "y".repeat(24 * 1024 + 1) }), "log_tail is too long"],
      [feedbackBody({ log_tail: ["a"] }), "log_tail must be a string"],
      [feedbackBody({ summary: { ...validSummary(), extra: 1 } }), "unknown key summary.extra"],
      [feedbackBody({ summary: { ...validSummary(), game_version: "1.0.0" } }), "game_version"],
      [feedbackBody({ summary: { ...validSummary(), version: "0.0.1" } }), "unknown version"],
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
    const body = feedbackBody({ text: "z".repeat(4000), log_tail: "l\n".repeat(200) });
    expect((await postJson("/v1/feedback", body)).status).toBe(201);
  });

  it("413 over 96 KB", async () => {
    const raw = JSON.stringify(feedbackBody()) + " ".repeat(96 * 1024);
    expect((await postJson("/v1/feedback", null, { raw })).status).toBe(413);
  });

  it("415 without application/json", async () => {
    expect((await postJson("/v1/feedback", feedbackBody(), { contentType: "application/x-www-form-urlencoded" })).status).toBe(415);
  });
});

describe("kind", () => {
  it("a body without kind is stored as a comment", async () => {
    expect((await postJson("/v1/feedback", feedbackBody({ log_tail: "one\n" }))).status).toBe(201);
    expect(await count("feedback", "kind = 'comment' AND log_head IS NULL AND log_tail = 'one\n'")).toBe(1);
  });

  it("kind comment keeps every rule of a comment", async () => {
    expect((await postJson("/v1/feedback", feedbackBody({ kind: "comment" }))).status).toBe(201);
    expect(await count("feedback", "kind = 'comment'")).toBe(1);
    const cases: [Record<string, any>, string][] = [
      [feedbackBody({ kind: "comment", text: "" }), "text is empty"],
      [feedbackBody({ kind: "comment", log_tail: "y".repeat(24 * 1024 + 1) }), "log_tail is too long"],
      [feedbackBody({ kind: "comment", log_head: "start\n" }), "log_head is only accepted with kind crash"],
      [feedbackBody({ log_head: "start\n" }), "log_head is only accepted with kind crash"],
    ];
    for (const [body, message] of cases) {
      const response = await postJson("/v1/feedback", body);
      expect(response.status, message).toBe(400);
      expect((await response.json<{ error: string }>()).error).toContain(message);
    }
    expect(await count("feedback")).toBe(1);
  });

  it("stores a crash log with an empty text, the start and the end of the log", async () => {
    const body = crashLogBody({ summary: crashSummary() });
    expect((await postJson("/v1/feedback", body)).status).toBe(201);
    const row = await env.DB.prepare("SELECT * FROM feedback").first<Record<string, any>>();
    expect(row).toMatchObject({
      kind: "crash",
      text: "",
      log_tail: body.log_tail,
      log_head: body.log_head,
      launcher_version: RELEASE,
      version: RELEASE,
      os_family: "linux",
    });
    expect(Object.keys(row!)).not.toContain("install_id");
  });

  it("a crash log may come without the start of the log, or with it null", async () => {
    const withoutHead = crashLogBody();
    delete withoutHead.log_head;
    expect((await postJson("/v1/feedback", withoutHead)).status).toBe(201);
    expect((await postJson("/v1/feedback", crashLogBody({ log_head: null }))).status).toBe(201);
    expect(await count("feedback", "kind = 'crash' AND log_head IS NULL")).toBe(2);
  });

  it("accepts a crash log at its limits: 200 lines in 56 KB, 100 lines in 16 KB", async () => {
    const tailLine = "t".repeat(Math.floor((56 * 1024) / 200) - 1) + "\n";
    const headLine = "h".repeat(Math.floor((16 * 1024) / 100) - 1) + "\n";
    const body = crashLogBody({ log_tail: tailLine.repeat(200), log_head: headLine.repeat(100) });
    expect(new TextEncoder().encode(body.log_tail).byteLength).toBeGreaterThan(55 * 1024);
    expect((await postJson("/v1/feedback", body)).status).toBe(201);
  });

  it("rejects bad crash logs", async () => {
    const cases: [Record<string, any>, string][] = [
      [crashLogBody({ kind: "bug" }), "kind is not an accepted value"],
      [crashLogBody({ kind: null }), "kind is not an accepted value"],
      [crashLogBody({ kind: 1 }), "kind is not an accepted value"],
      [crashLogBody({ log_tail: null }), "log_tail is required for a crash log"],
      [crashLogBody({ log_tail: " \n" }), "log_tail is required for a crash log"],
      [crashLogBody({ log_tail: "y".repeat(56 * 1024 + 1) }), "log_tail is too long"],
      [crashLogBody({ log_tail: "line\n".repeat(201) }), "log_tail has too many lines"],
      [crashLogBody({ log_head: "y".repeat(16 * 1024 + 1) }), "log_head is too long"],
      [crashLogBody({ log_head: "line\n".repeat(101) }), "log_head has too many lines"],
      [crashLogBody({ log_head: "a\u0000b" }), "control character"],
      [crashLogBody({ log_head: 7 }), "log_head must be a string or null"],
      [crashLogBody({ text: "x".repeat(4001) }), "text is too long"],
      [crashLogBody({ text: null }), "text must be a string"],
      [crashLogBody({ install_id: "3b241101-e2bb-4255-8caf-4136c566a962" }), "unknown key install_id"],
      [crashLogBody({ summary: { ...validSummary(), version: "0.0.1" } }), "unknown version"],
    ];
    for (const [body, message] of cases) {
      const response = await postJson("/v1/feedback", body);
      expect(response.status, message).toBe(400);
      expect((await response.json<{ error: string }>()).error).toContain(message);
    }
    expect(await count("feedback")).toBe(0);
  });

  it("a crash log may carry a text", async () => {
    expect((await postJson("/v1/feedback", crashLogBody({ text: "It fell in the Forest Temple." }))).status).toBe(201);
    expect(await count("feedback", "kind = 'crash' AND text = 'It fell in the Forest Temple.'")).toBe(1);
  });
});

describe("no installation id", () => {
  const id = "3b241101-e2bb-4255-8caf-4136c566a962";

  it("404 for DELETE and GET on /v1/install/<id>, and nothing is deleted", async () => {
    await postJson("/v1/session", sessionBody());
    await postJson("/v1/feedback", feedbackBody());
    for (const method of ["DELETE", "GET"]) {
      const response = await call(new Request(`${BASE}/v1/install/${id}`, { method }));
      expect(response.status, method).toBe(404);
      expect(await response.json()).toEqual({ error: "not found" });
    }
    expect(await count("sessions")).toBe(1);
    expect(await count("feedback")).toBe(1);
  });

  it("the database has no table of rate counters", async () => {
    const { results } = await env.DB.prepare("SELECT name FROM sqlite_master WHERE type='table'").all<{ name: string }>();
    const names = results.map((r) => r.name);
    expect(names).toEqual(expect.arrayContaining(["sessions", "feedback", "releases", "meta"]));
    expect(names).not.toContain("rate");
  });
});
