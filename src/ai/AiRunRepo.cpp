#include "ai/AiRunRepo.h"

#include "platform/Db.h"

#include <sqlite3.h>
#include <stdexcept>

namespace holder::ai {
namespace {

std::string column_text(sqlite3_stmt* stmt, int index) {
  const unsigned char* text = sqlite3_column_text(stmt, index);
  if (!text) return {};
  return reinterpret_cast<const char*>(text);
}

std::optional<std::string> column_nullable(sqlite3_stmt* stmt, int index) {
  if (sqlite3_column_type(stmt, index) == SQLITE_NULL) return std::nullopt;
  return column_text(stmt, index);
}

model::AiRun row_to_run(sqlite3_stmt* stmt) {
  model::AiRun run;
  run.run_id = column_text(stmt, 0);
  run.project_id = column_nullable(stmt, 1);
  run.thread_id = column_nullable(stmt, 2);
  run.message_id = column_nullable(stmt, 3);
  run.mode = column_text(stmt, 4);
  run.prompt = column_text(stmt, 5);
  run.context_json = column_nullable(stmt, 6);
  run.router_model = column_nullable(stmt, 7);
  run.ranked_json = column_nullable(stmt, 8);
  run.policy_trace_json = column_nullable(stmt, 9);
  run.chosen_model = column_nullable(stmt, 10);
  run.status = column_text(stmt, 11);
  run.error = column_nullable(stmt, 12);
  run.created_at = sqlite3_column_int64(stmt, 13);
  run.updated_at = sqlite3_column_int64(stmt, 14);
  return run;
} // LCOV_EXCL_LINE

void throw_sqlite(sqlite3* db, const std::string& msg) {
  throw std::runtime_error(msg + ": " + sqlite3_errmsg(db));
}

} // namespace

AiRunRepo::AiRunRepo(holder::platform::Db& db)
    : db_(db) {}

void AiRunRepo::create(const model::AiRun& run) {
  static constexpr const char* SQL =
      "INSERT INTO ai_runs("
      "run_id, project_id, thread_id, message_id, mode, prompt, context_json, router_model, "
      "ranked_json, policy_trace_json, chosen_model, status, error, created_at, updated_at) "
      "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare ai_runs insert failed");
  }

  sqlite3_bind_text(stmt, 1, run.run_id.c_str(), -1, SQLITE_TRANSIENT);
  if (run.project_id.has_value()) {
    sqlite3_bind_text(stmt, 2, run.project_id->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 2);
  }
  if (run.thread_id.has_value()) {
    sqlite3_bind_text(stmt, 3, run.thread_id->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 3);
  }
  if (run.message_id.has_value()) {
    sqlite3_bind_text(stmt, 4, run.message_id->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 4);
  }
  sqlite3_bind_text(stmt, 5, run.mode.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6, run.prompt.c_str(), -1, SQLITE_TRANSIENT);
  if (run.context_json.has_value()) {
    sqlite3_bind_text(stmt, 7, run.context_json->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 7);
  }
  if (run.router_model.has_value()) {
    sqlite3_bind_text(stmt, 8, run.router_model->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 8);
  }
  if (run.ranked_json.has_value()) {
    sqlite3_bind_text(stmt, 9, run.ranked_json->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 9);
  }
  if (run.policy_trace_json.has_value()) {
    sqlite3_bind_text(stmt, 10, run.policy_trace_json->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 10);
  }
  if (run.chosen_model.has_value()) {
    sqlite3_bind_text(stmt, 11, run.chosen_model->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 11);
  }
  sqlite3_bind_text(stmt, 12, run.status.c_str(), -1, SQLITE_TRANSIENT);
  if (run.error.has_value()) {
    sqlite3_bind_text(stmt, 13, run.error->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 13);
  }
  sqlite3_bind_int64(stmt, 14, run.created_at);
  sqlite3_bind_int64(stmt, 15, run.updated_at);

  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    throw_sqlite(db_.handle(), "insert ai run failed");
  }
  sqlite3_finalize(stmt);
}

std::optional<model::AiRun> AiRunRepo::get(const std::string& run_id) const {
  static constexpr const char* SQL =
      "SELECT run_id, project_id, thread_id, message_id, mode, prompt, context_json, router_model, "
      "ranked_json, policy_trace_json, chosen_model, status, error, created_at, updated_at "
      "FROM ai_runs WHERE run_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare ai_runs get failed");
  }
  sqlite3_bind_text(stmt, 1, run_id.c_str(), -1, SQLITE_TRANSIENT);

  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    auto run = row_to_run(stmt);
    sqlite3_finalize(stmt);
    return run;
  }
  sqlite3_finalize(stmt);
  if (rc == SQLITE_DONE) {
    return std::nullopt;
  }
  throw_sqlite(db_.handle(), "get ai run failed");
  return std::nullopt; // LCOV_EXCL_LINE
}

std::vector<model::AiRun> AiRunRepo::list_by_thread(const std::string& thread_id) const {
  static constexpr const char* SQL =
      "SELECT run_id, project_id, thread_id, message_id, mode, prompt, context_json, router_model, "
      "ranked_json, policy_trace_json, chosen_model, status, error, created_at, updated_at "
      "FROM ai_runs WHERE thread_id = ? ORDER BY created_at DESC;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare ai_runs list_by_thread failed");
  }
  sqlite3_bind_text(stmt, 1, thread_id.c_str(), -1, SQLITE_TRANSIENT);

  std::vector<model::AiRun> runs;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    runs.push_back(row_to_run(stmt));
  }
  sqlite3_finalize(stmt);
  return runs;
} // LCOV_EXCL_LINE

std::vector<model::AiRun> AiRunRepo::list_by_project(const std::string& project_id) const {
  static constexpr const char* SQL =
      "SELECT run_id, project_id, thread_id, message_id, mode, prompt, context_json, router_model, "
      "ranked_json, policy_trace_json, chosen_model, status, error, created_at, updated_at "
      "FROM ai_runs WHERE project_id = ? ORDER BY created_at DESC;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare ai_runs list_by_project failed");
  }
  sqlite3_bind_text(stmt, 1, project_id.c_str(), -1, SQLITE_TRANSIENT);

  std::vector<model::AiRun> runs;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    runs.push_back(row_to_run(stmt));
  }
  sqlite3_finalize(stmt);
  return runs;
} // LCOV_EXCL_LINE

void AiRunRepo::update_status(
    const std::string& run_id,
    const std::string& status,
    const std::optional<std::string>& error,
    const std::optional<std::string>& message_id,
    const std::optional<std::string>& chosen_model,
    const std::optional<std::string>& ranked_json,
    const std::optional<std::string>& policy_trace_json,
    long long updated_at
) {
  static constexpr const char* SQL =
      "UPDATE ai_runs SET status = ?, error = ?, message_id = ?, chosen_model = ?, ranked_json = ?, "
      "policy_trace_json = ?, updated_at = ? WHERE run_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare ai_runs update failed");
  }

  sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_TRANSIENT);
  if (error.has_value()) {
    sqlite3_bind_text(stmt, 2, error->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 2);
  }
  if (message_id.has_value()) {
    sqlite3_bind_text(stmt, 3, message_id->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 3);
  }
  if (chosen_model.has_value()) {
    sqlite3_bind_text(stmt, 4, chosen_model->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 4);
  }
  if (ranked_json.has_value()) {
    sqlite3_bind_text(stmt, 5, ranked_json->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 5);
  }
  if (policy_trace_json.has_value()) {
    sqlite3_bind_text(stmt, 6, policy_trace_json->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 6);
  }
  sqlite3_bind_int64(stmt, 7, updated_at);
  sqlite3_bind_text(stmt, 8, run_id.c_str(), -1, SQLITE_TRANSIENT);

  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    throw_sqlite(db_.handle(), "update ai run failed");
  }
  sqlite3_finalize(stmt);
}

} // namespace holder::ai
