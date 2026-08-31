#pragma once
#include "platform/Db.h"

#include <filesystem>
#include <string>

namespace holder::platform {

class Migrations {
 public:
  static constexpr int latest_schema_version = 4;

  // Apply schema.sql if DB is new/empty (v0.1).
  static void ensure_schema(Db& db, const std::filesystem::path& schema_sql_path);
  // Upgrade an existing database to latest_schema_version. Returns true when
  // at least one migration was applied.
  static bool migrate_to_latest(Db& db);
  static void ensure_schema_version(Db& db, int expected_version);
  // The schema_version currently stamped on an already-open database. Callers that only
  // need to detect staleness (e.g. deciding whether to trigger a rebuild rather than run
  // migrate_to_latest in place) can use this without triggering ensure_schema_version's
  // throw-on-mismatch behavior.
  static int read_schema_version(Db& db);

 private:
  static bool has_any_tables(Db& db);
  static std::string read_file(const std::filesystem::path& p);
};

} // namespace holder::platform
