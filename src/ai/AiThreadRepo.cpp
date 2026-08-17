#include "ai/AiThreadRepo.h"

#include <sqlite3.h>

#include <stdexcept>
#include <utility>

namespace holder::ai {
namespace {

void throw_sqlite(sqlite3* db, const std::string& what) {
  const char* msg = db ? sqlite3_errmsg(db) : "unknown sqlite error";
  throw std::runtime_error(what + ": " + msg);
}

void bind_text(sqlite3_stmt* stmt, int idx, const std::string& value) {
  if (sqlite3_bind_text(stmt, idx, value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
    throw std::runtime_error("sqlite bind_text failed"); // LCOV_EXCL_LINE
  }
}

void bind_text_optional(sqlite3_stmt* stmt, int idx, const std::optional<std::string>& value) {
  if (value.has_value()) {
    bind_text(stmt, idx, value.value());
  } else if (sqlite3_bind_null(stmt, idx) != SQLITE_OK) {
    throw std::runtime_error("sqlite bind_null failed"); // LCOV_EXCL_LINE
  }
}

void bind_int64(sqlite3_stmt* stmt, int idx, long long value) {
  if (sqlite3_bind_int64(stmt, idx, static_cast<sqlite3_int64>(value)) != SQLITE_OK) {
    throw std::runtime_error("sqlite bind_int64 failed"); // LCOV_EXCL_LINE
  }
}

holder::model::AiThread read_thread(sqlite3_stmt* stmt) {
  holder::model::AiThread t;
  t.thread_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  t.project_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  if (sqlite3_column_type(stmt, 2) == SQLITE_NULL) {
    t.card_id.reset();
  } else {
    t.card_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
  }
  t.title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
  t.created_at = sqlite3_column_int64(stmt, 4);
  t.updated_at = sqlite3_column_int64(stmt, 5);
  return t;
} // LCOV_EXCL_LINE

} // namespace

AiThreadRepo::AiThreadRepo(holder::platform::Db& db)
    : db_(db) {}

void AiThreadRepo::create(const holder::model::AiThread& thread) {
  static constexpr const char* SQL =
      "INSERT INTO ai_threads(thread_id, project_id, card_id, title, created_at, updated_at) "
      "VALUES(?, ?, ?, ?, ?, ?);";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare insert ai thread failed");
  }

  bind_text(stmt, 1, thread.thread_id);
  bind_text(stmt, 2, thread.project_id);
  bind_text_optional(stmt, 3, thread.card_id);
  bind_text(stmt, 4, thread.title);
  bind_int64(stmt, 5, thread.created_at);
  bind_int64(stmt, 6, thread.updated_at);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "insert ai thread failed");
  }
}

std::optional<holder::model::AiThread> AiThreadRepo::get(const std::string& thread_id) const {
  static constexpr const char* SQL =
      "SELECT thread_id, project_id, card_id, title, created_at, updated_at "
      "FROM ai_threads WHERE thread_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare get ai thread failed");
  }

  bind_text(stmt, 1, thread_id);

  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    auto thread = read_thread(stmt);
    sqlite3_finalize(stmt);
    return thread;
  }

  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "get ai thread failed");
  }
  return std::nullopt;
}

std::vector<holder::model::AiThread> AiThreadRepo::list(const std::string& project_id) const {
  static constexpr const char* SQL =
      "SELECT thread_id, project_id, card_id, title, created_at, updated_at "
      "FROM ai_threads WHERE project_id = ? ORDER BY updated_at DESC;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare list ai threads failed");
  }

  bind_text(stmt, 1, project_id);

  std::vector<holder::model::AiThread> out;
  while (true) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      out.push_back(read_thread(stmt));
      continue;
    }
    if (rc == SQLITE_DONE) break;
    sqlite3_finalize(stmt);
    throw_sqlite(db_.handle(), "list ai threads failed");
  }

  sqlite3_finalize(stmt);
  return out;
} // LCOV_EXCL_LINE

void AiThreadRepo::update_title(
    const std::string& thread_id,
    const std::string& title,
    long long updated_at
) {
  static constexpr const char* SQL =
      "UPDATE ai_threads SET title = ?, updated_at = ? WHERE thread_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare update ai thread title failed");
  }

  bind_text(stmt, 1, title);
  bind_int64(stmt, 2, updated_at);
  bind_text(stmt, 3, thread_id);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "update ai thread title failed");
  }
}

void AiThreadRepo::update_card_id(
    const std::string& thread_id,
    const std::optional<std::string>& card_id
) {
  static constexpr const char* SQL = "UPDATE ai_threads SET card_id = ? WHERE thread_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare update ai thread card_id failed");
  }

  bind_text_optional(stmt, 1, card_id);
  bind_text(stmt, 2, thread_id);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "update ai thread card_id failed");
  }
}

void AiThreadRepo::touch_updated(const std::string& thread_id, long long updated_at) {
  static constexpr const char* SQL = "UPDATE ai_threads SET updated_at = ? WHERE thread_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare touch ai thread failed");
  }

  bind_int64(stmt, 1, updated_at);
  bind_text(stmt, 2, thread_id);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "touch ai thread failed");
  }
}

void AiThreadRepo::remove(const std::string& thread_id) {
  static constexpr const char* SQL = "DELETE FROM ai_threads WHERE thread_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare delete ai thread failed");
  }

  bind_text(stmt, 1, thread_id);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "delete ai thread failed");
  }
}

} // namespace holder::ai
