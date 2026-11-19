// The bindings and variables the Worker reads (wrangler.toml, plus the ADMIN_TOKEN secret).
export interface Env {
  DB: D1Database;
  // Cloudflare's rate limiter keyed by the client's IP address in memory (wrangler.toml [[ratelimits]]).
  IP_LIMITER: RateLimit;
  // "1" accepts POSTs; any other value closes them with 410.
  TELEMETRY_OPEN: string;
  // Comma list of accepted summary.game_version values.
  GAME_VERSIONS: string;
  // "owner/repo" whose GitHub releases fill the releases table.
  RELEASES_REPO: string;
  // Password of the admin panel. Unset means the panel answers 503.
  ADMIN_TOKEN?: string;
}
