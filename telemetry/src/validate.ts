// Strict validation of the two POST bodies. Every object must have exactly the listed keys (a missing or an unknown
// key is a 400), and every leaf must have the listed type, range, length and characters. Whether a version names a
// published release is checked afterwards against D1 (index.ts), since it needs the database.

import { HttpError } from "./http";
import { PATCH_SETS } from "./patch_sets";

export const SUMMARY_SCHEMA = "nx2saria.session/1";

export const SESSION_BODY_CAP = 8 * 1024;
export const FEEDBACK_BODY_CAP = 40 * 1024;
export const TEXT_MAX_CHARS = 4000;
export const LOG_TAIL_MAX_BYTES = 24 * 1024;
export const LOG_TAIL_MAX_LINES = 200;

const MAX_INT = 2 ** 31 - 1;
const THIRTY_DAYS_S = 30 * 24 * 3600;
const MAX_FPS = 2000;
const MAX_MEMORY_MB = 4194304;

const UUID4 = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
// A release version as tagged, without the "v": starts with a letter or digit, then letters, digits, . + -.
export const VERSION_RE = /^[0-9A-Za-z][0-9A-Za-z.+-]{0,31}$/;
// The characters of settings and display values.
const TOKEN_RE = /^[A-Za-z0-9_.:x+-]*$/;
const PCI_ID_RE = /^0x[0-9a-f]{4}$/;
// A patch set's name as recomp/patches-v*.txt writes it.
const PATCH_NAME_RE = /^[a-z0-9_]{1,32}$/;
const MAX_PATCHES = 96;
const MAX_MODS = 512;
// C0 controls, DEL and C1 controls: never allowed in a one-line string.
const CONTROL_RE = /[\u0000-\u001f\u007f-\u009f]/;
// Multi-line text (comments, log lines) may hold tab, line feed and carriage return, and no other control.
const CONTROL_EXCEPT_LINES_RE = /[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f-\u009f]/;

// One leaf or object of the expected shape.
type Spec =
  | { kind: "object"; fields: Record<string, Spec> }
  | { kind: "string"; max: number; nullable: boolean; pattern?: RegExp; oneOf?: readonly string[] }
  | { kind: "int"; max: number; nullable: boolean }
  // A number >= 0 with at most one decimal.
  | { kind: "decimal1"; max: number; nullable: boolean }
  | { kind: "bool"; nullable: boolean }
  // A list of patch set names, each once.
  | { kind: "names"; max: number; nullable: boolean };

const obj = (fields: Record<string, Spec>): Spec => ({ kind: "object", fields });
const str = (max: number, extra: { pattern?: RegExp; oneOf?: readonly string[]; required?: boolean } = {}): Spec => ({
  kind: "string",
  max,
  nullable: !extra.required,
  ...(extra.pattern ? { pattern: extra.pattern } : {}),
  ...(extra.oneOf ? { oneOf: extra.oneOf } : {}),
});
const int = (max: number): Spec => ({ kind: "int", max, nullable: true });
const dec = (max: number): Spec => ({ kind: "decimal1", max, nullable: true });
const bool = (): Spec => ({ kind: "bool", nullable: true });
const token = (): Spec => str(32, { pattern: TOKEN_RE });

export const SESSION_ENDS = ["normal", "fatal", "device_lost", "crash"] as const;

// The shape of logs/session-summary.json. schema, version and game_version get their exact checks in
// validateSummary; here they are only required strings.
const SUMMARY_SPEC = obj({
  schema: str(32, { required: true }),
  version: str(32, { required: true, pattern: VERSION_RE }),
  game_version: str(32, { required: true }),
  os: obj({ family: str(16, { oneOf: ["windows", "linux"] }), name: str(64), wine: bool() }),
  cpu: obj({ name: str(64), threads: int(1024) }),
  ram_mb: int(MAX_MEMORY_MB),
  gpu: obj({
    name: str(64),
    vendor: str(6, { pattern: PCI_ID_RE }),
    device: str(6, { pattern: PCI_ID_RE }),
    driver: str(64),
    vram_mb: int(MAX_MEMORY_MB),
  }),
  display: obj({ output: token(), render_scale: token(), fullscreen: bool(), game_aspect: token(), picture: token() }),
  settings: obj({
    preset: token(),
    upscaler: token(),
    taa: token(),
    dlss: token(),
    framegen: token(),
    fps_limit: int(MAX_FPS),
    vk_present: token(),
  }),
  session: obj({
    duration_s: int(THIRTY_DAYS_S),
    frames: int(THIRTY_DAYS_S * MAX_FPS),
    end: str(16, { required: true, oneOf: SESSION_ENDS }),
  }),
  frametime: obj({
    fps_mean: dec(MAX_FPS),
    fps_low_1pct: dec(MAX_FPS),
    worst_ms: dec(600000),
    over_25ms: int(THIRTY_DAYS_S * MAX_FPS),
    over_100ms: int(THIRTY_DAYS_S * MAX_FPS),
  }),
  startup: obj({
    shader_prewarm_ms: int(MAX_INT),
    shaders_translated: int(MAX_INT),
    pipelines_built: int(MAX_INT),
    pipeline_build_ms: int(MAX_INT),
  }),
  patches: { kind: "names", max: MAX_PATCHES, nullable: true },
  mods: obj({ count: int(MAX_MODS), code: int(MAX_MODS) }),
  fatal: str(300),
});

export interface Summary {
  schema: string;
  version: string;
  game_version: string;
  os: { family: string | null; name: string | null; wine: boolean | null };
  cpu: { name: string | null; threads: number | null };
  ram_mb: number | null;
  gpu: { name: string | null; vendor: string | null; device: string | null; driver: string | null; vram_mb: number | null };
  display: {
    output: string | null;
    render_scale: string | null;
    fullscreen: boolean | null;
    game_aspect: string | null;
    picture: string | null;
  };
  settings: {
    preset: string | null;
    upscaler: string | null;
    taa: string | null;
    dlss: string | null;
    framegen: string | null;
    fps_limit: number | null;
    vk_present: string | null;
  };
  session: { duration_s: number | null; frames: number | null; end: string };
  frametime: {
    fps_mean: number | null;
    fps_low_1pct: number | null;
    worst_ms: number | null;
    over_25ms: number | null;
    over_100ms: number | null;
  };
  startup: {
    shader_prewarm_ms: number | null;
    shaders_translated: number | null;
    pipelines_built: number | null;
    pipeline_build_ms: number | null;
  };
  patches: string[] | null;
  mods: { count: number | null; code: number | null };
  fatal: string | null;
}

export interface SessionBody {
  install_id: string;
  launcher_version: string;
  summary: Summary;
}

export interface FeedbackBody {
  install_id: string;
  launcher_version: string;
  text: string;
  summary: Summary | null;
  log_tail: string | null;
}

function fail(message: string): never {
  throw new HttpError(400, message);
}

function isPlainObject(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

// Number of Unicode code points: the "characters" every length cap counts.
export function charLength(value: string): number {
  let n = 0;
  for (const _ of value) n++;
  return n;
}

// Rejects lone UTF-16 surrogates, which JSON allows as \ud800 escapes but which are not text.
function hasLoneSurrogate(value: string): boolean {
  return /[\ud800-\udbff](?![\udc00-\udfff])|(?<![\ud800-\udbff])[\udc00-\udfff]/.test(value);
}

function checkKeys(value: Record<string, unknown>, expected: readonly string[], path: string): void {
  for (const key of Object.keys(value)) {
    if (!expected.includes(key)) fail(`unknown key ${path}${key.slice(0, 64)}`);
  }
  for (const key of expected) {
    if (!Object.prototype.hasOwnProperty.call(value, key)) fail(`missing key ${path}${key}`);
  }
}

function checkSpec(value: unknown, spec: Spec, path: string): void {
  if (spec.kind === "object") {
    if (!isPlainObject(value)) fail(`${path} must be an object`);
    const keys = Object.keys(spec.fields);
    checkKeys(value, keys, `${path}.`);
    for (const key of keys) checkSpec(value[key], spec.fields[key]!, `${path}.${key}`);
    return;
  }
  if (value === null) {
    if (!spec.nullable) fail(`${path} must not be null`);
    return;
  }
  switch (spec.kind) {
    case "string":
      if (typeof value !== "string") fail(`${path} must be a string`);
      if (charLength(value) > spec.max) fail(`${path} is too long`);
      if (CONTROL_RE.test(value) || hasLoneSurrogate(value)) fail(`${path} has a control character`);
      if (spec.pattern && !spec.pattern.test(value)) fail(`${path} has a character outside its set`);
      if (spec.oneOf && !spec.oneOf.includes(value)) fail(`${path} is not an accepted value`);
      return;
    case "int":
      if (typeof value !== "number" || !Number.isSafeInteger(value) || value < 0) fail(`${path} must be an integer >= 0`);
      if (value > spec.max) fail(`${path} is out of range`);
      return;
    case "decimal1":
      if (typeof value !== "number" || !Number.isFinite(value) || value < 0) fail(`${path} must be a number >= 0`);
      if (value > spec.max) fail(`${path} is out of range`);
      if (Math.abs(value * 10 - Math.round(value * 10)) > 1e-6) fail(`${path} has more than one decimal`);
      return;
    case "bool":
      if (typeof value !== "boolean") fail(`${path} must be true, false or null`);
      return;
    case "names":
      if (!Array.isArray(value)) fail(`${path} must be a list or null`);
      if (value.length > spec.max) fail(`${path} is too long`);
      for (const name of value) {
        if (typeof name !== "string" || !PATCH_NAME_RE.test(name)) fail(`${path} has an entry that is not a patch set's name`);
      }
      if (new Set(value).size !== value.length) fail(`${path} names a patch set twice`);
      return;
  }
}

function checkInstallId(value: unknown): string {
  if (typeof value !== "string" || !UUID4.test(value)) fail("install_id must be a lowercase uuid4");
  return value;
}

export function isInstallId(value: string): boolean {
  return UUID4.test(value);
}

function checkVersionString(value: unknown, path: string): string {
  if (typeof value !== "string" || !VERSION_RE.test(value)) fail(`${path} is not a version`);
  return value;
}

// Validates a session summary. gameVersions is the GAME_VERSIONS list.
export function validateSummary(value: unknown, path: string, gameVersions: readonly string[]): Summary {
  checkSpec(value, SUMMARY_SPEC, path);
  const summary = value as Summary;
  if (summary.schema !== SUMMARY_SCHEMA) fail(`${path}.schema is not ${SUMMARY_SCHEMA}`);
  if (!gameVersions.includes(summary.game_version)) fail(`${path}.game_version is not supported`);
  if (summary.mods.count !== null && summary.mods.code !== null && summary.mods.code > summary.mods.count) {
    fail(`${path}.mods.code is above ${path}.mods.count`);
  }
  // Only the port's own sets are kept: a name this server does not know (a newer release's set) is dropped, sorted so
  // that the same sets always give the same text.
  if (summary.patches !== null) summary.patches = summary.patches.filter((name) => PATCH_SETS.includes(name)).sort();
  return summary;
}

export function parseGameVersions(list: string | undefined): string[] {
  return (list ?? "")
    .split(",")
    .map((v) => v.trim())
    .filter((v) => v.length > 0);
}

function parseJsonObject(text: string): Record<string, unknown> {
  let value: unknown;
  try {
    value = JSON.parse(text);
  } catch {
    fail("body is not JSON");
  }
  if (!isPlainObject(value)) fail("body must be a JSON object");
  return value;
}

export function validateSessionBody(text: string, gameVersions: readonly string[]): SessionBody {
  const body = parseJsonObject(text);
  checkKeys(body, ["install_id", "launcher_version", "summary"], "");
  return {
    install_id: checkInstallId(body.install_id),
    launcher_version: checkVersionString(body.launcher_version, "launcher_version"),
    summary: validateSummary(body.summary, "summary", gameVersions),
  };
}

// Multi-line text: tab, LF and CR allowed, no other control character, no lone surrogate.
function checkMultiline(value: string, path: string): void {
  if (CONTROL_EXCEPT_LINES_RE.test(value) || hasLoneSurrogate(value)) fail(`${path} has a control character`);
}

// Lines of a log tail; a final line feed does not start another line.
export function lineCount(value: string): number {
  if (value.length === 0) return 0;
  const lines = value.split("\n").length;
  return value.endsWith("\n") ? lines - 1 : lines;
}

export function validateFeedbackBody(text: string, gameVersions: readonly string[]): FeedbackBody {
  const body = parseJsonObject(text);
  checkKeys(body, ["install_id", "launcher_version", "text", "summary", "log_tail"], "");
  const installId = checkInstallId(body.install_id);
  const launcherVersion = checkVersionString(body.launcher_version, "launcher_version");

  const comment = body.text;
  if (typeof comment !== "string") fail("text must be a string");
  if (comment.trim().length === 0) fail("text is empty");
  if (charLength(comment) > TEXT_MAX_CHARS) fail("text is too long");
  checkMultiline(comment, "text");

  const summary = body.summary === null ? null : validateSummary(body.summary, "summary", gameVersions);

  const logTail = body.log_tail;
  if (logTail !== null) {
    if (typeof logTail !== "string") fail("log_tail must be a string or null");
    checkMultiline(logTail, "log_tail");
    if (new TextEncoder().encode(logTail).byteLength > LOG_TAIL_MAX_BYTES) fail("log_tail is too long");
    if (lineCount(logTail) > LOG_TAIL_MAX_LINES) fail("log_tail has too many lines");
  }

  return { install_id: installId, launcher_version: launcherVersion, text: comment, summary, log_tail: logTail };
}
