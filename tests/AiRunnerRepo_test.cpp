#include "ai/AiRunnerRepo.h"
#include "platform/Db.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <optional>

TEST_CASE("AiRunnerRepo stores and removes manual runners", "[db]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_ai_runner_repo";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  holder::platform::Db db;
  db.open(dir / "holder.db");

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);

  holder::ai::AiRunnerRepo repo(db);

  holder::model::AiRunner runner{
      .runner_id = "manual-test",
      .name = "Test Runner",
      .kind = "ollama",
      .base_url = std::optional<std::string>("http://10.0.0.5:11434"),
      .source = "manual",
      .enabled = true,
      .created_at = 10,
      .updated_at = 10,
  };

  repo.upsert(runner);

  holder::model::AiRunner earlier{
      .runner_id = "manual-empty-base-url",
      .name = "Empty Base URL",
      .kind = "openai-compatible",
      .base_url = std::nullopt,
      .source = "manual",
      .enabled = true,
      .created_at = 5,
      .updated_at = 5,
  };
  repo.upsert(earlier);

  const auto rows = repo.list();
  REQUIRE(rows.size() == 2);
  REQUIRE(rows[0].runner_id == "manual-empty-base-url");
  REQUIRE_FALSE(rows[0].base_url.has_value());
  REQUIRE(rows[1].runner_id == "manual-test");

  auto stored = repo.get("manual-test");
  REQUIRE(stored.has_value());
  REQUIRE(stored->name == "Test Runner");
  REQUIRE(stored->base_url == std::optional<std::string>("http://10.0.0.5:11434"));

  runner.name = "Test Runner 2";
  runner.enabled = false;
  runner.updated_at = 20;
  repo.upsert(runner);

  stored = repo.get("manual-test");
  REQUIRE(stored.has_value());
  REQUIRE(stored->name == "Test Runner 2");
  REQUIRE(stored->enabled == false);
  REQUIRE(stored->created_at == 10);
  REQUIRE(stored->updated_at == 20);

  repo.remove("manual-test");
  REQUIRE_FALSE(repo.get("manual-test").has_value());
}
