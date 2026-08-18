#include "ai/AiProviderSettingRepo.h"
#include "platform/Db.h"

#include <catch2/catch_test_macros.hpp>

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
  REQUIRE_THROWS(repo.get("switchyard"));
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
  REQUIRE_THROWS(repo.upsert("switchyard", true, 1));
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
  REQUIRE_THROWS(repo.remove("switchyard"));
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
  REQUIRE_THROWS(repo.list());
  REQUIRE_THROWS(repo.get("switchyard"));
  sqlite3_progress_handler(db.handle(), 0, nullptr, nullptr);
}
