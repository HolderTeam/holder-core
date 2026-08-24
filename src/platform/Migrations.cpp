#include "platform/Migrations.h"
#include "platform/Tx.h"

#include <fstream>
#include <spdlog/spdlog.h>
#include <sqlite3.h>
#include <sstream>
#include <stdexcept>

namespace holder::platform {
namespace {

int read_schema_version(Db& db) {
  static constexpr const char* SQL = "SELECT version FROM schema_version LIMIT 1;";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db.handle(), SQL, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    throw std::runtime_error(std::string("sqlite prepare failed: ") + sqlite3_errmsg(db.handle()));
  }

  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    const int version = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return version;
  }

  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw std::runtime_error(std::string("sqlite step failed: ") + sqlite3_errmsg(db.handle()));
  }

  throw std::runtime_error("schema_version row missing");
}

void migrate_v1_to_v2(Db& db) {
  static constexpr const char* SQL = R"sql(
CREATE TABLE IF NOT EXISTS card_tags (
  project_id  TEXT NOT NULL,
  card_id     TEXT NOT NULL,
  tag         TEXT NOT NULL,
  created_at  INTEGER NOT NULL,

  FOREIGN KEY(project_id) REFERENCES projects(project_id) ON DELETE CASCADE,
  FOREIGN KEY(card_id)    REFERENCES cards(card_id)       ON DELETE CASCADE,

  PRIMARY KEY(project_id, card_id, tag)
);

CREATE INDEX IF NOT EXISTS idx_card_tags_tag
  ON card_tags(project_id, tag);

CREATE INDEX IF NOT EXISTS idx_card_tags_card
  ON card_tags(project_id, card_id);

UPDATE schema_version SET version = 2 WHERE version = 1;
)sql";

  Tx tx(db);
  db.exec(SQL);
  tx.commit();
}

void migrate_v2_to_v3(Db& db) {
  static constexpr const char* SQL = R"sql(
DROP TABLE IF EXISTS alerts;

CREATE TABLE IF NOT EXISTS milestones (
  milestone_id  TEXT PRIMARY KEY,
  project_id    TEXT NOT NULL,
  card_id       TEXT NOT NULL,
  start_at      INTEGER NOT NULL,
  end_at        INTEGER NULL,
  all_day       INTEGER NOT NULL DEFAULT 0,
  kind          TEXT NULL,
  description   TEXT NULL,
  created_at    INTEGER NOT NULL,
  updated_at    INTEGER NOT NULL,

  FOREIGN KEY(project_id) REFERENCES projects(project_id) ON DELETE CASCADE,
  FOREIGN KEY(card_id)    REFERENCES cards(card_id)       ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_milestones_project_card
  ON milestones(project_id, card_id);

CREATE INDEX IF NOT EXISTS idx_milestones_project_start
  ON milestones(project_id, start_at);

UPDATE schema_version SET version = 3 WHERE version = 2;
)sql";

  Tx tx(db);
  db.exec(SQL);
  tx.commit();
}

void migrate_v3_to_v4(Db& db) {
  static constexpr const char* SQL = R"sql(
DROP TABLE IF EXISTS asset_placements;
DROP TABLE IF EXISTS assets;
DROP TABLE IF EXISTS resource_metadata;
DROP TABLE IF EXISTS storage_locations;
DROP TABLE IF EXISTS resources;

CREATE TABLE resources (
  resource_id TEXT PRIMARY KEY,
  project_id TEXT NOT NULL,
  type TEXT NOT NULL,
  label TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL,
  FOREIGN KEY(project_id) REFERENCES projects(project_id) ON DELETE CASCADE
);
CREATE INDEX idx_resources_project ON resources(project_id);
CREATE INDEX idx_resources_project_type ON resources(project_id, type);

CREATE TABLE resource_metadata (
  resource_id TEXT NOT NULL,
  property TEXT NOT NULL,
  value_index INTEGER NOT NULL,
  value TEXT NOT NULL,
  FOREIGN KEY(resource_id) REFERENCES resources(resource_id) ON DELETE CASCADE,
  PRIMARY KEY(resource_id, property, value_index)
);
CREATE INDEX idx_resource_metadata_property ON resource_metadata(property, value);

CREATE TABLE storage_locations (
  location_id TEXT PRIMARY KEY,
  project_id TEXT NOT NULL,
  name TEXT NOT NULL,
  provider TEXT NOT NULL,
  config_json TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL,
  FOREIGN KEY(project_id) REFERENCES projects(project_id) ON DELETE CASCADE
);
CREATE INDEX idx_storage_locations_project ON storage_locations(project_id, updated_at DESC);

CREATE TABLE assets (
  asset_id TEXT PRIMARY KEY,
  resource_id TEXT NOT NULL,
  project_id TEXT NOT NULL,
  original_filename TEXT NOT NULL,
  media_type TEXT NOT NULL,
  byte_size INTEGER NOT NULL,
  plaintext_sha256 TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL,
  FOREIGN KEY(resource_id) REFERENCES resources(resource_id) ON DELETE CASCADE,
  FOREIGN KEY(project_id) REFERENCES projects(project_id) ON DELETE CASCADE,
  CHECK(byte_size >= 0)
);
CREATE INDEX idx_assets_resource ON assets(resource_id, updated_at DESC);
CREATE INDEX idx_assets_project_hash ON assets(project_id, plaintext_sha256);

CREATE TABLE asset_placements (
  placement_id TEXT PRIMARY KEY,
  asset_id TEXT NOT NULL,
  location_id TEXT NOT NULL,
  object_key TEXT NOT NULL,
  encoding TEXT NOT NULL,
  stored_byte_size INTEGER NOT NULL,
  stored_sha256 TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  FOREIGN KEY(asset_id) REFERENCES assets(asset_id) ON DELETE CASCADE,
  FOREIGN KEY(location_id) REFERENCES storage_locations(location_id) ON DELETE RESTRICT,
  UNIQUE(location_id, object_key),
  CHECK(stored_byte_size >= 0)
);
CREATE INDEX idx_asset_placements_asset ON asset_placements(asset_id);
CREATE INDEX idx_asset_placements_location ON asset_placements(location_id);

UPDATE schema_version SET version = 4 WHERE version = 3;
)sql";

  Tx tx(db);
  db.exec(SQL);
  tx.commit();
}

} // namespace

std::string Migrations::read_file(const std::filesystem::path& p) {
  std::ifstream in(p);
  if (!in.is_open()) {
    throw std::runtime_error("Failed to open schema file: " + p.string());
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool Migrations::has_any_tables(Db& db) {
  // Check for any non-internal tables.
  static constexpr const char* SQL = "SELECT 1 "
                                     "FROM sqlite_master "
                                     "WHERE type='table' "
                                     "  AND name NOT LIKE 'sqlite_%' "
                                     "LIMIT 1;";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db.handle(), SQL, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    throw std::runtime_error(std::string("sqlite prepare failed: ") + sqlite3_errmsg(db.handle()));
  }

  rc = sqlite3_step(stmt);
  bool any = (rc == SQLITE_ROW);

  sqlite3_finalize(stmt);
  return any;
}

void Migrations::ensure_schema(Db& db, const std::filesystem::path& schema_sql_path) {
  spdlog::info("DB opened: {}", db.path().string());

  if (has_any_tables(db)) {
    spdlog::info("Schema already present (tables exist), skipping schema apply.");
    return;
  }

  spdlog::info("No tables found; applying schema from: {}", schema_sql_path.string());
  const auto sql = read_file(schema_sql_path);

  Tx tx(db);
  db.exec(sql);
  tx.commit();

  spdlog::info("Schema applied successfully.");
}

bool Migrations::migrate_to_latest(Db& db) {
  int version = read_schema_version(db);
  if (version > latest_schema_version) {
    throw std::runtime_error(
        "Schema version mismatch. Expected at most " + std::to_string(latest_schema_version) +
        ", got " + std::to_string(version)
    );
  }

  bool migrated = false;
  while (version < latest_schema_version) {
    switch (version) {
    case 1:
      migrate_v1_to_v2(db);
      version = 2;
      migrated = true;
      break;
    case 2:
      migrate_v2_to_v3(db);
      version = 3;
      migrated = true;
      break;
    case 3:
      migrate_v3_to_v4(db);
      version = 4;
      migrated = true;
      break;
    default:
      throw std::runtime_error("Unsupported schema version: " + std::to_string(version));
    }
  }
  return migrated;
}

void Migrations::ensure_schema_version(Db& db, int expected_version) {
  const int version = read_schema_version(db);
  if (version != expected_version) {
    throw std::runtime_error(
        "Schema version mismatch. Expected " + std::to_string(expected_version) + ", got " +
        std::to_string(version)
    );
  }
}

} // namespace holder::platform
