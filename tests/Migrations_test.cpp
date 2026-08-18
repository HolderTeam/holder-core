#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "platform/Db.h"
#include "platform/Migrations.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path find_schema_sql() {
#ifdef SCHEMA_SQL_PATH
  std::filesystem::path p = SCHEMA_SQL_PATH;
  if (std::filesystem::exists(p)) return p;
#endif
  namespace fs = std::filesystem;
  fs::path p1 = fs::current_path() / "schema" / "schema.sql";
  if (fs::exists(p1)) return p1;
  fs::path p2 = fs::current_path().parent_path() / "schema" / "schema.sql";
  if (fs::exists(p2)) return p2;
  throw std::runtime_error("schema.sql not found for tests");
}

std::filesystem::path make_temp_dir() {
  const auto base = std::filesystem::temp_directory_path();
  const auto suffix = std::to_string(
      static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())
  );
  auto dir = base / ("holder_migrations_test_" + suffix);
  std::filesystem::create_directories(dir);
  return dir;
}

} // namespace

TEST_CASE("ensure_schema_version accepts expected version", "[migrations]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  const auto schema_path = find_schema_sql();
  holder::platform::Migrations::ensure_schema(db, schema_path);

  REQUIRE_NOTHROW(holder::platform::Migrations::ensure_schema_version(db, 1));
}

TEST_CASE("ensure_schema_version rejects mismatch", "[migrations]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  const auto schema_path = find_schema_sql();
  holder::platform::Migrations::ensure_schema(db, schema_path);

  REQUIRE_THROWS_WITH(
      holder::platform::Migrations::ensure_schema_version(db, 2),
      Catch::Matchers::ContainsSubstring("Schema version mismatch")
  );
}

TEST_CASE("ensure_schema fails when schema file is missing", "[migrations]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  const auto missing_schema = dir / "missing-schema.sql";
  REQUIRE_FALSE(std::filesystem::exists(missing_schema));

  REQUIRE_THROWS_WITH(
      holder::platform::Migrations::ensure_schema(db, missing_schema),
      Catch::Matchers::ContainsSubstring("Failed to open schema file")
  );
}

TEST_CASE("ensure_schema throws when sqlite prepare fails", "[migrations]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  db.close();

  const auto schema_path = find_schema_sql();
  REQUIRE_THROWS_WITH(
      holder::platform::Migrations::ensure_schema(db, schema_path),
      Catch::Matchers::ContainsSubstring("sqlite prepare failed")
  );
}

TEST_CASE("ensure_schema_version throws when sqlite prepare fails", "[migrations]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  db.close();

  REQUIRE_THROWS_WITH(
      holder::platform::Migrations::ensure_schema_version(db, 1),
      Catch::Matchers::ContainsSubstring("sqlite prepare failed")
  );
}

TEST_CASE("ensure_schema_version throws when schema_version row is missing", "[migrations]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  db.exec("CREATE TABLE schema_version(version INTEGER NOT NULL);");

  REQUIRE_THROWS_WITH(
      holder::platform::Migrations::ensure_schema_version(db, 1),
      Catch::Matchers::ContainsSubstring("schema_version row missing")
  );
}

TEST_CASE("ensure_schema_version throws when sqlite step fails", "[migrations]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  auto fail_fn = [](sqlite3_context* ctx, int /*argc*/, sqlite3_value** /*argv*/) {
    sqlite3_result_error(ctx, "forced-step-error", -1);
  };
  REQUIRE(
      sqlite3_create_function_v2(
          db.handle(),
          "always_fail",
          0,
          SQLITE_UTF8,
          nullptr,
          fail_fn,
          nullptr,
          nullptr,
          nullptr
      ) == SQLITE_OK
  );

  db.exec("CREATE VIEW schema_version AS SELECT always_fail() AS version;");

  REQUIRE_THROWS_WITH(
      holder::platform::Migrations::ensure_schema_version(db, 1),
      Catch::Matchers::ContainsSubstring("sqlite step failed")
  );
}
