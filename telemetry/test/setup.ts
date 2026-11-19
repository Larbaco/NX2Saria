// Applies migrations/ to the test D1 database once per test file (applyD1Migrations skips applied ones).
import { applyD1Migrations } from "cloudflare:test";
import { env } from "cloudflare:workers";

await applyD1Migrations(env.DB, env.TEST_MIGRATIONS);
