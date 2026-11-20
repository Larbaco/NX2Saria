// The cron expression (wrangler.toml [triggers]) that runs the daily retention purge; every other cron refreshes the
// releases table. Kept out of index.ts because every named export of the main module is taken as an entrypoint.
export const PURGE_CRON = "43 3 * * *";
