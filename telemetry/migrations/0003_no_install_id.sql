-- A report and a comment are no longer tied to an installation: the install_id columns go, with what they held, and
-- so do the per-installation rate counters. The limit that remains is counted per IP address in memory.
DROP INDEX IF EXISTS sessions_install_id;
DROP INDEX IF EXISTS feedback_install_id;
ALTER TABLE sessions DROP COLUMN install_id;
ALTER TABLE feedback DROP COLUMN install_id;
DROP INDEX IF EXISTS rate_window;
DROP TABLE IF EXISTS rate;
