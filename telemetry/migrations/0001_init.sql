-- Session reports, player comments, rate counters and the list of published releases.
-- Times are Unix seconds (UTC). No column holds an IP address or a request header.

-- One row per accepted POST /v1/session: every leaf of the session summary as its own column.
CREATE TABLE sessions (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  install_id TEXT NOT NULL,
  launcher_version TEXT NOT NULL,
  country TEXT NOT NULL,
  received_at INTEGER NOT NULL,
  schema TEXT NOT NULL,
  version TEXT NOT NULL,
  game_version TEXT NOT NULL,
  os_family TEXT,
  os_name TEXT,
  os_wine INTEGER,
  cpu_name TEXT,
  cpu_threads INTEGER,
  ram_mb INTEGER,
  gpu_name TEXT,
  gpu_vendor TEXT,
  gpu_device TEXT,
  gpu_driver TEXT,
  gpu_vram_mb INTEGER,
  display_output TEXT,
  display_render_scale TEXT,
  display_fullscreen INTEGER,
  display_game_aspect TEXT,
  settings_preset TEXT,
  settings_upscaler TEXT,
  settings_taa TEXT,
  settings_dlss TEXT,
  settings_framegen TEXT,
  settings_fps_limit INTEGER,
  settings_vk_present TEXT,
  session_duration_s INTEGER,
  session_frames INTEGER,
  session_end TEXT NOT NULL,
  frametime_fps_mean REAL,
  frametime_fps_low_1pct REAL,
  frametime_worst_ms REAL,
  frametime_over_25ms INTEGER,
  frametime_over_100ms INTEGER,
  startup_shader_prewarm_ms INTEGER,
  startup_shaders_translated INTEGER,
  startup_pipelines_built INTEGER,
  startup_pipeline_build_ms INTEGER,
  fatal TEXT
);
CREATE INDEX sessions_received_at ON sessions (received_at);
CREATE INDEX sessions_install_id ON sessions (install_id);
CREATE INDEX sessions_version ON sessions (version, received_at);

-- One row per accepted POST /v1/feedback. summary_json is the validated summary as sent (or NULL); version, the
-- gpu and os columns copy its leaves so the admin panel can filter and show them without parsing JSON.
CREATE TABLE feedback (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  install_id TEXT NOT NULL,
  launcher_version TEXT NOT NULL,
  text TEXT NOT NULL,
  summary_json TEXT,
  log_tail TEXT,
  country TEXT NOT NULL,
  received_at INTEGER NOT NULL,
  version TEXT,
  gpu_name TEXT,
  gpu_vendor TEXT,
  os_family TEXT
);
CREATE INDEX feedback_received_at ON feedback (received_at);
CREATE INDEX feedback_install_id ON feedback (install_id);

-- Rate counters: kind is session_min, session_day, feedback_min or feedback_day; window is the Unix second at which
-- that minute or UTC day starts.
CREATE TABLE rate (
  install_id TEXT NOT NULL,
  kind TEXT NOT NULL,
  "window" INTEGER NOT NULL,
  count INTEGER NOT NULL,
  PRIMARY KEY (install_id, kind, "window")
) WITHOUT ROWID;
CREATE INDEX rate_window ON rate ("window");

-- Published releases (tag without its leading "v"); refreshed hourly from the GitHub releases API.
CREATE TABLE releases (
  version TEXT PRIMARY KEY,
  published_at TEXT
) WITHOUT ROWID;
