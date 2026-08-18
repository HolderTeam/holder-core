#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "index/FtsIndexer.h"
#include "platform/Db.h"

#include <chrono>
#include <cmath>
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
  auto dir = base / ("holder_fts_test_" + suffix);
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

int count_cards_fts(holder::platform::Db& db, const std::string& query) {
  static constexpr const char* SQL = "SELECT count(*) FROM cards_fts WHERE cards_fts MATCH ?;";
  sqlite3_stmt* stmt = nullptr;
  REQUIRE(sqlite3_prepare_v2(db.handle(), SQL, -1, &stmt, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_bind_text(stmt, 1, query.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK);
  int count = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    count = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return count;
}

int count_ai_fts(holder::platform::Db& db, const std::string& query) {
  static constexpr const char* SQL = "SELECT count(*) FROM ai_fts WHERE ai_fts MATCH ?;";
  sqlite3_stmt* stmt = nullptr;
  REQUIRE(sqlite3_prepare_v2(db.handle(), SQL, -1, &stmt, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_bind_text(stmt, 1, query.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK);
  int count = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    count = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return count;
}

} // namespace

TEST_CASE("FtsIndexer card upsert/delete", "[fts]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);

  holder::index::FtsIndexer fts(db);
  fts.upsert_card("card-1", "proj-1", "Title", "Hello world");
  REQUIRE(count_cards_fts(db, "hello") == 1);

  fts.upsert_card("card-1", "proj-1", "Title", "Goodbye world");
  REQUIRE(count_cards_fts(db, "hello") == 0);
  REQUIRE(count_cards_fts(db, "goodbye") == 1);

  fts.delete_card("card-1");
  REQUIRE(count_cards_fts(db, "goodbye") == 0);
}

TEST_CASE("FtsIndexer message upsert/delete", "[fts]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);

  holder::index::FtsIndexer fts(db);
  fts.upsert_message("msg-1", "thread-1", "proj-1", "alpha beta");
  REQUIRE(count_ai_fts(db, "alpha") == 1);

  fts.upsert_message("msg-1", "thread-1", "proj-1", "gamma");
  REQUIRE(count_ai_fts(db, "alpha") == 0);
  REQUIRE(count_ai_fts(db, "gamma") == 1);

  fts.delete_message("msg-1");
  REQUIRE(count_ai_fts(db, "gamma") == 0);
}

TEST_CASE("FtsIndexer search returns snippets", "[fts]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);

  holder::index::FtsIndexer fts(db);
  {
    static constexpr const char* SQL_PROJECT =
        "INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
        "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);";
    db.exec(SQL_PROJECT);
    static constexpr const char* SQL_CARD =
        "INSERT INTO cards(card_id, project_id, title, rel_path, sort_key, created_at, updated_at) "
        "VALUES('card-1', 'proj-1', 'Title', 'cards/xx/yy/card-1.md', 0.0, 1, 2);";
    db.exec(SQL_CARD);
  }
  fts.upsert_card("card-1", "proj-1", "Title", "The quick brown fox");
  const auto rows = fts.search_cards("proj-1", "brown", 10, 0);
  REQUIRE(rows.size() == 1);
  REQUIRE(rows[0].id == "card-1");
  REQUIRE(rows[0].title == "Title");
  REQUIRE(rows[0].created_at == 1);
  REQUIRE(rows[0].updated_at == 2);
  REQUIRE(std::isfinite(rows[0].rank));
  REQUIRE(rows[0].snippet.find('[') != std::string::npos);
}

TEST_CASE("FtsIndexer search_cards handles empty and pre-bracketed snippets", "[fts]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);

  holder::index::FtsIndexer fts(db);
  {
    static constexpr const char* SQL_PROJECT =
        "INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
        "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);";
    db.exec(SQL_PROJECT);
    static constexpr const char* SQL_CARD_EMPTY =
        "INSERT INTO cards(card_id, project_id, title, rel_path, sort_key, created_at, updated_at) "
        "VALUES('card-empty', 'proj-1', 'NeedleTitle', 'cards/a/b/card-empty.md', 0.0, 1, 1);";
    static constexpr const char* SQL_CARD_BRACKET =
        "INSERT INTO cards(card_id, project_id, title, rel_path, sort_key, created_at, updated_at) "
        "VALUES('card-bracket', 'proj-1', 'OtherTitle', 'cards/a/b/card-bracket.md', 0.0, 1, 1);";
    db.exec(SQL_CARD_EMPTY);
    db.exec(SQL_CARD_BRACKET);
  }

  // Query hits title while body is empty. Depending on SQLite FTS build/config,
  // snippet() for the body column may be empty or return highlighted text.
  fts.upsert_card("card-empty", "proj-1", "NeedleTitle", "");
  auto empty_rows = fts.search_cards("proj-1", "NeedleTitle", 10, 0);
  REQUIRE(empty_rows.size() == 1);
  REQUIRE(empty_rows[0].id == "card-empty");
  REQUIRE((empty_rows[0].snippet.empty() || empty_rows[0].snippet.find('[') != std::string::npos));

  // Body contains literal brackets and match term; snippet should be preserved as already
  // bracketed.
  fts.upsert_card("card-bracket", "proj-1", "OtherTitle", "alpha [literal] omega");
  auto bracket_rows = fts.search_cards("proj-1", "literal", 10, 0);
  REQUIRE(bracket_rows.size() == 1);
  REQUIRE(bracket_rows[0].id == "card-bracket");
  REQUIRE(bracket_rows[0].snippet.find('[') != std::string::npos);
}

TEST_CASE("FtsIndexer search_messages handles empty and non-bracket snippets", "[fts]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);

  holder::index::FtsIndexer fts(db);
  {
    static constexpr const char* SQL_PROJECT =
        "INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
        "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);";
    static constexpr const char* SQL_THREAD =
        "INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
        "VALUES('thread1', 'proj-1', 'T', 1, 1);";
    static constexpr const char* SQL_MSG_EMPTY =
        "INSERT INTO ai_messages(message_id, thread_id, role, source, content, created_at) "
        "VALUES('msg-empty', 'thread1', 'assistant', 'local', '', 1);";
    static constexpr const char* SQL_MSG_PLAIN =
        "INSERT INTO ai_messages(message_id, thread_id, role, source, content, created_at) "
        "VALUES('msg-plain', 'thread1', 'assistant', 'local', 'plain body snippet', 2);";
    db.exec(SQL_PROJECT);
    db.exec(SQL_THREAD);
    db.exec(SQL_MSG_EMPTY);
    db.exec(SQL_MSG_PLAIN);
  }

  // Already-highlighted/bracketed snippets should be preserved.
  fts.upsert_message("msg-empty", "thread-1", "proj-1", "alpha [literal] omega");
  auto bracket_rows = fts.search_messages("proj-1", "literal", 10, 0);
  REQUIRE(bracket_rows.size() >= 1);
  bool saw_bracketed = false;
  for (const auto& row : bracket_rows) {
    if (row.id == "msg-empty") {
      saw_bracketed = true;
      REQUIRE(row.snippet.find('[') != std::string::npos);
    }
  }
  REQUIRE(saw_bracketed);

  // Non-highlighted snippet path should be wrapped in [] by FtsIndexer.
  fts.upsert_message("msg-plain", "thread-1", "proj-1", "plain body snippet");
  auto plain_rows = fts.search_messages("proj-1", "plain", 10, 0);
  REQUIRE(plain_rows.size() >= 1);
  bool saw_wrapped = false;
  for (const auto& row : plain_rows) {
    if (row.id == "msg-plain") {
      saw_wrapped = true;
      REQUIRE_FALSE(row.snippet.empty());
      REQUIRE(row.snippet.front() == '[');
    }
  }
  REQUIRE(saw_wrapped);
}

TEST_CASE("FtsIndexer throws when sqlite handle is closed", "[fts]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);

  holder::index::FtsIndexer fts(db);
  db.close();

  REQUIRE_THROWS(fts.upsert_card("card-1", "proj-1", "Title", "Body"));
}

namespace {
int sqlite_interrupt_cb(void* data) {
  auto* flag = static_cast<int*>(data);
  return (flag && *flag) ? 1 : 0;
}

int sqlite_deny_insert_into_table(void* data, int action, const char* detail1, const char* detail2, const char*, const char*) {
  (void)detail2;
  const char* deny_table = static_cast<const char*>(data);
  if (action == SQLITE_INSERT && detail1 != nullptr && deny_table != nullptr &&
      std::string(detail1) == std::string(deny_table)) {
    return SQLITE_DENY;
  }
  return SQLITE_OK;
}
} // namespace

TEST_CASE("FtsIndexer delete/upsert throw on interrupted sqlite step", "[fts]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);

  holder::index::FtsIndexer fts(db);
  fts.upsert_card("card-1", "proj-1", "Title", "Body");
  fts.upsert_message("msg-1", "thread-1", "proj-1", "alpha beta");

  int interrupt_on = 1;
  sqlite3_progress_handler(db.handle(), 1, sqlite_interrupt_cb, &interrupt_on);
  REQUIRE_THROWS(fts.delete_card("card-1"));
  REQUIRE_THROWS(fts.delete_message("msg-1"));
  REQUIRE_THROWS(fts.upsert_card("card-1", "proj-1", "Title", "Body"));
  REQUIRE_THROWS(fts.upsert_message("msg-2", "thread-2", "proj-1", "gamma"));
  sqlite3_progress_handler(db.handle(), 0, nullptr, nullptr);
}

TEST_CASE("FtsIndexer search throws on interrupted sqlite step", "[fts]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);

  holder::index::FtsIndexer fts(db);
  {
    static constexpr const char* SQL_PROJECT =
        "INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
        "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);";
    static constexpr const char* SQL_CARD =
        "INSERT INTO cards(card_id, project_id, title, rel_path, sort_key, created_at, updated_at) "
        "VALUES('card-1', 'proj-1', 'Title', 'cards/xx/yy/card-1.md', 0.0, 1, 2);";
    static constexpr const char* SQL_THREAD =
        "INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
        "VALUES('thread-1', 'proj-1', 'T', 1, 1);";
    static constexpr const char* SQL_MSG =
        "INSERT INTO ai_messages(message_id, thread_id, role, source, content, created_at) "
        "VALUES('msg-1', 'thread-1', 'assistant', 'local', 'alpha beta', 1);";
    db.exec(SQL_PROJECT);
    db.exec(SQL_CARD);
    db.exec(SQL_THREAD);
    db.exec(SQL_MSG);
  }
  fts.upsert_card("card-1", "proj-1", "Title", "alpha beta");
  fts.upsert_message("msg-1", "thread-1", "proj-1", "alpha beta");

  int interrupt_on = 1;
  sqlite3_progress_handler(db.handle(), 1, sqlite_interrupt_cb, &interrupt_on);
  REQUIRE_THROWS(fts.search_cards("proj-1", "alpha", 10, 0));
  REQUIRE_THROWS(fts.search_messages("proj-1", "alpha", 10, 0));
  sqlite3_progress_handler(db.handle(), 0, nullptr, nullptr);
}

TEST_CASE("FtsIndexer search throws when join tables are missing", "[fts]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);

  holder::index::FtsIndexer fts(db);
  db.exec("DROP TABLE cards;");
  db.exec("DROP TABLE ai_messages;");

  REQUIRE_THROWS(fts.search_cards("proj-1", "alpha", 10, 0));
  REQUIRE_THROWS(fts.search_messages("proj-1", "alpha", 10, 0));
}

TEST_CASE("FtsIndexer upsert throws when insert prepare is denied by authorizer", "[fts]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);

  holder::index::FtsIndexer fts(db);
  const char* deny_cards = "cards_fts";
  sqlite3_set_authorizer(db.handle(), sqlite_deny_insert_into_table, const_cast<char*>(deny_cards));
  REQUIRE_THROWS(fts.upsert_card("card-1", "proj-1", "Title", "Body"));

  const char* deny_ai = "ai_fts";
  sqlite3_set_authorizer(db.handle(), sqlite_deny_insert_into_table, const_cast<char*>(deny_ai));
  REQUIRE_THROWS(fts.upsert_message("msg-1", "thread-1", "proj-1", "alpha beta"));

  sqlite3_set_authorizer(db.handle(), nullptr, nullptr);
}

TEST_CASE("FtsIndexer delete throws when sqlite handle is closed", "[fts]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);

  holder::index::FtsIndexer fts(db);
  db.close();

  REQUIRE_THROWS(fts.delete_card("card-1"));
  REQUIRE_THROWS(fts.delete_message("msg-1"));
}

TEST_CASE("FtsIndexer upsert_message throws when sqlite handle is closed", "[fts]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);

  holder::index::FtsIndexer fts(db);
  db.close();

  REQUIRE_THROWS(fts.upsert_message("msg-1", "thread-1", "proj-1", "alpha beta"));
}
