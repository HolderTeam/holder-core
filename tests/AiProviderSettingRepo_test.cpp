#include "ai/AiProviderSettingRepo.h"
#include "platform/Db.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
#include <sqlite3.h>

namespace {
int sqlite_interrupt_cb(void*) { return 1; }
} // namespace

TEST_CASE("AiProviderSettingRepo upsert/list/remove", "[db]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_ai_provider_settings";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);

  holder::ai::AiProviderSettingRepo repo(db);
  REQUIRE(repo.list().empty());

  repo.upsert("switchyard", true, 100);
  auto one = repo.get("switchyard");
  REQUIRE(one.has_value());
  REQUIRE(one->provider == "switchyard");
  REQUIRE(one->enabled == true);
  REQUIRE(one->updated_at == 100);

  repo.upsert("switchyard", false, 200);
  one = repo.get("switchyard");
  REQUIRE(one.has_value());
  REQUIRE(one->enabled == false);
  REQUIRE(one->updated_at == 200);

  repo.upsert("chadjeopardy", true, 300);
  const auto rows = repo.list();
  REQUIRE(rows.size() == 2);

  repo.remove("switchyard");
  REQUIRE_FALSE(repo.get("switchyard").has_value());
}

TEST_CASE("AiProviderSettingRepo throws when get prepare fails", "[db]") {
  const auto dir = std::filesystem::temp_directory_path() /
                   "holder_ai_provider_settings_get_prepare_fail";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);
  db.exec("DROP TABLE ai_provider_settings;");

  holder::ai::AiProviderSettingRepo repo(db);
  REQUIRE_THROWS_WITH(
      repo.list(),
      Catch::Matchers::ContainsSubstring("prepare list provider settings failed: no such table: ai_provider_settings")
  );
  REQUIRE_THROWS_WITH(
      repo.get("switchyard"),
      Catch::Matchers::ContainsSubstring("prepare get provider setting failed: no such table: ai_provider_settings")
  );
  REQUIRE_THROWS_WITH(
      repo.upsert("switchyard", true, 1),
      Catch::Matchers::ContainsSubstring("prepare upsert provider setting failed: no such table: ai_provider_settings")
  );
  REQUIRE_THROWS_WITH(
      repo.remove("switchyard"),
      Catch::Matchers::ContainsSubstring("prepare delete provider setting failed: no such table: ai_provider_settings")
  );
}

TEST_CASE("AiProviderSettingRepo throws when upsert step fails", "[db]") {
  const auto dir = std::filesystem::temp_directory_path() /
                   "holder_ai_provider_settings_upsert_step_fail";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);
  db.exec("CREATE TRIGGER fail_ai_provider_settings_insert BEFORE INSERT ON ai_provider_settings "
          "BEGIN SELECT RAISE(ABORT, 'no insert'); END;");

  holder::ai::AiProviderSettingRepo repo(db);
  REQUIRE_THROWS_WITH(
      repo.upsert("switchyard", true, 1),
      Catch::Matchers::ContainsSubstring("upsert provider setting failed: no insert")
  );
}

TEST_CASE("AiProviderSettingRepo throws when delete step fails", "[db]") {
  const auto dir = std::filesystem::temp_directory_path() /
                   "holder_ai_provider_settings_delete_step_fail";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);

  holder::ai::AiProviderSettingRepo repo(db);
  repo.upsert("switchyard", true, 1);

  db.exec("CREATE TRIGGER fail_ai_provider_settings_delete BEFORE DELETE ON ai_provider_settings "
          "BEGIN SELECT RAISE(ABORT, 'no delete'); END;");
  REQUIRE_THROWS_WITH(
      repo.remove("switchyard"),
      Catch::Matchers::ContainsSubstring("delete provider setting failed: no delete")
  );
}

TEST_CASE("AiProviderSettingRepo throws when list/get step is interrupted", "[db]") {
  const auto dir = std::filesystem::temp_directory_path() /
                   "holder_ai_provider_settings_step_interrupt";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);

  holder::ai::AiProviderSettingRepo repo(db);
  repo.upsert("switchyard", true, 1);

  sqlite3_progress_handler(db.handle(), 1, sqlite_interrupt_cb, nullptr);
  REQUIRE_THROWS_WITH(
      repo.list(),
      Catch::Matchers::ContainsSubstring("list provider settings failed: interrupted")
  );
  REQUIRE_THROWS_WITH(
      repo.get("switchyard"),
      Catch::Matchers::ContainsSubstring("get provider setting failed: interrupted")
  );
  sqlite3_progress_handler(db.handle(), 0, nullptr, nullptr);
}
