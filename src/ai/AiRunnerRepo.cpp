#include "ai/AiRunnerRepo.h"

#include <sqlite3.h>

#include <stdexcept>

namespace holder::ai {
namespace {

void throw_sqlite(sqlite3* db, const std::string& what) { // LCOV_EXCL_LINE
  const char* msg = db ? sqlite3_errmsg(db) : "unknown sqlite error"; // LCOV_EXCL_LINE
  throw std::runtime_error(what + ": " + msg); // LCOV_EXCL_LINE
} // LCOV_EXCL_LINE

void bind_text(sqlite3_stmt* stmt, int idx, const std::string& value) {
  if (sqlite3_bind_text(stmt, idx, value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
    throw std::runtime_error("sqlite bind_text failed"); // LCOV_EXCL_LINE
  }
}

void bind_optional_text(sqlite3_stmt* stmt, int idx, const std::optional<std::string>& value) {
  if (!value.has_value() || value->empty()) {
    if (sqlite3_bind_null(stmt, idx) != SQLITE_OK) {
      throw std::runtime_error("sqlite bind_null failed"); // LCOV_EXCL_LINE
    }
    return;
  }
  bind_text(stmt, idx, value.value());
}

std::optional<std::string> column_optional_text(sqlite3_stmt* stmt, int idx) {
  if (sqlite3_column_type(stmt, idx) == SQLITE_NULL) {
    return std::nullopt;
  }
  return std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, idx)));
}

holder::model::AiRunner read_row(sqlite3_stmt* stmt) {
  holder::model::AiRunner out;
  out.runner_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  out.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  out.kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
  out.base_url = column_optional_text(stmt, 3);
  out.source = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
  out.enabled = sqlite3_column_int(stmt, 5) != 0;
  out.created_at = sqlite3_column_int64(stmt, 6);
  out.updated_at = sqlite3_column_int64(stmt, 7);
  return out;
} // LCOV_EXCL_LINE

} // namespace

AiRunnerRepo::AiRunnerRepo(holder::platform::Db& db)
    : db_(db) {}

std::vector<holder::model::AiRunner> AiRunnerRepo::list() const {
  static constexpr const char* SQL =
      "SELECT runner_id, name, kind, base_url, source, enabled, created_at, updated_at "
      "FROM ai_runners ORDER BY created_at ASC, runner_id ASC;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare list runners failed"); // LCOV_EXCL_LINE
  }

  std::vector<holder::model::AiRunner> rows;
  for (;;) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      rows.push_back(read_row(stmt));
      continue;
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
      throw_sqlite(db_.handle(), "list runners failed"); // LCOV_EXCL_LINE
    }
    break;
  }
  return rows;
} // LCOV_EXCL_LINE

std::optional<holder::model::AiRunner> AiRunnerRepo::get(const std::string& runner_id) const {
  static constexpr const char* SQL =
      "SELECT runner_id, name, kind, base_url, source, enabled, created_at, updated_at "
      "FROM ai_runners WHERE runner_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare get runner failed"); // LCOV_EXCL_LINE
  }
  bind_text(stmt, 1, runner_id);

  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    auto out = read_row(stmt);
    sqlite3_finalize(stmt);
    return out;
  }
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "get runner failed"); // LCOV_EXCL_LINE
  }
  return std::nullopt;
}

void AiRunnerRepo::upsert(const holder::model::AiRunner& runner) {
  static constexpr const char* SQL =
      "INSERT INTO ai_runners("
      "runner_id, name, kind, base_url, source, enabled, created_at, updated_at"
      ") VALUES(?, ?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(runner_id) DO UPDATE SET "
      "name = excluded.name, "
      "kind = excluded.kind, "
      "base_url = excluded.base_url, "
      "source = excluded.source, "
      "enabled = excluded.enabled, "
      "updated_at = excluded.updated_at;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare upsert runner failed"); // LCOV_EXCL_LINE
  }

  bind_text(stmt, 1, runner.runner_id);
  bind_text(stmt, 2, runner.name);
  bind_text(stmt, 3, runner.kind);
  bind_optional_text(stmt, 4, runner.base_url);
  bind_text(stmt, 5, runner.source);
  sqlite3_bind_int(stmt, 6, runner.enabled ? 1 : 0);
  sqlite3_bind_int64(stmt, 7, runner.created_at);
  sqlite3_bind_int64(stmt, 8, runner.updated_at);

  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt); // LCOV_EXCL_LINE
    throw_sqlite(db_.handle(), "upsert runner failed"); // LCOV_EXCL_LINE
  }
  sqlite3_finalize(stmt);
}

void AiRunnerRepo::remove(const std::string& runner_id) {
  static constexpr const char* SQL = "DELETE FROM ai_runners WHERE runner_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare delete runner failed"); // LCOV_EXCL_LINE
  }
  bind_text(stmt, 1, runner_id);

  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt); // LCOV_EXCL_LINE
    throw_sqlite(db_.handle(), "delete runner failed"); // LCOV_EXCL_LINE
  }
  sqlite3_finalize(stmt);
}

} // namespace holder::ai
