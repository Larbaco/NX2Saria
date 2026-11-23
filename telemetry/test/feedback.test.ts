// POST /v1/feedback, and the path /v1/install/<id>, which is not a route.
import { env } from "cloudflare:workers";
import { beforeEach, describe, expect, it } from "vitest";
import { BASE, RELEASE, call, count, feedbackBody, postJson, resetDb, sessionBody, validSummary } from "./helpers";

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

  it("413 over 40 KB", async () => {
    const raw = JSON.stringify(feedbackBody()) + " ".repeat(40 * 1024);
    expect((await postJson("/v1/feedback", null, { raw })).status).toBe(413);
  });

  it("415 without application/json", async () => {
    expect((await postJson("/v1/feedback", feedbackBody(), { contentType: "application/x-www-form-urlencoded" })).status).toBe(415);
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
