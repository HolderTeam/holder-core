#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "platform/Db.h"
#include "platform/Tx.h"

#include <chrono>
#include <filesystem>
#include <string>

namespace {

std::filesystem::path make_temp_dir() {
  const auto base = std::filesystem::temp_directory_path();
  const auto suffix = std::to_string(
      static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())
  );
  auto dir = base / ("holder_db_test_" + suffix);
  std::filesystem::create_directories(dir);
  return dir;
}

} // namespace

TEST_CASE("Db move constructor transfers ownership", "[db]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "one.db";

  holder::platform::Db first;
  first.open(db_path);
  sqlite3* original_handle = first.handle();
  REQUIRE(original_handle != nullptr);

  holder::platform::Db moved(std::move(first));
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  REQUIRE(first.handle() == nullptr);
  REQUIRE(moved.handle() == original_handle);
  REQUIRE(moved.path() == db_path);
  REQUIRE_NOTHROW(moved.exec("CREATE TABLE IF NOT EXISTS t1 (id INTEGER);"));
}

TEST_CASE("Db move assignment transfers ownership and allows self move", "[db]") {
  const auto dir = make_temp_dir();
  const auto src_path = dir / "src.db";
  const auto dst_path = dir / "dst.db";

  holder::platform::Db source;
  source.open(src_path);
  sqlite3* source_handle = source.handle();
  REQUIRE(source_handle != nullptr);

  holder::platform::Db target;
  target.open(dst_path);
  REQUIRE(target.handle() != nullptr);

  target = std::move(source);
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  REQUIRE(source.handle() == nullptr);
  REQUIRE(target.handle() == source_handle);
  REQUIRE(target.path() == src_path);
  REQUIRE_NOTHROW(target.exec("CREATE TABLE IF NOT EXISTS t2 (id INTEGER);"));

  sqlite3* stable = target.handle();
  target = std::move(target);
  REQUIRE(target.handle() == stable);
}

TEST_CASE("Db open failure throws sqlite open error", "[db]") {
  const auto dir = make_temp_dir();
  const auto impossible_path = dir / "missing-parent" / "holder.db";

  holder::platform::Db db;
  REQUIRE_THROWS_WITH(
      db.open(impossible_path),
      Catch::Matchers::ContainsSubstring("sqlite open failed")
  );
}

TEST_CASE("Db exec failure includes sqlite message", "[db]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "bad-sql.db";

  holder::platform::Db db;
  db.open(db_path);

  REQUIRE_THROWS_WITH(
      db.exec("THIS IS NOT VALID SQL;"),
      Catch::Matchers::ContainsSubstring("sqlite exec failed")
  );
}

TEST_CASE("Tx destructor swallows rollback failure", "[db][tx]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "tx-rollback.db";

  holder::platform::Db db;
  db.open(db_path);

  REQUIRE_NOTHROW([&]() {
    holder::platform::Tx tx(db);
    db.close();
  }());
}
