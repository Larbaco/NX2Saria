# NX2Saria telemetry server

A Cloudflare Worker with a D1 database that receives the launcher's session reports, player comments and the crash
logs a player chose to send, and shows the results on a password-protected admin page.
What players are told about it is in [docs/telemetry.md](../docs/telemetry.md).

- No analytics service of anyone else, no R2, no runtime dependencies: `src/` is all the code that runs.
- The Worker reads no request header other than `Content-Type`, `Content-Length`, `CF-Connecting-IP` and, on the
  admin routes, `Authorization`. The client's IP address (`CF-Connecting-IP`) is used only as the key of
  Cloudflare's in-memory rate limiter on the two POST routes (`IP_LIMITER` in `wrangler.toml`, 20 requests a minute);
  it is never written to D1 or logged. Nothing is kept of where a request came from: no country either (migration
  `0002` removed the column and what it held). Cloudflare itself, which runs the Worker, handles each connection as
  for any site.
- A report and a comment carry no installation id and no other identifier (migration `0003` removed the column and
  what it held). No row names an installation or a person, so the server cannot tell whose rows are whose and
  nothing can be deleted on request: a row goes when it is 180 days old. A comment's free text is the exception to
  "no identifier": it holds whatever its writer put in it.
- A report's `patches` list is kept only as far as it names the port's own patch sets (`src/patch_sets.ts`, every
  `set` line of `recomp/patches-v*.txt`); a name the server does not know is dropped, not stored. After adding a
  patch set, update that list (`tests/telemetry_patch_sets_check.py` fails until then) and deploy again.
- Sessions, comments and crash logs are deleted after 180 days by a daily cron.

## Files

| Path | What it holds |
|---|---|
| `src/index.ts` | Router, the POST handlers, the cron dispatch |
| `src/validate.ts` | The strict validator of both POST bodies (keys, types, ranges, lengths, characters) |
| `src/db.ts` | D1 statements: inserts, retention, releases refresh |
| `src/admin.ts` | Admin panel, CSV exports, log view, Basic auth |
| `src/http.ts` | Responses, the capped body reader |
| `src/patch_sets.ts` | The names of the port's patch sets a report may name |
| `src/crons.ts` | Which cron is the purge |
| `migrations/` | The D1 schema |
| `scripts/releases-sql.mjs` | Prints SQL that fills the `releases` table (from GitHub, or one given version) |
| `deploy.sh` | One-command deploy (below) |
| `test/` | Tests (Vitest inside the Workers runtime, with a local D1) |

## Routes

| Method and path | Answer |
|---|---|
| `POST /v1/session` | Body `{"launcher_version", "summary"}`, at most 8 KB. `201 {"ok":true}` |
| `POST /v1/feedback` | Body `{"launcher_version", "text", "summary", "log_tail"}` and the optional keys `"kind"` and `"log_head"`, at most 96 KB. `201 {"ok":true}`. See [Comments and crash logs](#comments-and-crash-logs) |
| `GET /v1/admin` | Admin page; filters `?days=7\|30\|180` (default 30) and `?version=` |
| `GET /v1/admin/sessions.csv`, `GET /v1/admin/feedback.csv` | CSV exports with the same filters (newest 100 000 rows). `feedback.csv` also takes `?kind=comment\|crash`; without it, it holds both, told apart by its `kind` column |
| `GET /v1/admin/feedback/<id>/log` | The log lines of a comment or of a crash log, as `text/plain`. A crash log's start and end come under one title line each |

Errors are JSON `{"error": "..."}`:

- `400`: unknown or missing key anywhere (an `install_id` key is an unknown key), a value of the wrong type, out of
  range, too long or with characters outside its set, a `launcher_version` or `summary.version` that is not a
  published release, a `game_version` not in `GAME_VERSIONS`, a body that is not JSON or not UTF-8.
- `413`: body over the limit (checked on `Content-Length` and again on the bytes read). `415`: `Content-Type` is not
  `application/json`.
- `429`: more than 20 POST requests in a minute from one IP address (counted in memory, never stored). It is the
  only limit.
- `410 {"error":"closed"}`: every POST while `TELEMETRY_OPEN` is not `"1"`.
- `401` (with `WWW-Authenticate: Basic`) on the admin routes without the right password; `503` when `ADMIN_TOKEN` is
  not set, so the panel is never open by mistake.
- `404` for an unknown path, `405` (with `Allow`) for a wrong method.

### Comments and crash logs

Both arrive at `POST /v1/feedback` and are rows of the `feedback` table, told apart by `kind`:

| `kind` | `text` | `log_tail` | `log_head` |
|---|---|---|---|
| `comment`, also when the key is absent | required, 1 to 4000 characters | optional: at most 200 lines and 24 KB | refused |
| `crash` | may be empty, at most 4000 characters | required: at most 200 lines and 56 KB | optional: at most 100 lines and 16 KB |

- A body without `kind` is a comment, so a launcher older than the crash logs keeps working unchanged. A launcher
  that sends `kind` or `log_head` to a server older than them gets `400` (an unknown key).
- `log_head` is the start of the game's log (the lines up to `patches:`), which the last lines of a long session no
  longer hold. 200 of the game's log lines weigh about 46 KB (measured over 184 logs: median 46 KB, largest 58 KB),
  which is why a crash log's end may be 56 KB; a comment's 24 KB holds about 107 of them.
- The field limits are checked after the body is parsed, by kind; the 96 KB of the body is 56 + 16 KB of log lines,
  the summary, the text and the room JSON escapes take.

The exact schema of `summary` is in [docs/telemetry.md](../docs/telemetry.md) and in `SUMMARY_SPEC` in
`src/validate.ts`.

### Admin panel

Open `https://<worker>/v1/admin` in a browser; any user name, the password is `ADMIN_TOKEN`. It shows the number of
sessions, of comments and of crash logs sent, the share of sessions that did not end normally; counts by version, GPU,
OS, preset, aspect ratio, upscaler, frame generation, mods (none, without code, with code) and patch set on; median
`fps_mean`, median `fps_low_1pct` and p95 `worst_ms` by version and by GPU; the most common fatal lines with numbers and addresses replaced by `#`; the 50 newest
comments with links to their logs; and, in a list of their own so that they do not bury the comments, the 50 newest
crash logs with the session's end, its fatal line and a link to the log. The statistics read at most the newest 50 000 sessions of the period.

Every value is HTML-escaped. Pages are sent with `Content-Security-Policy: default-src 'none'; style-src
'unsafe-inline'`, `X-Content-Type-Options: nosniff` and `Cache-Control: no-store`. CSV cells are always quoted and a
text cell starting with `=`, `+`, `-`, `@`, tab or carriage return gets a leading `'` so a spreadsheet does not run it.

### Configuration

| Name | Kind | Value |
|---|---|---|
| `DB` | D1 binding | database `nx2saria-telemetry` |
| `TELEMETRY_OPEN` | var | `"1"` accepts reports; anything else answers 410 to POSTs |
| `GAME_VERSIONS` | var | comma list of accepted `game_version` values, `"1.1.1"` |
| `RELEASES_REPO` | var | `"Larbaco/NX2Saria"`, whose GitHub releases are the accepted versions |
| `ADMIN_TOKEN` | secret | admin password; never in `wrangler.toml` |

Crons (UTC): `17 * * * *` refreshes the `releases` table from
`https://api.github.com/repos/<RELEASES_REPO>/releases?per_page=100` (drafts skipped, the tag's `v` removed; releases
no longer in that listing stay accepted). `43 3 * * *` deletes sessions, comments and crash logs older than 180 days.

## Deploy

You need a Cloudflare account and Node.js 20 or newer. `wrangler.toml` holds no account id and no secret, and its
`database_id` is a placeholder; the deploy uses `wrangler.deploy.toml`, a git-ignored copy with the real id.

### With deploy.sh

```sh
cd telemetry
npm ci
export CLOUDFLARE_API_TOKEN=...      # My Profile > API Tokens: "Edit Cloudflare Workers" template plus D1 Edit
export CLOUDFLARE_ACCOUNT_ID=...     # only when the token can see more than one account
./deploy.sh --dry-run                # prints every command, runs none
./deploy.sh
```

It creates the D1 database only when the account has none of that name, writes `wrangler.deploy.toml`, applies the
migrations, fills `releases` from GitHub, deploys the Worker, and sets `ADMIN_TOKEN`: from the `ADMIN_TOKEN`
environment variable when set; otherwise it keeps an existing secret, or generates one with `openssl` and prints it
once. It ends with the Worker's URL. Running it again changes nothing but the code and the releases.

### By hand

```sh
cd telemetry
npm ci
npx wrangler login                    # or export CLOUDFLARE_API_TOKEN
npx wrangler d1 create nx2saria-telemetry
# Copy the printed database_id:
sed 's/00000000-0000-4000-8000-000000000000/<database_id>/' wrangler.toml > wrangler.deploy.toml
npx wrangler d1 migrations apply nx2saria-telemetry --remote --config wrangler.deploy.toml
node scripts/releases-sql.mjs > /tmp/releases.sql
npx wrangler d1 execute nx2saria-telemetry --remote --config wrangler.deploy.toml --file /tmp/releases.sql
npx wrangler deploy --config wrangler.deploy.toml
npx wrangler secret put ADMIN_TOKEN --config wrangler.deploy.toml    # type a long random password
```

The secret is set after the first deploy because `wrangler secret put` needs the Worker to exist; until then the
panel answers 503.

`wrangler deploy` prints the URL, `https://nx2saria-telemetry.<your-subdomain>.workers.dev`. That URL goes into the
launcher's `TELEMETRY_URL`. To check it: `curl <url>/v1/session` answers
`{"error":"method not allowed"}`, and `<url>/v1/admin` asks for the password.

### Deploying a version with a new migration

`deploy.sh` applies the migrations before it deploys the Worker, which is the order every migration here needs.
Migration `0004` adds `feedback.kind` (`comment` for every row there already is) and `feedback.log_head`: the Worker
before it never names either column, so it keeps working between the two steps, while the Worker after it fails on
every `POST /v1/feedback` without them. By hand: `npx wrangler d1 migrations apply ... --remote` first, then
`npx wrangler deploy`. A launcher that offers crash logs must not be released before this Worker is deployed: until
then each crash log it sends is refused with `400`.

### Turning reports off

Set `TELEMETRY_OPEN = "0"` in `wrangler.toml` and deploy again (`./deploy.sh`, or `npx wrangler deploy --config
wrangler.deploy.toml`). Every POST then answers `410 {"error":"closed"}` and the launcher stops sending until its own
version changes. The admin panel keeps working. Set it back to `"1"` and deploy to reopen.

### Accepting a new release

The hourly cron picks up a release within the hour of its publication on GitHub. To accept it at once:

```sh
node scripts/releases-sql.mjs > /tmp/releases.sql
npx wrangler d1 execute nx2saria-telemetry --remote --config wrangler.deploy.toml --file /tmp/releases.sql
```

## Run locally

```sh
cd telemetry
npm ci
printf 'ADMIN_TOKEN=local-dev-password\n' > .dev.vars      # git-ignored
npx wrangler d1 migrations apply nx2saria-telemetry --local
node scripts/releases-sql.mjs --version 0.19.0-rc.2 > .wrangler/releases.sql
npx wrangler d1 execute nx2saria-telemetry --local --file .wrangler/releases.sql
npx wrangler dev --local --port 8790                     # same as: npm run dev
```

The local database lives in `.wrangler/state/` (git-ignored). The admin page is then at
`http://localhost:8790/v1/admin`. Point a launcher at it with `NX2SARIA_TELEMETRY_URL=http://localhost:8790`. Add
`--test-scheduled` to `wrangler dev` to run a cron by hand:
`curl "http://localhost:8790/__scheduled?cron=43+3+*+*+*"` (purge) or `cron=17+*+*+*+*` (releases refresh).

## Tests

```sh
cd telemetry
npm ci
npm test               # vitest inside the Workers runtime, local D1 with migrations/ applied
npx tsc --noEmit       # type check
```

The tests cover the schema (unknown and missing keys at every level, types, ranges, lengths, characters, versions),
body limits, content type, the per-IP limit, the absence of an installation id, the kill switch, admin authentication,
the panel's counts and escaping, the CSV exports, the log view, the retention purge and the releases refresh with
GitHub mocked.
