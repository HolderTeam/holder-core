#pragma once

#include "platform/Db.h"

#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace holder::platform {

enum class DatabaseHealth { Missing, Healthy, Corrupt, IoError };

struct DatabaseHealthResult {
  DatabaseHealth health = DatabaseHealth::Missing;
  std::string detail;
};

struct DatabaseRebuildHooks {
  std::function<void(Db&)> audit_existing;
  std::function<void(Db&)> restore_before_projects;
  std::function<void(Db&)> restore_after_projects;
  std::function<void(Db&)> validate_rebuilt;
};

struct DatabaseRebuildRequest {
  std::filesystem::path database_path;
  std::filesystem::path backup_root;
  std::string schema_sql;
  int expected_schema_version = 0;
  std::vector<std::filesystem::path> project_roots;
  std::vector<std::pair<std::string, std::filesystem::path>> required_authorities;
  bool durable_ownership_ready = false;
  bool dry_run = false;
  DatabaseRebuildHooks hooks;
};

struct DatabaseRebuildReport {
  bool dry_run = false;
  std::string previous_health;
  std::size_t projects = 0;
  std::size_t cards = 0;
  std::size_t ai_threads = 0;
  std::size_t ai_messages = 0;
  std::size_t resources = 0;
  std::size_t assets = 0;
  std::size_t placements = 0;
  std::size_t locations = 0;
  std::filesystem::path backup_path;

  std::string to_json() const;
};

DatabaseHealthResult inspect_database_health(const std::filesystem::path& path);
bool database_rebuild_is_ready(const std::filesystem::path& readiness_path);
void mark_database_rebuild_ready(const std::filesystem::path& readiness_path);
void audit_core_durable_ownership(Db& db);
DatabaseRebuildReport rebuild_database_projection(const DatabaseRebuildRequest& request);

} // namespace holder::platform
