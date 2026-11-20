// The admin panel, its CSV exports and the log view.
import { env } from "cloudflare:workers";
import { beforeEach, describe, expect, it } from "vitest";
import { csvCell, median, normalizeFatal, quantile } from "../src/admin";
import { BASE, OLD_RELEASE, RELEASE, adminGet, call, crashSummary, feedbackBody, newInstallId, postJson, resetDb, sessionBody, validSummary } from "./helpers";

beforeEach(resetDb);

// Two installs, four sessions (one crash), one comment with a hostile text and a log.
async function seed(): Promise<{ installA: string }> {
  const a = newInstallId();
  const b = newInstallId();
  await postJson("/v1/session", sessionBody(a), { country: "BR" });
  const slow = validSummary();
  slow.frametime.fps_mean = 30.5;
  slow.gpu.name = "AMD Radeon RX 6600";
  slow.gpu.vendor = "0x1002";
  await postJson("/v1/session", sessionBody(a, slow), { country: "BR" });
  await postJson("/v1/session", sessionBody(b), { country: "DE" });
  await postJson("/v1/session", sessionBody(b, crashSummary()), { country: "DE" });
  await postJson(
    "/v1/feedback",
    feedbackBody(b, { text: '<script>alert("x")</script> & =SUM(A1)', summary: validSummary(), log_tail: "<b>log</b>\nsecond\n" }),
    { country: "DE" },
  );
  return { installA: a };
}

describe("authentication", () => {
  it("401 without a token and with a wrong one", async () => {
    const none = await adminGet("/v1/admin", null);
    expect(none.status).toBe(401);
    expect(none.headers.get("WWW-Authenticate")).toMatch(/^Basic realm=/);
    expect((await adminGet("/v1/admin", "wrong-token")).status).toBe(401);
    expect((await adminGet("/v1/admin", "")).status).toBe(401);
    const bearer = await call(new Request(`${BASE}/v1/admin`, { headers: { Authorization: "Bearer test-admin-token" } }));
    expect(bearer.status).toBe(401);
    expect((await adminGet("/v1/admin/sessions.csv", "wrong-token")).status).toBe(401);
    expect((await adminGet("/v1/admin/feedback/1/log", null)).status).toBe(401);
  });

  it("503 when ADMIN_TOKEN is not set, even with a password", async () => {
    expect((await adminGet("/v1/admin", "anything", { ADMIN_TOKEN: undefined })).status).toBe(503);
    expect((await adminGet("/v1/admin", "", { ADMIN_TOKEN: "" })).status).toBe(503);
    expect((await adminGet("/v1/admin/feedback.csv", "x", { ADMIN_TOKEN: undefined })).status).toBe(503);
  });

  it("any user name with the right password", async () => {
    const request = new Request(`${BASE}/v1/admin`, { headers: { Authorization: `Basic ${btoa("ana.souza:test-admin-token")}` } });
    expect((await call(request)).status).toBe(200);
  });

  it("405 for POST on admin routes", async () => {
    expect((await call(new Request(`${BASE}/v1/admin`, { method: "POST" }))).status).toBe(405);
  });
});

describe("panel", () => {
  it("shows the counts with safe headers", async () => {
    await seed();
    const response = await adminGet("/v1/admin");
    expect(response.status).toBe(200);
    expect(response.headers.get("Content-Type")).toBe("text/html; charset=utf-8");
    expect(response.headers.get("Content-Security-Policy")).toBe("default-src 'none'; style-src 'unsafe-inline'");
    expect(response.headers.get("X-Content-Type-Options")).toBe("nosniff");
    expect(response.headers.get("Cache-Control")).toBe("no-store");
    const html = await response.text();
    expect(html).toContain('<b id="sessions">4</b>');
    expect(html).toContain('<b id="installs">2</b>');
    expect(html).toContain('<b id="comments">1</b>');
    expect(html).toContain('<b id="abnormal">25.0%</b>');
    expect(html).toContain("AMD Radeon RX 6600");
    expect(html).toContain("0x1002");
    expect(html).toMatch(/By aspect ratio<\/caption>[\s\S]*?<td>16:10<\/td>/);
    expect(html).toMatch(/By preset<\/caption>[\s\S]*?<td>high<\/td>/);
    expect(html).toMatch(/<td>BR<\/td><td class="n">2<\/td>/);
    // Median fps_mean of version RELEASE over 59.9, 30.5 and 59.9 (the crash has none).
    expect(html).toMatch(new RegExp(`<td>${RELEASE.replace(/\./g, "\\.")}</td><td class="n">4</td><td class="n">59\\.9</td>`));
    // The crash's fatal line, normalized.
    expect(html).toContain("access violation at # in module main+#");
    expect(html).toContain('href="/v1/admin/feedback/');
  });

  it("escapes a hostile comment", async () => {
    await seed();
    const html = await (await adminGet("/v1/admin")).text();
    expect(html).not.toContain("<script>");
    expect(html).toContain("&lt;script&gt;alert(&quot;x&quot;)&lt;/script&gt; &amp; =SUM(A1)");
  });

  it("filters by days and version", async () => {
    await seed();
    const old = Math.floor(Date.now() / 1000) - 40 * 86400;
    await env.DB.prepare("UPDATE sessions SET received_at = ? WHERE country = 'BR'").bind(old).run();
    const html30 = await (await adminGet("/v1/admin?days=30")).text();
    expect(html30).toContain('<b id="sessions">2</b>');
    const html180 = await (await adminGet("/v1/admin?days=180")).text();
    expect(html180).toContain('<b id="sessions">4</b>');
    const other = await (await adminGet(`/v1/admin?days=180&version=${OLD_RELEASE}`)).text();
    expect(other).toContain('<b id="sessions">0</b>');
    expect(other).toContain('<b id="comments">0</b>');
    // A bad value falls back to the defaults instead of failing.
    expect((await adminGet("/v1/admin?days=9999&version=%3Cx%3E")).status).toBe(200);
  });

  it("renders an empty database", async () => {
    const html = await (await adminGet("/v1/admin")).text();
    expect(html).toContain('<b id="sessions">0</b>');
    expect(html).toContain("No data.");
  });
});

describe("CSV", () => {
  it("exports sessions with every column", async () => {
    const { installA } = await seed();
    const response = await adminGet("/v1/admin/sessions.csv?days=30");
    expect(response.status).toBe(200);
    expect(response.headers.get("Content-Type")).toBe("text/csv; charset=utf-8");
    expect(response.headers.get("Content-Disposition")).toBe('attachment; filename="sessions.csv"');
    const lines = (await response.text()).trimEnd().split("\r\n");
    expect(lines).toHaveLength(5);
    expect(lines[0]).toContain('"install_id","launcher_version","country","schema","version"');
    expect(lines[0]).toContain('"frametime_fps_low_1pct"');
    expect(lines.filter((l) => l.includes(installA))).toHaveLength(2);
    expect(lines.some((l) => l.includes('"AMD Radeon RX 6600","0x1002"'))).toBe(true);
  });

  it("exports comments quoted and guarded against formulas", async () => {
    await seed();
    await postJson("/v1/feedback", feedbackBody(undefined, { text: '=HYPERLINK("http://example.invalid")' }));
    await postJson("/v1/feedback", feedbackBody(undefined, { text: "@cmd, with comma\nand line" }));
    const text = await (await adminGet("/v1/admin/feedback.csv")).text();
    expect(text).toContain(`"'=HYPERLINK(""http://example.invalid"")"`);
    expect(text).toContain(`"'@cmd, with comma\nand line"`);
    expect(text).toContain(`"<script>alert(""x"")</script> & =SUM(A1)"`);
    expect(text.split("\r\n")[0]).toBe(
      '"id","received_at","install_id","launcher_version","version","gpu_name","gpu_vendor","os_family","country","text","has_log","summary_json"',
    );
  });

  it("csvCell", () => {
    expect(csvCell(null)).toBe("");
    expect(csvCell(12)).toBe('"12"');
    expect(csvCell("-1+2")).toBe(`"'-1+2"`);
    expect(csvCell("+x")).toBe(`"'+x"`);
    expect(csvCell("\tx")).toBe(`"'\tx"`);
    expect(csvCell('a"b')).toBe('"a""b"');
  });
});

describe("log view", () => {
  it("serves the log as plain text", async () => {
    await seed();
    const row = await env.DB.prepare("SELECT id FROM feedback LIMIT 1").first<{ id: number }>();
    const response = await adminGet(`/v1/admin/feedback/${row!.id}/log`);
    expect(response.status).toBe(200);
    expect(response.headers.get("Content-Type")).toBe("text/plain; charset=utf-8");
    expect(response.headers.get("X-Content-Type-Options")).toBe("nosniff");
    expect(await response.text()).toBe("<b>log</b>\nsecond\n");
  });

  it("404 for a comment without a log or an unknown id", async () => {
    await postJson("/v1/feedback", feedbackBody());
    const row = await env.DB.prepare("SELECT id FROM feedback LIMIT 1").first<{ id: number }>();
    expect((await adminGet(`/v1/admin/feedback/${row!.id}/log`)).status).toBe(404);
    expect((await adminGet("/v1/admin/feedback/999999/log")).status).toBe(404);
    expect((await adminGet("/v1/admin/feedback/abc/log")).status).toBe(404);
  });
});

describe("statistics helpers", () => {
  it("median and p95", () => {
    expect(median([])).toBeNull();
    expect(median([1, 2, 3])).toBe(2);
    expect(median([1, 2, 3, 4])).toBe(2.5);
    const hundred = Array.from({ length: 100 }, (_, i) => i + 1);
    expect(quantile(hundred, 0.95)).toBe(95);
    expect(quantile([7], 0.95)).toBe(7);
  });

  it("normalizeFatal", () => {
    expect(normalizeFatal("pipeline 12 failed at 0xDEADBEEF (deadbeef01) code -4")).toBe("pipeline # failed at # (#) code -#");
  });
});
