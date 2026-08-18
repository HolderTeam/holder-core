#include "ai/AiRunRepo.h"
#include "platform/Db.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sqlite3.h>

namespace {
int sqlite_interrupt_cb(void*) { return 1; }
} // namespace

TEST_CASE("AiRunRepo create/get/update", "[db]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_ai_runs";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);

  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
          "VALUES('thread-1', 'proj-1', 'Thread', 1, 1);");
  db.exec("INSERT INTO ai_messages(message_id, thread_id, role, source, content, created_at) "
          "VALUES('msg-1', 'thread-1', 'assistant', 'local', 'hi', 1);");

  holder::ai::AiRunRepo repo(db);
  holder::model::AiRun run;
  run.run_id = "run-1";
  run.project_id = "proj-1";
  run.thread_id = "thread-1";
  run.mode = "auto";
  run.prompt = "hello";
  run.status = "started";
  run.created_at = 1;
  run.updated_at = 1;
  repo.create(run);

  const auto fetched = repo.get("run-1");
  REQUIRE(fetched.has_value());
  REQUIRE(fetched->run_id == "run-1");
  REQUIRE(fetched->status == "started");

  repo.update_status(
      "run-1",
      "completed",
      std::nullopt,
      "msg-1",
      "model-1",
      std::nullopt,
      std::optional<std::string>(R"({"path":"cloud","result":{"status":"completed"}})"),
      2
  );
  const auto updated = repo.get("run-1");
  REQUIRE(updated.has_value());
  REQUIRE(updated->status == "completed");
  REQUIRE(updated->message_id.value() == "msg-1");
  REQUIRE(updated->chosen_model.value() == "model-1");
  REQUIRE(updated->policy_trace_json.has_value());
}

TEST_CASE("AiRunRepo create covers nullable and optional insert fields", "[db]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_ai_runs_optional_fields";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);
  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
          "VALUES('thread-1', 'proj-1', 'Thread', 1, 1);");
  db.exec("INSERT INTO ai_messages(message_id, thread_id, role, source, content, created_at) "
          "VALUES('msg-x', 'thread-1', 'assistant', 'local', 'hi', 1);");

  holder::ai::AiRunRepo repo(db);
  holder::model::AiRun run;
  run.run_id = "run-optional";
  run.project_id = std::nullopt;
  run.thread_id = std::nullopt;
  run.message_id = std::optional<std::string>("msg-x");
  run.mode = "manual";
  run.prompt = "hello";
  run.status = "failed";
  run.error = std::optional<std::string>("boom");
  run.created_at = 10;
  run.updated_at = 10;
  repo.create(run);

  const auto fetched = repo.get("run-optional");
  REQUIRE(fetched.has_value());
  REQUIRE_FALSE(fetched->project_id.has_value());
  REQUIRE_FALSE(fetched->thread_id.has_value());
  REQUIRE(fetched->message_id.has_value());
  REQUIRE(fetched->error.has_value());
}

TEST_CASE("AiRunRepo list_by_thread and list_by_project return newest first", "[db]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_ai_runs_lists";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);

  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
          "VALUES('thread-1', 'proj-1', 'Thread', 1, 1);");

  holder::ai::AiRunRepo repo(db);
  holder::model::AiRun older;
  older.run_id = "run-old";
  older.project_id = "proj-1";
  older.thread_id = "thread-1";
  older.mode = "auto";
  older.prompt = "old";
  older.status = "completed";
  older.created_at = 100;
  older.updated_at = 100;
  repo.create(older);

  holder::model::AiRun newer = older;
  newer.run_id = "run-new";
  newer.prompt = "new";
  newer.created_at = 200;
  newer.updated_at = 200;
  repo.create(newer);

  const auto by_thread = repo.list_by_thread("thread-1");
  REQUIRE(by_thread.size() == 2);
  REQUIRE(by_thread[0].run_id == "run-new");
  REQUIRE(by_thread[1].run_id == "run-old");

  const auto by_project = repo.list_by_project("proj-1");
  REQUIRE(by_project.size() == 2);
  REQUIRE(by_project[0].run_id == "run-new");
  REQUIRE(by_project[1].run_id == "run-old");
}

TEST_CASE("AiRunRepo throws when insert step fails", "[db]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_ai_runs_insert_fail";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);
  db.exec("CREATE TRIGGER fail_ai_runs_insert BEFORE INSERT ON ai_runs "
          "BEGIN SELECT RAISE(ABORT, 'no insert'); END;");

  holder::ai::AiRunRepo repo(db);
  holder::model::AiRun run;
  run.run_id = "run-fail";
  run.mode = "auto";
  run.prompt = "x";
  run.status = "started";
  run.created_at = 1;
  run.updated_at = 1;
  REQUIRE_THROWS(repo.create(run));
}

TEST_CASE("AiRunRepo throws when table missing for prepare paths", "[db]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_ai_runs_prepare_fail";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);
  db.exec("DROP TABLE ai_runs;");

  holder::ai::AiRunRepo repo(db);
  holder::model::AiRun run;
  run.run_id = "run-prepare";
  run.mode = "auto";
  run.prompt = "x";
  run.status = "started";
  run.created_at = 1;
  run.updated_at = 1;

  REQUIRE_THROWS(repo.create(run));
  REQUIRE_THROWS(repo.list_by_thread("thread-1"));
  REQUIRE_THROWS(repo.list_by_project("proj-1"));
  REQUIRE_THROWS(repo.update_status(
      "run-1",
      "failed",
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      2
  ));
}

TEST_CASE("AiRunRepo throws when update step fails", "[db]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_ai_runs_update_fail";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);

  holder::ai::AiRunRepo repo(db);
  holder::model::AiRun run;
  run.run_id = "run-update-fail";
  run.mode = "auto";
  run.prompt = "x";
  run.status = "started";
  run.created_at = 1;
  run.updated_at = 1;
  repo.create(run);

  db.exec("CREATE TRIGGER fail_ai_runs_update BEFORE UPDATE ON ai_runs "
          "BEGIN SELECT RAISE(ABORT, 'no update'); END;");
  REQUIRE_THROWS(repo.update_status(
      "run-update-fail",
      "failed",
      std::optional<std::string>("err"),
      std::optional<std::string>("msg"),
      std::optional<std::string>("m"),
      std::optional<std::string>("[]"),
      std::optional<std::string>("{}"),
      2
  ));
}

TEST_CASE("AiRunRepo throws when get step is interrupted", "[db]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_ai_runs_get_interrupt";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);

  holder::ai::AiRunRepo repo(db);
  holder::model::AiRun run;
  run.run_id = "run-get-interrupt";
  run.mode = "auto";
  run.prompt = "x";
  run.status = "started";
  run.created_at = 1;
  run.updated_at = 1;
  repo.create(run);

  sqlite3_progress_handler(db.handle(), 1, sqlite_interrupt_cb, nullptr);
  REQUIRE_THROWS(repo.get("run-get-interrupt"));
  sqlite3_progress_handler(db.handle(), 0, nullptr, nullptr);
}
