// Tests run inside the Workers runtime (workerd through Miniflare) with a local D1 database; test/setup.ts applies
// migrations/ to it before the tests.
import { readFileSync } from "node:fs";
import path from "node:path";
import { cloudflareTest, readD1Migrations } from "@cloudflare/vitest-plugin";
import { defineConfig } from "vitest/config";

export default defineConfig(async () => {
  const migrations = await readD1Migrations(path.join(import.meta.dirname, "migrations"));
  // The crons line of wrangler.toml, so a test can check that src/crons.ts names one of them.
  const toml = readFileSync(path.join(import.meta.dirname, "wrangler.toml"), "utf8");
  const cronsLine = /^crons\s*=\s*(\[.*\])\s*$/m.exec(toml)?.[1] ?? "[]";
  return {
    plugins: [
      cloudflareTest({
        wrangler: { configPath: "./wrangler.toml" },
        miniflare: {
          bindings: { TEST_MIGRATIONS: migrations, TEST_CRONS: JSON.parse(cronsLine), ADMIN_TOKEN: "test-admin-token" },
        },
      }),
    ],
    test: { setupFiles: ["./test/setup.ts"] },
  };
});
