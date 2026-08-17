#pragma once
#include "platform/Db.h"

#include <filesystem>
#include <string>

namespace holder::platform {

class Migrations {
 public:
  // Apply schema.sql if DB is new/empty (v0.1).
  static void ensure_schema(Db& db, const std::filesystem::path& schema_sql_path);
  static void ensure_schema_version(Db& db, int expected_version);

 private:
  static bool has_any_tables(Db& db);
  static std::string read_file(const std::filesystem::path& p);
};

} // namespace holder::platform
