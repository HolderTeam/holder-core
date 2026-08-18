#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "index/Reindexer.h"
#include "platform/Db.h"

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
  auto dir = base / ("holder_reindex_test_" + suffix);
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

int count_table(holder::platform::Db& db, const std::string& table) {
  const std::string sql = "SELECT count(*) FROM " + table + ";";
  sqlite3_stmt* stmt = nullptr;
  REQUIRE(sqlite3_prepare_v2(db.handle(), sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
  int count = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    count = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return count;
}

struct InterruptAfter {
  int remaining;
};

int sqlite_interrupt_after(void* data) {
  auto* interrupt = static_cast<InterruptAfter*>(data);
  if (interrupt == nullptr) return 0;
  if (interrupt->remaining <= 0) return 1;
  --interrupt->remaining;
  return 0;
}

} // namespace

TEST_CASE("Reindexer rebuilds FTS tables", "[reindex]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);

  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");
  db.exec(
      "INSERT INTO cards(card_id, project_id, title, rel_path, sort_key, created_at, updated_at) "
      "VALUES('card-1', 'proj-1', 'Title', 'cards/aa/bb/card-1.md', 0.0, 1, 1);"
  );
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
          "VALUES('thread-1', 'proj-1', 'Thread', 2, 2);");
  db.exec("INSERT INTO ai_messages(message_id, thread_id, role, source, content, created_at) "
          "VALUES('msg-1', 'thread-1', 'user', 'manual', 'Hello', 3);");

  db.exec("DELETE FROM cards_fts;");
  db.exec("DELETE FROM ai_fts;");

  holder::index::Reindexer reindexer(db);
  reindexer.run();

  REQUIRE(count_table(db, "cards_fts") == 1);
  REQUIRE(count_table(db, "ai_fts") == 1);
}

TEST_CASE("Reindexer throws when cards query prepare fails", "[reindex]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);

  db.exec("DROP TABLE cards;");

  holder::index::Reindexer reindexer(db);
  REQUIRE_THROWS(reindexer.run());
}

TEST_CASE("Reindexer throws when ai messages query prepare fails", "[reindex]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);

  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");
  db.exec(
      "INSERT INTO cards(card_id, project_id, title, rel_path, sort_key, created_at, updated_at) "
      "VALUES('card-1', 'proj-1', 'Title', 'cards/aa/bb/card-1.md', 0.0, 1, 1);"
  );
  db.exec("DROP TABLE ai_messages;");

  holder::index::Reindexer reindexer(db);
  REQUIRE_THROWS(reindexer.run());
}

TEST_CASE("Reindexer throws when cards scan is interrupted", "[reindex]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);

  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");

  for (int i = 0; i < 5000; ++i) {
    const std::string id = "card-" + std::to_string(i);
    std::string insert =
        "INSERT INTO cards(card_id, project_id, title, rel_path, sort_key, created_at, updated_at) "
        "VALUES('";
    insert += id;
    insert += "', 'proj-1', 'Title', 'cards/aa/bb/";
    insert += id;
    insert += ".md', 0.0, 1, 1);";
    db.exec(insert);
  }

  InterruptAfter interrupt{1000};
  sqlite3_progress_handler(db.handle(), 1, sqlite_interrupt_after, &interrupt);
  holder::index::Reindexer reindexer(db);
  REQUIRE_THROWS(reindexer.run());
  sqlite3_progress_handler(db.handle(), 0, nullptr, nullptr);
}

TEST_CASE("Reindexer throws when ai messages scan is interrupted", "[reindex]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);

  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
          "VALUES('thread-1', 'proj-1', 'Thread', 2, 2);");

  for (int i = 0; i < 5000; ++i) {
    const std::string id = "msg-" + std::to_string(i);
    db.exec(
        "INSERT INTO ai_messages(message_id, thread_id, role, source, content, created_at) "
        "VALUES('" +
        id + "', 'thread-1', 'user', 'manual', 'Hello', 3);"
    );
  }

  InterruptAfter interrupt{5000};
  sqlite3_progress_handler(db.handle(), 1, sqlite_interrupt_after, &interrupt);
  holder::index::Reindexer reindexer(db);
  REQUIRE_THROWS(reindexer.run());
  sqlite3_progress_handler(db.handle(), 0, nullptr, nullptr);
}
