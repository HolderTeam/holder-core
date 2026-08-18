#include "ai/AiProviderCredentialRepo.h"
#include "platform/Db.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sqlite3.h>

namespace {
int sqlite_interrupt_cb(void*) { return 1; }
} // namespace

TEST_CASE("AiProviderCredentialRepo upsert/list/remove", "[db]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_ai_provider_credentials";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);

  holder::ai::AiProviderCredentialRepo repo(db);
  REQUIRE(repo.list().empty());

  repo.upsert("chocolatefactory", "key-1", 100, 100);
  auto one = repo.get("chocolatefactory");
  REQUIRE(one.has_value());
  REQUIRE(one->provider == "chocolatefactory");
  REQUIRE(one->api_key_preview == "key-1");
  REQUIRE(one->created_at == 100);
  REQUIRE(one->updated_at == 100);

  repo.upsert("chocolatefactory", "key-2", 999, 200);
  one = repo.get("chocolatefactory");
  REQUIRE(one.has_value());
  REQUIRE(one->api_key_preview == "key-2");
  REQUIRE(one->created_at == 100);
  REQUIRE(one->updated_at == 200);

  repo.upsert("openai", "other", 300, 300);
  const auto rows = repo.list();
  REQUIRE(rows.size() == 2);

  repo.remove("chocolatefactory");
  REQUIRE_FALSE(repo.get("chocolatefactory").has_value());
}

TEST_CASE("AiProviderCredentialRepo throws when upsert prepare fails", "[db]") {
  const auto dir = std::filesystem::temp_directory_path() /
                   "holder_ai_provider_credentials_prepare_fail";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);
  db.exec("DROP TABLE ai_provider_credentials;");

  holder::ai::AiProviderCredentialRepo repo(db);
  REQUIRE_THROWS(repo.upsert("openai", "k", 1, 1));
}

TEST_CASE("AiProviderCredentialRepo throws when upsert step fails", "[db]") {
  const auto dir = std::filesystem::temp_directory_path() /
                   "holder_ai_provider_credentials_upsert_step_fail";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);
  db.exec("CREATE TRIGGER fail_ai_provider_insert BEFORE INSERT ON ai_provider_credentials "
          "BEGIN SELECT RAISE(ABORT, 'no insert'); END;");

  holder::ai::AiProviderCredentialRepo repo(db);
  REQUIRE_THROWS(repo.upsert("openai", "k", 1, 1));
}

TEST_CASE("AiProviderCredentialRepo throws when delete step fails", "[db]") {
  const auto dir = std::filesystem::temp_directory_path() /
                   "holder_ai_provider_credentials_delete_step_fail";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);

  holder::ai::AiProviderCredentialRepo repo(db);
  repo.upsert("openai", "k", 1, 1);

  db.exec("CREATE TRIGGER fail_ai_provider_delete BEFORE DELETE ON ai_provider_credentials "
          "BEGIN SELECT RAISE(ABORT, 'no delete'); END;");
  REQUIRE_THROWS(repo.remove("openai"));
}

TEST_CASE("AiProviderCredentialRepo throws when list/get step is interrupted", "[db]") {
  const auto dir = std::filesystem::temp_directory_path() /
                   "holder_ai_provider_credentials_step_interrupt";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);

  holder::ai::AiProviderCredentialRepo repo(db);
  repo.upsert("openai", "k", 1, 1);

  sqlite3_progress_handler(db.handle(), 1, sqlite_interrupt_cb, nullptr);
  REQUIRE_THROWS(repo.list());
  REQUIRE_THROWS(repo.get("openai"));
  sqlite3_progress_handler(db.handle(), 0, nullptr, nullptr);
}
