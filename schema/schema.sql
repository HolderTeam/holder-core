-- schema.sql (v0.1)
-- Local-first holder schema: projects, cards, links, resources, AI threads/messages, and FTS5.
-- The app/server is responsible for keeping FTS tables in sync (no triggers in v0.1).

PRAGMA foreign_keys = ON;

-- ----------------------------
-- Projects
-- ----------------------------
CREATE TABLE IF NOT EXISTS projects (
  project_id  TEXT PRIMARY KEY,          -- UUID
  name        TEXT NOT NULL,
  root_path   TEXT NOT NULL,             -- absolute path on disk
  git_remote_url TEXT NULL,              -- optional git remote (origin)
  git_provider   TEXT NULL,              -- optional provider label
  privacy_mode   TEXT NOT NULL DEFAULT 'encrypted_git', -- 'encrypted_git' | 'plain'
  project_key_id TEXT NULL,              -- keyring identifier (no secret bytes in DB)
  created_at  INTEGER NOT NULL,          -- unix epoch seconds (or ms, but be consistent)
  updated_at  INTEGER NOT NULL,
  CHECK(privacy_mode IN ('encrypted_git', 'plain'))
);

CREATE INDEX IF NOT EXISTS idx_projects_updated
  ON projects(updated_at);

-- ----------------------------
-- cards
-- ----------------------------
CREATE TABLE IF NOT EXISTS cards (
  card_id        TEXT PRIMARY KEY,       -- UUID
  project_id     TEXT NOT NULL,
  title          TEXT NOT NULL,
  rel_path       TEXT NOT NULL,          -- path relative to project root (e.g. cards/ab/cd/<uuid>.md)
  parent_card_id TEXT NULL,              -- folder-like nesting (optional)
  sort_key       REAL NOT NULL DEFAULT 0.0, -- manual ordering within a parent scope
  created_at     INTEGER NOT NULL,
  updated_at     INTEGER NOT NULL,
  deleted_at     INTEGER NULL,

  FOREIGN KEY(project_id) REFERENCES projects(project_id) ON DELETE CASCADE,
  FOREIGN KEY(parent_card_id) REFERENCES cards(card_id) ON DELETE SET NULL
);

CREATE INDEX IF NOT EXISTS idx_cards_project_updated
  ON cards(project_id, updated_at);

CREATE INDEX IF NOT EXISTS idx_cards_project_parent_sort
  ON cards(project_id, parent_card_id, sort_key);

CREATE INDEX IF NOT EXISTS idx_cards_project_title
  ON cards(project_id, title);

-- Ensure rel_path uniqueness within a project (so file mapping stays sane)
CREATE UNIQUE INDEX IF NOT EXISTS uq_cards_project_relpath
  ON cards(project_id, rel_path);

-- ----------------------------
-- card links
-- ----------------------------
-- Stores explicit directed edges between items. Used for:
-- - wiki-style links
-- - backlinks
-- - arbitrary relationships
CREATE TABLE IF NOT EXISTS card_links (
  project_id    TEXT NOT NULL,
  from_card_id  TEXT NOT NULL,
  to_card_id    TEXT NOT NULL,
  to_type       TEXT NOT NULL,           -- 'card' | 'ai_message' | 'ai_thread' | 'resource'
  kind          TEXT NOT NULL,           -- e.g. 'wiki', 'ref', 'tag', 'related'
  label         TEXT NULL,
  created_at    INTEGER NOT NULL,

  FOREIGN KEY(project_id)   REFERENCES projects(project_id) ON DELETE CASCADE,

  PRIMARY KEY(project_id, from_card_id, to_card_id, to_type, kind)
);

CREATE INDEX IF NOT EXISTS idx_card_links_from
  ON card_links(project_id, from_card_id);

CREATE INDEX IF NOT EXISTS idx_card_links_to
  ON card_links(project_id, to_card_id);

-- ----------------------------
-- Project resources (pointers only in v0.1)
-- ----------------------------
CREATE TABLE IF NOT EXISTS resources (
  resource_id  TEXT PRIMARY KEY,         -- UUID
  project_id   TEXT NOT NULL,
  kind         TEXT NOT NULL,            -- 'dir', 'file', 'repo', 'url'
  uri          TEXT NOT NULL,            -- store as URI: file:///..., https://..., git+ssh://...
  label        TEXT NOT NULL,
  desc         TEXT NULL,                -- human description (why it matters)
  created_at   INTEGER NOT NULL,
  updated_at   INTEGER NOT NULL,

  FOREIGN KEY(project_id) REFERENCES projects(project_id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_resources_project
  ON resources(project_id);

CREATE INDEX IF NOT EXISTS idx_resources_project_kind
  ON resources(project_id, kind);

-- ----------------------------
-- AI threads + messages
-- ----------------------------
CREATE TABLE IF NOT EXISTS ai_threads (
  thread_id   TEXT PRIMARY KEY,          -- UUID
  project_id  TEXT NOT NULL,
  card_id     TEXT NULL,                 -- optional attachment to a card/card
  title       TEXT NOT NULL,
  created_at  INTEGER NOT NULL,
  updated_at  INTEGER NOT NULL,

  FOREIGN KEY(project_id) REFERENCES projects(project_id) ON DELETE CASCADE,
  FOREIGN KEY(card_id)    REFERENCES cards(card_id)       ON DELETE SET NULL
);

CREATE INDEX IF NOT EXISTS idx_ai_threads_project_updated
  ON ai_threads(project_id, updated_at);

CREATE INDEX IF NOT EXISTS idx_ai_threads_card
  ON ai_threads(card_id);

CREATE TABLE IF NOT EXISTS ai_messages (
  message_id  TEXT PRIMARY KEY,          -- UUID
  thread_id   TEXT NOT NULL,
  role        TEXT NOT NULL,             -- 'user' | 'assistant'
  source      TEXT NOT NULL,             -- 'local' | 'manual_paste' | 'other'
  provider    TEXT NULL,                 -- e.g. 'Ollama', 'ChatGPT', 'Claude', 'Gemini', 'Perplexity'
  model       TEXT NULL,                 -- free-text model id/name
  content     TEXT NOT NULL,             -- markdown
  created_at  INTEGER NOT NULL,
  deleted_at  INTEGER NULL,
  prompt_hash TEXT NULL,                 -- used to correlate "export prompt" -> "paste response"
  meta_json   TEXT NULL,                 -- future-proof (citations, token counts, etc.)

  FOREIGN KEY(thread_id) REFERENCES ai_threads(thread_id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_ai_messages_thread_time
  ON ai_messages(thread_id, created_at);

CREATE INDEX IF NOT EXISTS idx_ai_messages_deleted
  ON ai_messages(deleted_at);

CREATE INDEX IF NOT EXISTS idx_ai_messages_prompt_hash
  ON ai_messages(prompt_hash);

-- ----------------------------
-- AI runs (routing/execution metadata)
-- ----------------------------
CREATE TABLE IF NOT EXISTS ai_runs (
  run_id       TEXT PRIMARY KEY,         -- UUID
  project_id   TEXT NULL,
  thread_id    TEXT NULL,
  message_id   TEXT NULL,                -- assistant message created by the run
  mode         TEXT NOT NULL,            -- 'auto' | 'model'
  prompt       TEXT NOT NULL,
  context_json TEXT NULL,                -- serialized context snapshot
  router_model TEXT NULL,
  ranked_json  TEXT NULL,                -- ranked candidates + scores
  policy_trace_json TEXT NULL,           -- structured routing/degradation trace
  chosen_model TEXT NULL,
  status       TEXT NOT NULL,            -- 'started' | 'completed' | 'failed'
  error        TEXT NULL,
  created_at   INTEGER NOT NULL,
  updated_at   INTEGER NOT NULL,

  FOREIGN KEY(project_id) REFERENCES projects(project_id) ON DELETE SET NULL,
  FOREIGN KEY(thread_id)  REFERENCES ai_threads(thread_id) ON DELETE SET NULL,
  FOREIGN KEY(message_id) REFERENCES ai_messages(message_id) ON DELETE SET NULL
);

CREATE INDEX IF NOT EXISTS idx_ai_runs_project_time
  ON ai_runs(project_id, created_at);

CREATE INDEX IF NOT EXISTS idx_ai_runs_thread_time
  ON ai_runs(thread_id, created_at);

-- ----------------------------
-- AI local model configuration (global)
-- ----------------------------
CREATE TABLE IF NOT EXISTS ai_local_model_config (
  key          TEXT PRIMARY KEY,         -- always 'global'
  fast_model   TEXT NULL,
  strong_model TEXT NULL,
  deep_model   TEXT NULL,
  updated_at   INTEGER NOT NULL
);

-- ----------------------------
-- AI runner configuration
-- ----------------------------
CREATE TABLE IF NOT EXISTS ai_runners (
  runner_id   TEXT PRIMARY KEY,
  name        TEXT NOT NULL,
  kind        TEXT NOT NULL,
  base_url    TEXT NULL,
  source      TEXT NOT NULL,
  enabled     INTEGER NOT NULL,
  created_at  INTEGER NOT NULL,
  updated_at  INTEGER NOT NULL
);

-- ----------------------------
-- Cloud provider credentials
-- ----------------------------
CREATE TABLE IF NOT EXISTS ai_provider_credentials (
  provider   TEXT PRIMARY KEY,          -- provider key, e.g. chocolatefactory
  api_key_preview TEXT NOT NULL,        -- masked preview only; raw secret lives in SecretStore
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL
);

-- ----------------------------
-- Cloud provider runtime settings
-- ----------------------------
CREATE TABLE IF NOT EXISTS ai_provider_settings (
  provider   TEXT PRIMARY KEY,          -- provider key, e.g. chocolatefactory
  enabled    INTEGER NOT NULL,          -- 0/1 runtime toggle for provider availability
  updated_at INTEGER NOT NULL
);

-- ----------------------------
-- Cloud quota usage events
-- ----------------------------
CREATE TABLE IF NOT EXISTS ai_cloud_usage_events (
  event_id         TEXT PRIMARY KEY,     -- UUID
  provider         TEXT NOT NULL,
  model_id         TEXT NOT NULL,
  prompt_tokens    INTEGER NOT NULL,
  response_tokens  INTEGER NOT NULL,
  total_tokens     INTEGER NOT NULL,
  created_at       INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_ai_cloud_usage_provider_model_time
  ON ai_cloud_usage_events(provider, model_id, created_at);

CREATE INDEX IF NOT EXISTS idx_ai_cloud_usage_time
  ON ai_cloud_usage_events(created_at);

-- ----------------------------
-- Cloud model cooldown/error state
-- ----------------------------
CREATE TABLE IF NOT EXISTS ai_cloud_model_cooldowns (
  provider       TEXT NOT NULL,
  model_id       TEXT NOT NULL,
  failure_count  INTEGER NOT NULL,
  cooldown_until INTEGER NOT NULL,
  last_error     TEXT NULL,
  updated_at     INTEGER NOT NULL,
  PRIMARY KEY(provider, model_id)
);

CREATE INDEX IF NOT EXISTS idx_ai_cloud_cooldowns_until
  ON ai_cloud_model_cooldowns(cooldown_until);

-- ----------------------------
-- Thread compaction state (rolling summary + pinned facts)
-- ----------------------------
CREATE TABLE IF NOT EXISTS ai_thread_compaction_state (
  thread_id                  TEXT PRIMARY KEY,
  rolling_summary            TEXT NULL,
  pinned_facts_json          TEXT NULL,  -- JSON array of short strings
  last_compacted_message_id  TEXT NULL,
  updated_at                 INTEGER NOT NULL,

  FOREIGN KEY(thread_id) REFERENCES ai_threads(thread_id) ON DELETE CASCADE
);

-- ----------------------------
-- AI nudges
-- ----------------------------
CREATE TABLE IF NOT EXISTS ai_nudges (
  nudge_id           TEXT PRIMARY KEY,
  kind               TEXT NOT NULL,
  project_id         TEXT NOT NULL,
  card_id            TEXT NULL,
  title              TEXT NOT NULL,
  body               TEXT NOT NULL,
  meta_json          TEXT NOT NULL DEFAULT '{}',
  basis_fingerprint  TEXT NULL,
  basis_commit       TEXT NULL,
  created_at         INTEGER NOT NULL,
  dismissed_at       INTEGER NULL,

  FOREIGN KEY(project_id) REFERENCES projects(project_id) ON DELETE CASCADE,
  FOREIGN KEY(card_id)    REFERENCES cards(card_id) ON DELETE SET NULL
);

CREATE INDEX IF NOT EXISTS idx_ai_nudges_project_created
  ON ai_nudges(project_id, created_at);

CREATE INDEX IF NOT EXISTS idx_ai_nudges_project_card_created
  ON ai_nudges(project_id, card_id, created_at);

-- ----------------------------
-- Full-text search (FTS5)
-- ----------------------------
-- Contentless FTS: server maintains rows explicitly.
-- Store IDs as UNINDEXED columns for join-back.
--
-- cards: index title + body
CREATE VIRTUAL TABLE IF NOT EXISTS cards_fts USING fts5(
  card_id    UNINDEXED,
  project_id UNINDEXED,
  title,
  body,
  tokenize = 'unicode61'
);

-- AI messages: index content
-- We include project_id for easy scoping without extra joins.
CREATE VIRTUAL TABLE IF NOT EXISTS ai_fts USING fts5(
  message_id UNINDEXED,
  thread_id  UNINDEXED,
  project_id UNINDEXED,
  content,
  tokenize = 'unicode61'
);



-- ----------------------------
-- Alerts
-- ----------------------------
CREATE TABLE IF NOT EXISTS alerts (
  alert_id     TEXT PRIMARY KEY,
  project_id   TEXT NOT NULL,
  card_id      TEXT NULL,
  title        TEXT NOT NULL,
  due_at       INTEGER NOT NULL,         -- epoch seconds (UTC)
  repeat_rule  TEXT NULL,               -- optional RRULE later
  created_at   INTEGER NOT NULL,
  updated_at   INTEGER NOT NULL,
  fired_at     INTEGER NULL,
  dismissed_at INTEGER NULL,

  FOREIGN KEY(project_id) REFERENCES projects(project_id) ON DELETE CASCADE,
  FOREIGN KEY(card_id)    REFERENCES cards(card_id)       ON DELETE SET NULL
);

CREATE INDEX IF NOT EXISTS idx_alerts_due
  ON alerts(due_at);

CREATE INDEX IF NOT EXISTS idx_alerts_project_due
  ON alerts(project_id, due_at);


-- ----------------------------
-- A tiny schema version table (handy for migrations)
-- ----------------------------
CREATE TABLE IF NOT EXISTS schema_version (
  version INTEGER NOT NULL
);

-- Initialize schema version to 1 if empty
INSERT INTO schema_version(version)
SELECT 1
WHERE NOT EXISTS (SELECT 1 FROM schema_version);
