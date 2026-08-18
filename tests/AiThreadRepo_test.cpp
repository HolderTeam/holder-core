#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "ai/AiThreadRepo.h"
#include "model/AiThread.h"
#include "model/Project.h"
#include "platform/Db.h"
#include "project/ProjectRepo.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sqlite3.h>
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
  auto dir = base / ("holder_ai_thread_test_" + suffix);
  std::filesystem::create_directories(dir);
  return dir;
}

void apply_schema(holder::platform::Db& db) {
  const auto schema_path = find_schema_sql();
  std::ifstream in(schema_path);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);
}

void create_project(holder::platform::Db& db, const std::string& project_id) {
  holder::project::ProjectRepo repo(db);
  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Project";
  project.root_path = "/tmp/project";
  project.created_at = 1;
  project.updated_at = 1;
  repo.create(project);
}

} // namespace

TEST_CASE("AiThreadRepo CRUD", "[aithreaddrepo]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  create_project(db, "proj-1");

  holder::ai::AiThreadRepo repo(db);

  holder::model::AiThread thread;
  thread.thread_id = "thread-1";
  thread.project_id = "proj-1";
  thread.card_id = std::nullopt;
  thread.title = "First";
  thread.created_at = 10;
  thread.updated_at = 10;

  repo.create(thread);

  const auto fetched = repo.get("thread-1");
  REQUIRE(fetched.has_value());
  REQUIRE(fetched->title == "First");
  REQUIRE(fetched->card_id.has_value() == false);

  auto list = repo.list("proj-1");
  REQUIRE(list.size() == 1);

  repo.update_title("thread-1", "Renamed", 20);
  const auto renamed = repo.get("thread-1");
  REQUIRE(renamed.has_value());
  REQUIRE(renamed->title == "Renamed");
  REQUIRE(renamed->updated_at == 20);

  repo.touch_updated("thread-1", 30);
  const auto touched = repo.get("thread-1");
  REQUIRE(touched.has_value());
  REQUIRE(touched->updated_at == 30);

  repo.remove("thread-1");
  REQUIRE_FALSE(repo.get("thread-1").has_value());
}

TEST_CASE("AiThreadRepo throws on insert/update/delete step failures", "[aithreaddrepo]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  create_project(db, "proj-1");

  holder::ai::AiThreadRepo repo(db);
  holder::model::AiThread thread;
  thread.thread_id = "thread-fail";
  thread.project_id = "proj-1";
  thread.title = "Fail";
  thread.created_at = 1;
  thread.updated_at = 1;

  db.exec("CREATE TRIGGER fail_ai_threads_insert BEFORE INSERT ON ai_threads "
          "BEGIN SELECT RAISE(ABORT, 'no insert'); END;");
  REQUIRE_THROWS(repo.create(thread));
  db.exec("DROP TRIGGER fail_ai_threads_insert;");

  thread.thread_id = "thread-ok";
  repo.create(thread);

  db.exec("CREATE TRIGGER fail_ai_threads_update BEFORE UPDATE ON ai_threads "
          "BEGIN SELECT RAISE(ABORT, 'no update'); END;");
  REQUIRE_THROWS(repo.update_title("thread-ok", "x", 2));
  REQUIRE_THROWS(repo.touch_updated("thread-ok", 3));
  db.exec("DROP TRIGGER fail_ai_threads_update;");

  db.exec("CREATE TRIGGER fail_ai_threads_delete BEFORE DELETE ON ai_threads "
          "BEGIN SELECT RAISE(ABORT, 'no delete'); END;");
  REQUIRE_THROWS(repo.remove("thread-ok"));
}

TEST_CASE("AiThreadRepo throws on get/list step error paths", "[aithreaddrepo]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  create_project(db, "proj-1");

  holder::ai::AiThreadRepo repo(db);
  holder::model::AiThread thread;
  thread.thread_id = "thread-int";
  thread.project_id = "proj-1";
  thread.title = "Interrupt";
  thread.created_at = 1;
  thread.updated_at = 1;
  repo.create(thread);

  sqlite3_progress_handler(
      db.handle(),
      1,
      [](void*) -> int {
        return 1;
      },
      nullptr
  );

  REQUIRE_THROWS(repo.get("thread-int"));
  REQUIRE_THROWS(repo.list("proj-1"));
  sqlite3_progress_handler(db.handle(), 0, nullptr, nullptr);
}

TEST_CASE("AiThreadRepo throws on prepare failures", "[aithreaddrepo]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  create_project(db, "proj-1");

  holder::ai::AiThreadRepo repo(db);
  db.exec("DROP TABLE ai_threads;");

  holder::model::AiThread thread;
  thread.thread_id = "thread-prepare";
  thread.project_id = "proj-1";
  thread.title = "Prepare";
  thread.created_at = 1;
  thread.updated_at = 1;

  REQUIRE_THROWS(repo.create(thread));
  REQUIRE_THROWS(repo.get("thread-prepare"));
  REQUIRE_THROWS(repo.list("proj-1"));
  REQUIRE_THROWS(repo.update_title("thread-prepare", "x", 2));
  REQUIRE_THROWS(repo.update_card_id("thread-prepare", std::nullopt));
  REQUIRE_THROWS(repo.touch_updated("thread-prepare", 3));
  REQUIRE_THROWS(repo.remove("thread-prepare"));
}

TEST_CASE("AiThreadRepo update_card_id fails on invalid card fk", "[aithreaddrepo]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  create_project(db, "proj-1");

  holder::ai::AiThreadRepo repo(db);
  holder::model::AiThread thread;
  thread.thread_id = "thread-card-fk";
  thread.project_id = "proj-1";
  thread.title = "Card FK";
  thread.created_at = 1;
  thread.updated_at = 1;
  repo.create(thread);

  REQUIRE_THROWS(repo.update_card_id("thread-card-fk", std::optional<std::string>("missing-card")));
}

TEST_CASE("AiThreadRepo list throws when interrupted during large scan", "[aithreaddrepo]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  create_project(db, "proj-1");

  holder::ai::AiThreadRepo repo(db);
  for (int i = 0; i < 5000; ++i) {
    holder::model::AiThread thread;
    thread.thread_id = "thread-many-" + std::to_string(i);
    thread.project_id = "proj-1";
    thread.title = "Many";
    thread.created_at = i + 1;
    thread.updated_at = i + 1;
    repo.create(thread);
  }

  int callback_count = 0;
  sqlite3_progress_handler(
      db.handle(),
      1,
      [](void* ctx) -> int {
        auto* count = static_cast<int*>(ctx);
        ++(*count);
        return (*count > 2000) ? 1 : 0;
      },
      &callback_count
  );
  REQUIRE_THROWS(repo.list("proj-1"));
  sqlite3_progress_handler(db.handle(), 0, nullptr, nullptr);
}
