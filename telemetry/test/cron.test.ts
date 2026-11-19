// The two crons: the daily retention purge and the hourly releases refresh (with the GitHub API mocked).
import { createScheduledController } from "cloudflare:test";
import { env } from "cloudflare:workers";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import worker from "../src/index";
import { PURGE_CRON } from "../src/crons";
import { releasesFromListing } from "../src/db";
import { count, feedbackBody, newInstallId, postJson, resetDb, sessionBody, testEnv } from "./helpers";

beforeEach(resetDb);
afterEach(() => vi.restoreAllMocks());

function runCron(cron: string): Promise<void> {
  return worker.scheduled(createScheduledController({ cron, scheduledTime: Date.now() }), testEnv());
}

describe("retention purge", () => {
  it("runs on a cron that wrangler.toml schedules, besides an hourly one", () => {
    expect(env.TEST_CRONS).toContain(PURGE_CRON);
    expect(env.TEST_CRONS.filter((c) => c !== PURGE_CRON)).toEqual(["17 * * * *"]);
  });

  it("deletes sessions, comments and rate counters past their age", async () => {
    const id = newInstallId();
    await postJson("/v1/session", sessionBody(id));
    await postJson("/v1/session", sessionBody(id));
    await postJson("/v1/feedback", feedbackBody(id));
    await postJson("/v1/feedback", feedbackBody(id));
    const currentWindows = await count("rate", "install_id = ?", id);
    const now = Math.floor(Date.now() / 1000);
    const old = now - 181 * 86400;
    const recent = now - 179 * 86400;
    await env.DB.batch([
      env.DB.prepare("UPDATE sessions SET received_at = ? WHERE id = (SELECT MIN(id) FROM sessions)").bind(old),
      env.DB.prepare("UPDATE sessions SET received_at = ? WHERE id = (SELECT MAX(id) FROM sessions)").bind(recent),
      env.DB.prepare("UPDATE feedback SET received_at = ? WHERE id = (SELECT MIN(id) FROM feedback)").bind(old),
      env.DB.prepare(`INSERT INTO rate (install_id, kind, "window", count) VALUES (?, 'session_day', ?, 3)`).bind(id, now - 3 * 86400),
    ]);
    expect(await count("rate", "install_id = ?", id)).toBe(currentWindows + 1);

    await runCron(PURGE_CRON);

    expect(await count("sessions")).toBe(1);
    expect(await count("sessions", "received_at = ?", recent)).toBe(1);
    expect(await count("feedback")).toBe(1);
    // The current windows stay; the three-day-old one is gone.
    expect(await count("rate", "install_id = ?", id)).toBe(currentWindows);
  });

  it("the purge cron does not call GitHub", async () => {
    const spy = vi.spyOn(globalThis, "fetch");
    await runCron(PURGE_CRON);
    expect(spy).not.toHaveBeenCalled();
  });
});

describe("releases refresh", () => {
  it("fetches the repository's releases and upserts them without the v", async () => {
    const listing = [
      { tag_name: "v0.20.0", draft: false, prerelease: false, published_at: "2026-12-01T10:00:00Z" },
      { tag_name: "v0.20.0-rc.1", draft: false, prerelease: true, published_at: "2026-11-25T10:00:00Z" },
      { tag_name: "v0.21.0", draft: true, published_at: null },
      { tag_name: "v0.19.0-rc.2", draft: false, published_at: "2026-11-18T12:00:00Z" },
      { tag_name: "<bad tag>", draft: false, published_at: "2026-11-01T00:00:00Z" },
      { name: "no tag" },
    ];
    const spy = vi
      .spyOn(globalThis, "fetch")
      .mockResolvedValue(new Response(JSON.stringify(listing), { headers: { "Content-Type": "application/json" } }));

    await runCron("17 * * * *");

    expect(spy).toHaveBeenCalledTimes(1);
    const [url, init] = spy.mock.calls[0]!;
    expect(String(url)).toBe("https://api.github.com/repos/Larbaco/NX2Saria/releases?per_page=100");
    expect(new Headers(init?.headers).get("User-Agent")).toBe("NX2Saria-Telemetry");
    const { results } = await env.DB.prepare("SELECT version, published_at FROM releases ORDER BY version").all();
    expect(results).toEqual([
      { version: "0.18.7", published_at: "2026-10-01T12:00:00Z" },
      { version: "0.19.0-rc.2", published_at: "2026-11-18T12:00:00Z" },
      { version: "0.20.0", published_at: "2026-12-01T10:00:00Z" },
      { version: "0.20.0-rc.1", published_at: "2026-11-25T10:00:00Z" },
    ]);

    // A report of the new release is now accepted.
    const body = sessionBody();
    body.launcher_version = "0.20.0";
    body.summary.version = "0.20.0";
    expect((await postJson("/v1/session", body)).status).toBe(201);
  });

  it("keeps the table when GitHub fails", async () => {
    vi.spyOn(globalThis, "fetch").mockResolvedValue(new Response("rate limited", { status: 403 }));
    await expect(runCron("17 * * * *")).rejects.toThrow("403");
    expect(await count("releases")).toBe(2);
  });

  it("releasesFromListing refuses a non-array", () => {
    expect(() => releasesFromListing({ message: "Not Found" })).toThrow();
    expect(releasesFromListing([])).toEqual([]);
  });

  it("refreshes at once, once per period, when a report names a version the table lacks", async () => {
    await env.DB.prepare("DELETE FROM meta").run();
    const listing = [{ tag_name: "v0.20.0", draft: false, published_at: "2026-12-01T10:00:00Z" }];
    const spy = vi
      .spyOn(globalThis, "fetch")
      .mockImplementation(async () => new Response(JSON.stringify(listing), { headers: { "Content-Type": "application/json" } }));
    const body = sessionBody();
    body.launcher_version = "0.20.0";
    body.summary.version = "0.20.0";
    expect((await postJson("/v1/session", body)).status).toBe(201);
    expect(spy).toHaveBeenCalledTimes(1);

    // a version GitHub does not list either: refused, and no second call inside the period
    const bogus = sessionBody();
    bogus.launcher_version = "9.9.9";
    expect((await postJson("/v1/session", bogus)).status).toBe(400);
    expect(spy).toHaveBeenCalledTimes(1);
  });
});
