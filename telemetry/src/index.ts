// NX2Saria's telemetry Worker: receives session reports and player comments from the launcher and serves the admin
// panel. A report and a comment carry no installation id: no stored row names an installation or a person.
// Routes are listed in README.md.
//
// The Worker reads no request header except Content-Type, Content-Length, CF-Connecting-IP (the POST routes, as the
// key of the in-memory rate limiter only) and (admin routes only) Authorization. It stores no IP and nothing else
// about where a request came from.

import type { Env } from "./env";
import { HttpError, error, isJsonContentType, json, methodNotAllowed, readCappedBody } from "./http";
import {
  FEEDBACK_BODY_CAP,
  SESSION_BODY_CAP,
  parseGameVersions,
  validateFeedbackBody,
  validateSessionBody,
} from "./validate";
import {
  areReleases,
  claimReleasesRefresh,
  insertFeedback,
  insertSession,
  nowSeconds,
  purgeOld,
  refreshReleases,
} from "./db";
import { PURGE_CRON } from "./crons";
import { checkAdmin, feedbackCsv, feedbackLog, renderPanel, sessionsCsv } from "./admin";

function isOpen(env: Env): boolean {
  return env.TELEMETRY_OPEN === "1";
}

// Common front of both POST routes: kill switch, content type, capped body. Returns the body text.
async function readPost(request: Request, env: Env, cap: number): Promise<string> {
  if (!isOpen(env)) throw new HttpError(410, "closed");
  if (!isJsonContentType(request)) throw new HttpError(415, "content-type must be application/json");
  return readCappedBody(request, cap);
}

// Both versions must be published releases. A release made since the hourly refresh is not in the table yet: the
// first report naming an unknown version refreshes it at once (at most every RELEASES_REFRESH_GAP_S), so the reports
// of a release's first hour are not refused.
async function checkVersions(env: Env, versions: string[]): Promise<void> {
  if (await areReleases(env.DB, versions)) return;
  if (await claimReleasesRefresh(env.DB, nowSeconds())) {
    try {
      await refreshReleases(env.DB, env.RELEASES_REPO);
    } catch (e) {
      console.error("releases refresh failed", e instanceof Error ? e.message : String(e));
    }
    if (await areReleases(env.DB, versions)) return;
  }
  throw new HttpError(400, "unknown version");
}

async function postSession(request: Request, env: Env): Promise<Response> {
  const text = await readPost(request, env, SESSION_BODY_CAP);
  const body = validateSessionBody(text, parseGameVersions(env.GAME_VERSIONS));
  await checkVersions(env, [body.launcher_version, body.summary.version]);
  await insertSession(env.DB, body.launcher_version, nowSeconds(), body.summary).run();
  return json(201, { ok: true });
}

async function postFeedback(request: Request, env: Env): Promise<Response> {
  const text = await readPost(request, env, FEEDBACK_BODY_CAP);
  const body = validateFeedbackBody(text, parseGameVersions(env.GAME_VERSIONS));
  await checkVersions(env, body.summary === null ? [body.launcher_version] : [body.launcher_version, body.summary.version]);
  await insertFeedback(env.DB, {
    kind: body.kind,
    launcherVersion: body.launcher_version,
    text: body.text,
    summary: body.summary,
    logTail: body.log_tail,
    logHead: body.log_head,
    now: nowSeconds(),
  }).run();
  return json(201, { ok: true });
}

// The per-IP limit of the POST routes, the only limit there is: the address is the key of Cloudflare's in-memory
// counter for this request and is not kept, logged or written anywhere. A request without the header (never on
// Cloudflare) shares one key.
async function ipLimited(request: Request, env: Env): Promise<boolean> {
  const ip = request.headers.get("CF-Connecting-IP") ?? "none";
  const { success } = await env.IP_LIMITER.limit({ key: ip });
  return !success;
}

async function route(request: Request, env: Env): Promise<Response> {
  const { pathname } = new URL(request.url);
  const method = request.method;
  const isPublicWrite = method === "POST" && (pathname === "/v1/session" || pathname === "/v1/feedback");
  if (isPublicWrite && (await ipLimited(request, env))) return error(429, "rate limited");

  if (pathname === "/v1/session") {
    return method === "POST" ? postSession(request, env) : methodNotAllowed("POST");
  }
  if (pathname === "/v1/feedback") {
    return method === "POST" ? postFeedback(request, env) : methodNotAllowed("POST");
  }

  const adminLog = /^\/v1\/admin\/feedback\/([^/]+)\/log$/.exec(pathname);
  const isAdmin =
    pathname === "/v1/admin" || pathname === "/v1/admin/sessions.csv" || pathname === "/v1/admin/feedback.csv" || adminLog !== null;
  if (isAdmin) {
    if (method !== "GET" && method !== "HEAD") return methodNotAllowed("GET, HEAD");
    const denied = await checkAdmin(request, env);
    if (denied) return denied;
    const now = nowSeconds();
    if (pathname === "/v1/admin") return renderPanel(request, env, now);
    if (pathname === "/v1/admin/sessions.csv") return sessionsCsv(request, env, now);
    if (pathname === "/v1/admin/feedback.csv") return feedbackCsv(request, env, now);
    const id = adminLog![1]!;
    if (!/^[1-9][0-9]{0,15}$/.test(id)) return error(404, "not found");
    return feedbackLog(env, Number(id));
  }

  return error(404, "not found");
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    try {
      return await route(request, env);
    } catch (e) {
      if (e instanceof HttpError) return error(e.status, e.message);
      // Never echo internal errors; the message stays in the Worker's own log.
      console.error("unhandled error", e instanceof Error ? e.message : String(e));
      return error(500, "internal error");
    }
  },

  async scheduled(controller: ScheduledController, env: Env): Promise<void> {
    if (controller.cron === PURGE_CRON) {
      await purgeOld(env.DB, nowSeconds());
    } else {
      await refreshReleases(env.DB, env.RELEASES_REPO);
    }
  },
} satisfies ExportedHandler<Env>;
