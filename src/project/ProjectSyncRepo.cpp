#include "project/ProjectSyncRepo.h"

#include <sqlite3.h>

#include <array>
#include <set>
#include <stdexcept>

namespace holder::project {
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
    return;
  }
  if (sqlite3_bind_null(stmt, idx) != SQLITE_OK) {
    throw std::runtime_error("sqlite bind_null failed"); // LCOV_EXCL_LINE
  }
}

void bind_int64_optional(sqlite3_stmt* stmt, int idx, const std::optional<long long>& value) {
  if (value.has_value()) {
    if (sqlite3_bind_int64(stmt, idx, static_cast<sqlite3_int64>(value.value())) != SQLITE_OK) {
      throw std::runtime_error("sqlite bind_int64 failed"); // LCOV_EXCL_LINE
    }
    return;
  }
  if (sqlite3_bind_null(stmt, idx) != SQLITE_OK) {
    throw std::runtime_error("sqlite bind_null failed"); // LCOV_EXCL_LINE
  }
}

void bind_int64(sqlite3_stmt* stmt, int idx, long long value) {
  if (sqlite3_bind_int64(stmt, idx, static_cast<sqlite3_int64>(value)) != SQLITE_OK) {
    throw std::runtime_error("sqlite bind_int64 failed"); // LCOV_EXCL_LINE
  }
}

void bind_int(sqlite3_stmt* stmt, int idx, int value) {
  if (sqlite3_bind_int(stmt, idx, value) != SQLITE_OK) {
    throw std::runtime_error("sqlite bind_int failed"); // LCOV_EXCL_LINE
  }
}

std::optional<long long> read_int64_optional(sqlite3_stmt* stmt, int idx) {
  if (sqlite3_column_type(stmt, idx) == SQLITE_NULL) {
    return std::nullopt;
  }
  return sqlite3_column_int64(stmt, idx);
}

std::optional<std::string> read_text_optional(sqlite3_stmt* stmt, int idx) {
  if (sqlite3_column_type(stmt, idx) == SQLITE_NULL) {
    return std::nullopt;
  }
  return reinterpret_cast<const char*>(sqlite3_column_text(stmt, idx));
}

int backoff_seconds_for_retry(int retry_count) {
  static constexpr std::array<int, 4> kSchedule = {60, 300, 900, 1800};
  if (retry_count <= 0) return 0;
  const std::size_t idx = static_cast<std::size_t>(retry_count - 1);
  if (idx < kSchedule.size()) return kSchedule[idx];
  return kSchedule.back();
}

std::set<std::string> table_columns(sqlite3* db, const std::string& table_name) {
  const std::string sql = "PRAGMA table_info(" + table_name + ");";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db, "prepare table_info failed"); // LCOV_EXCL_LINE
  }

  std::set<std::string> columns;
  while (true) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      const unsigned char* name = sqlite3_column_text(stmt, 1);
      if (name != nullptr) {
        columns.insert(reinterpret_cast<const char*>(name));
      }
      continue;
    }
    if (rc == SQLITE_DONE) {
      break;
    }
    sqlite3_finalize(stmt); // LCOV_EXCL_LINE
    throw_sqlite(db, "table_info step failed"); // LCOV_EXCL_LINE
  }

  sqlite3_finalize(stmt);
  return columns;
}

} // namespace

ProjectSyncRepo::ProjectSyncRepo(holder::platform::Db& db)
    : db_(db) {
  ensure_table();
}

void ProjectSyncRepo::ensure_table() {
  db_.exec("CREATE TABLE IF NOT EXISTS project_sync_state ("
           " project_id TEXT PRIMARY KEY REFERENCES projects(project_id) ON DELETE CASCADE,"
           " last_commit_at INTEGER NULL,"
           " last_push_at INTEGER NULL,"
           " last_pull_at INTEGER NULL,"
           " uncommitted_changes_count INTEGER NOT NULL DEFAULT 0,"
           " unpushed_commits_count INTEGER NOT NULL DEFAULT 0,"
           " last_push_status TEXT NULL,"
           " last_pull_status TEXT NULL,"
           " last_sync_error TEXT NULL,"
           " last_sync_error_at INTEGER NULL,"
           " retry_count INTEGER NOT NULL DEFAULT 0,"
           " next_retry_at INTEGER NULL,"
           " pull_retry_count INTEGER NOT NULL DEFAULT 0,"
           " next_pull_retry_at INTEGER NULL,"
           " updated_at INTEGER NOT NULL DEFAULT 0"
           ");");

  const auto cols = table_columns(db_.handle(), "project_sync_state");
  if (cols.find("pull_retry_count") == cols.end()) {
    db_.exec(
        "ALTER TABLE project_sync_state ADD COLUMN pull_retry_count INTEGER NOT NULL DEFAULT 0;"
    );
  }
  if (cols.find("next_pull_retry_at") == cols.end()) {
    db_.exec("ALTER TABLE project_sync_state ADD COLUMN next_pull_retry_at INTEGER NULL;");
  }
}

std::optional<holder::model::ProjectSyncState> ProjectSyncRepo::get(const std::string& project_id
) const {
  static constexpr const char* SQL =
      "SELECT project_id, last_commit_at, last_push_at, last_pull_at,"
      " uncommitted_changes_count, unpushed_commits_count,"
      " last_push_status, last_pull_status, last_sync_error, last_sync_error_at, retry_count,"
      " next_retry_at, pull_retry_count, next_pull_retry_at, updated_at"
      " FROM project_sync_state WHERE project_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare get project sync state failed");
  }

  bind_text(stmt, 1, project_id);
  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    holder::model::ProjectSyncState state;
    state.project_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    state.last_commit_at = read_int64_optional(stmt, 1);
    state.last_push_at = read_int64_optional(stmt, 2);
    state.last_pull_at = read_int64_optional(stmt, 3);
    state.uncommitted_changes_count = sqlite3_column_int(stmt, 4);
    state.unpushed_commits_count = sqlite3_column_int(stmt, 5);
    state.last_push_status = read_text_optional(stmt, 6);
    state.last_pull_status = read_text_optional(stmt, 7);
    state.last_sync_error = read_text_optional(stmt, 8);
    state.last_sync_error_at = read_int64_optional(stmt, 9);
    state.retry_count = sqlite3_column_int(stmt, 10);
    state.next_retry_at = read_int64_optional(stmt, 11);
    state.pull_retry_count = sqlite3_column_int(stmt, 12);
    state.next_pull_retry_at = read_int64_optional(stmt, 13);
    state.updated_at = sqlite3_column_int64(stmt, 14);
    sqlite3_finalize(stmt);
    return state;
  }
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "get project sync state failed");
  }
  return std::nullopt;
}

void ProjectSyncRepo::upsert(const holder::model::ProjectSyncState& state) {
  static constexpr const char* SQL =
      "INSERT INTO project_sync_state("
      " project_id, last_commit_at, last_push_at, last_pull_at,"
      " uncommitted_changes_count, unpushed_commits_count,"
      " last_push_status, last_pull_status, last_sync_error, last_sync_error_at, retry_count,"
      " next_retry_at, pull_retry_count, next_pull_retry_at, updated_at)"
      " VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
      " ON CONFLICT(project_id) DO UPDATE SET"
      " last_commit_at=excluded.last_commit_at,"
      " last_push_at=excluded.last_push_at,"
      " last_pull_at=excluded.last_pull_at,"
      " uncommitted_changes_count=excluded.uncommitted_changes_count,"
      " unpushed_commits_count=excluded.unpushed_commits_count,"
      " last_push_status=excluded.last_push_status,"
      " last_pull_status=excluded.last_pull_status,"
      " last_sync_error=excluded.last_sync_error,"
      " last_sync_error_at=excluded.last_sync_error_at,"
      " retry_count=excluded.retry_count,"
      " next_retry_at=excluded.next_retry_at,"
      " pull_retry_count=excluded.pull_retry_count,"
      " next_pull_retry_at=excluded.next_pull_retry_at,"
      " updated_at=excluded.updated_at;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare upsert project sync state failed");
  }

  bind_text(stmt, 1, state.project_id);
  bind_int64_optional(stmt, 2, state.last_commit_at);
  bind_int64_optional(stmt, 3, state.last_push_at);
  bind_int64_optional(stmt, 4, state.last_pull_at);
  bind_int(stmt, 5, state.uncommitted_changes_count);
  bind_int(stmt, 6, state.unpushed_commits_count);
  bind_text_optional(stmt, 7, state.last_push_status);
  bind_text_optional(stmt, 8, state.last_pull_status);
  bind_text_optional(stmt, 9, state.last_sync_error);
  bind_int64_optional(stmt, 10, state.last_sync_error_at);
  bind_int(stmt, 11, state.retry_count);
  bind_int64_optional(stmt, 12, state.next_retry_at);
  bind_int(stmt, 13, state.pull_retry_count);
  bind_int64_optional(stmt, 14, state.next_pull_retry_at);
  bind_int64(stmt, 15, state.updated_at);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "upsert project sync state failed");
  }
}

void ProjectSyncRepo::update_activity_counts(
    const std::string& project_id,
    const ProjectSyncActivityUpdate& update
) {
  holder::model::ProjectSyncState state =
      get(project_id).value_or(holder::model::ProjectSyncState{});
  state.project_id = project_id;
  state.uncommitted_changes_count = update.uncommitted_changes_count;
  state.unpushed_commits_count = update.unpushed_commits_count;
  state.updated_at = update.updated_at;
  upsert(state);
}

void ProjectSyncRepo::record_push_result(
    const std::string& project_id,
    const std::string& status,
    bool success,
    const std::optional<std::string>& error_message,
    long long now
) {
  holder::model::ProjectSyncState state =
      get(project_id).value_or(holder::model::ProjectSyncState{});
  state.project_id = project_id;
  state.last_push_status = status;
  state.updated_at = now;
  if (success) {
    state.last_push_at = now;
    state.last_sync_error.reset();
    state.last_sync_error_at.reset();
    state.retry_count = 0;
    state.next_retry_at.reset();
  } else {
    state.last_sync_error = error_message.has_value() && !error_message->empty()
                                ? error_message
                                : std::optional<std::string>{"push failed"};
    state.last_sync_error_at = now;
    state.retry_count += 1;
    state.next_retry_at = now + backoff_seconds_for_retry(state.retry_count);
  }
  upsert(state);
}

void ProjectSyncRepo::record_pull_result(
    const std::string& project_id,
    const std::string& status,
    bool success,
    const std::optional<std::string>& error_message,
    long long now
) {
  holder::model::ProjectSyncState state =
      get(project_id).value_or(holder::model::ProjectSyncState{});
  state.project_id = project_id;
  state.last_pull_status = status;
  state.updated_at = now;
  if (success) {
    state.last_pull_at = now;
    state.last_sync_error.reset();
    state.last_sync_error_at.reset();
    state.pull_retry_count = 0;
    state.next_pull_retry_at.reset();
  } else {
    state.last_sync_error = error_message.has_value() && !error_message->empty()
                                ? error_message
                                : std::optional<std::string>{"pull failed"};
    state.last_sync_error_at = now;
    state.pull_retry_count += 1;
    state.next_pull_retry_at = now + backoff_seconds_for_retry(state.pull_retry_count);
  }
  upsert(state);
}

void ProjectSyncRepo::remove(const std::string& project_id) {
  static constexpr const char* SQL = "DELETE FROM project_sync_state WHERE project_id = ?;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare delete project sync state failed");
  }
  bind_text(stmt, 1, project_id);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "delete project sync state failed");
  }
}

} // namespace holder::project
