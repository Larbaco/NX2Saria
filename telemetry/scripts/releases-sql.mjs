#!/usr/bin/env node
// Prints SQL that upserts rows into the releases table, for `wrangler d1 execute --file`.
//
//   node scripts/releases-sql.mjs                       the newest 100 releases of RELEASES_REPO (wrangler.toml)
//   node scripts/releases-sql.mjs --repo owner/name     the same for another repository
//   node scripts/releases-sql.mjs --version 0.19.0-rc.2 one version, without asking GitHub (local tests)
//
// Applies the same rules as refreshReleases in src/db.ts: drafts skipped, the tag's leading "v" removed, anything
// that is not a version string dropped.
import { readFileSync } from "node:fs";
import path from "node:path";

const VERSION_RE = /^[0-9A-Za-z][0-9A-Za-z.+-]{0,31}$/;
const DATE_RE = /^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:.]+Z$/;

function sqlString(value) {
  return value === null ? "NULL" : `'${String(value).replace(/'/g, "''")}'`;
}

function upsert(rows) {
  if (rows.length === 0) return "";
  const values = rows.map(([v, p]) => `(${sqlString(v)}, ${sqlString(p)})`).join(",\n  ");
  return `INSERT INTO releases (version, published_at) VALUES\n  ${values}\nON CONFLICT (version) DO UPDATE SET published_at = excluded.published_at;\n`;
}

function repoFromToml() {
  const toml = readFileSync(path.join(import.meta.dirname, "..", "wrangler.toml"), "utf8");
  const match = /^RELEASES_REPO\s*=\s*"([^"]+)"/m.exec(toml);
  if (!match) throw new Error("RELEASES_REPO not found in wrangler.toml");
  return match[1];
}

const args = process.argv.slice(2);
const flag = (name) => {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : undefined;
};

const single = flag("--version");
if (single !== undefined) {
  if (!VERSION_RE.test(single)) throw new Error(`not a version: ${single}`);
  process.stdout.write(upsert([[single, new Date().toISOString()]]));
} else {
  const repo = flag("--repo") ?? repoFromToml();
  if (!/^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/.test(repo)) throw new Error(`not owner/repo: ${repo}`);
  const response = await fetch(`https://api.github.com/repos/${repo}/releases?per_page=100`, {
    headers: { "User-Agent": "NX2Saria-Telemetry", Accept: "application/vnd.github+json", "X-GitHub-Api-Version": "2022-11-28" },
  });
  if (!response.ok) throw new Error(`GitHub releases API answered ${response.status}`);
  const listing = await response.json();
  if (!Array.isArray(listing)) throw new Error("releases listing is not an array");
  const rows = [];
  for (const item of listing) {
    if (typeof item !== "object" || item === null || item.draft === true || typeof item.tag_name !== "string") continue;
    const version = item.tag_name.replace(/^v/, "");
    if (!VERSION_RE.test(version)) continue;
    rows.push([version, typeof item.published_at === "string" && DATE_RE.test(item.published_at) ? item.published_at : null]);
  }
  if (rows.length === 0) throw new Error(`no releases found in ${repo}`);
  process.stdout.write(upsert(rows));
  process.stderr.write(`${rows.length} releases of ${repo}\n`);
}
