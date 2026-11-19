#!/usr/bin/env bash
# Deploys the telemetry Worker and its D1 database to Cloudflare. Safe to run again: each step checks what already
# exists. README.md ("Deploy") explains each step and the manual equivalent.
#
#   CLOUDFLARE_API_TOKEN=... ./deploy.sh            deploy
#   ./deploy.sh --dry-run                           print the commands without running any of them
#
# Environment:
#   CLOUDFLARE_API_TOKEN   required (an API token with Workers Scripts:Edit and D1:Edit on the account)
#   CLOUDFLARE_ACCOUNT_ID  needed only when the token can see more than one account
#   ADMIN_TOKEN            the admin panel password to set; when unset, an existing ADMIN_TOKEN secret is kept, and
#                          when there is none a new one is generated and printed once
#   D1_LOCATION            optional location hint for a new database (weur, eeur, apac, oc, wnam, enam)
set -euo pipefail

DB_NAME="nx2saria-telemetry"
PLACEHOLDER_ID="00000000-0000-4000-8000-000000000000"
DEPLOY_CONFIG="wrangler.deploy.toml"

dry_run=0
case "${1:-}" in
  --dry-run) dry_run=1 ;;
  "") ;;
  -h | --help)
    sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
    ;;
  *)
    echo "unknown argument: $1 (use --dry-run or --help)" >&2
    exit 2
    ;;
esac

cd "$(dirname "$0")"

if [[ -z "${CLOUDFLARE_API_TOKEN:-}" ]]; then
  if [[ $dry_run -eq 1 ]]; then
    echo "note: CLOUDFLARE_API_TOKEN is not set; a real run needs it" >&2
  else
    echo "CLOUDFLARE_API_TOKEN is not set (README.md, Deploy)" >&2
    exit 1
  fi
fi
for tool in node npx; do
  command -v "$tool" > /dev/null || { echo "$tool is required" >&2; exit 1; }
done
[[ -d node_modules ]] || { echo "run 'npm ci' in telemetry/ first" >&2; exit 1; }

wrangler() { npx --no-install wrangler "$@"; }

# Prints a command; runs it too unless this is a dry run. Stdin is /dev/null so wrangler never waits on a prompt
# (without a terminal it takes the default answer, which applies the migrations).
run() {
  printf '+'
  printf ' %q' "$@"
  printf '\n'
  if [[ $dry_run -eq 0 ]]; then "$@" < /dev/null; fi
}

# The id of the D1 database named DB_NAME, or nothing when the account has none.
database_id() {
  wrangler d1 list --json < /dev/null | node -e '
    let s = ""; process.stdin.on("data", (d) => (s += d)).on("end", () => {
      const db = JSON.parse(s).find((d) => d.name === process.argv[1]);
      if (db) process.stdout.write(db.uuid);
    });' "$DB_NAME"
}

echo "== D1 database $DB_NAME"
if [[ $dry_run -eq 1 ]]; then
  run wrangler d1 list --json
  echo "(dry run: when absent) wrangler d1 create $DB_NAME --no-update-config"
  db_id="<database-id>"
else
  db_id="$(database_id)"
  if [[ -z "$db_id" ]]; then
    create=(wrangler d1 create "$DB_NAME" --no-update-config)
    [[ -n "${D1_LOCATION:-}" ]] && create+=(--location "$D1_LOCATION")
    run "${create[@]}"
    db_id="$(database_id)"
  fi
  [[ "$db_id" =~ ^[0-9a-f-]{36}$ ]] || { echo "could not find the id of $DB_NAME" >&2; exit 1; }
  echo "database_id $db_id"
fi

echo "== $DEPLOY_CONFIG (wrangler.toml with the real database_id; git-ignored)"
if [[ $dry_run -eq 1 ]]; then
  echo "+ sed s/$PLACEHOLDER_ID/<database-id>/ wrangler.toml > $DEPLOY_CONFIG"
else
  grep -q "$PLACEHOLDER_ID" wrangler.toml || { echo "wrangler.toml lost its placeholder database_id" >&2; exit 1; }
  sed "s/$PLACEHOLDER_ID/$db_id/" wrangler.toml > "$DEPLOY_CONFIG"
fi

echo "== Migrations"
run wrangler d1 migrations apply "$DB_NAME" --remote --config "$DEPLOY_CONFIG"

echo "== Releases table"
seed_sql="$(mktemp "${TMPDIR:-/tmp}/nx2saria-releases.XXXXXX.sql")"
trap 'rm -f "$seed_sql"' EXIT
if [[ $dry_run -eq 1 ]]; then
  echo "+ node scripts/releases-sql.mjs > $seed_sql"
else
  node scripts/releases-sql.mjs > "$seed_sql"
fi
run wrangler d1 execute "$DB_NAME" --remote --config "$DEPLOY_CONFIG" --file "$seed_sql"

echo "== Worker"
deploy_log="$(mktemp "${TMPDIR:-/tmp}/nx2saria-deploy.XXXXXX.log")"
trap 'rm -f "$seed_sql" "$deploy_log"' EXIT
if [[ $dry_run -eq 1 ]]; then
  run wrangler deploy --config "$DEPLOY_CONFIG"
else
  run wrangler deploy --config "$DEPLOY_CONFIG" | tee "$deploy_log"
fi

echo "== ADMIN_TOKEN secret"
if [[ -n "${ADMIN_TOKEN:-}" ]]; then
  echo "+ printf '%s' \"\$ADMIN_TOKEN\" | wrangler secret put ADMIN_TOKEN --config $DEPLOY_CONFIG"
  if [[ $dry_run -eq 0 ]]; then printf '%s' "$ADMIN_TOKEN" | wrangler secret put ADMIN_TOKEN --config "$DEPLOY_CONFIG"; fi
elif [[ $dry_run -eq 1 ]]; then
  run wrangler secret list --format json --config "$DEPLOY_CONFIG"
  echo "(dry run: when absent) openssl rand -hex 24 | wrangler secret put ADMIN_TOKEN --config $DEPLOY_CONFIG"
elif wrangler secret list --format json --config "$DEPLOY_CONFIG" < /dev/null | grep -q '"ADMIN_TOKEN"'; then
  echo "ADMIN_TOKEN already set; kept (set ADMIN_TOKEN in the environment to replace it)"
else
  command -v openssl > /dev/null || { echo "openssl is required to generate ADMIN_TOKEN" >&2; exit 1; }
  new_token="$(openssl rand -hex 24)"
  printf '%s' "$new_token" | wrangler secret put ADMIN_TOKEN --config "$DEPLOY_CONFIG"
  echo
  echo "New admin password (shown only now; keep it in a password manager):"
  echo "  $new_token"
  echo
  unset new_token
fi

echo "== Done"
if [[ $dry_run -eq 1 ]]; then
  echo "The Worker URL is printed by 'wrangler deploy' (https://nx2saria-telemetry.<subdomain>.workers.dev)."
else
  url="$(grep -oE 'https://[A-Za-z0-9.-]+\.workers\.dev' "$deploy_log" | head -n 1 || true)"
  if [[ -n "$url" ]]; then
    echo "Worker URL: $url"
    echo "Admin panel: $url/v1/admin"
    echo "Put this URL in the launcher's TELEMETRY_URL."
  else
    echo "Worker deployed; find its URL in the Cloudflare dashboard (Workers & Pages)."
  fi
fi
