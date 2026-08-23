#include "card/MilestoneRepo.h"

#include <sqlite3.h>

#include <stdexcept>

namespace holder::card {
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

void bind_int64_optional(sqlite3_stmt* stmt, int idx, const std::optional<long long>& value) {
  if (value.has_value()) {
    bind_int64(stmt, idx, value.value());
  } else if (sqlite3_bind_null(stmt, idx) != SQLITE_OK) {
    throw std::runtime_error("sqlite bind_null failed"); // LCOV_EXCL_LINE
  }
}

void bind_bool(sqlite3_stmt* stmt, int idx, bool value) {
  if (sqlite3_bind_int(stmt, idx, value ? 1 : 0) != SQLITE_OK) {
    throw std::runtime_error("sqlite bind_int failed"); // LCOV_EXCL_LINE
  }
}

holder::model::Milestone read_milestone(sqlite3_stmt* stmt) {
  holder::model::Milestone milestone;
  milestone.milestone_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  milestone.project_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  milestone.card_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
  milestone.start_at = sqlite3_column_int64(stmt, 3);
  if (sqlite3_column_type(stmt, 4) == SQLITE_NULL) {
    milestone.end_at.reset();
  } else {
    milestone.end_at = sqlite3_column_int64(stmt, 4);
  }
  milestone.all_day = sqlite3_column_int(stmt, 5) != 0;
  if (sqlite3_column_type(stmt, 6) == SQLITE_NULL) {
    milestone.kind.reset();
  } else {
    milestone.kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
  }
  if (sqlite3_column_type(stmt, 7) == SQLITE_NULL) {
    milestone.description.reset();
  } else {
    milestone.description = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
  }
  milestone.created_at = sqlite3_column_int64(stmt, 8);
  milestone.updated_at = sqlite3_column_int64(stmt, 9);
  return milestone;
} // LCOV_EXCL_LINE

} // namespace

MilestoneRepo::MilestoneRepo(holder::platform::Db& db)
    : db_(db) {}

void MilestoneRepo::replace_for_card(
    const std::string& project_id,
    const std::string& card_id,
    const std::vector<holder::model::Milestone>& milestones
) {
  static constexpr const char* DELETE_SQL =
      "DELETE FROM milestones WHERE project_id = ? AND card_id = ?;";
  sqlite3_stmt* delete_stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), DELETE_SQL, -1, &delete_stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare delete milestones failed");
  }
  bind_text(delete_stmt, 1, project_id);
  bind_text(delete_stmt, 2, card_id);
  const int delete_rc = sqlite3_step(delete_stmt);
  sqlite3_finalize(delete_stmt);
  if (delete_rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "delete milestones failed"); // LCOV_EXCL_LINE
  }

  if (milestones.empty()) {
    return;
  }

  static constexpr const char* INSERT_SQL =
      "INSERT INTO milestones(milestone_id, project_id, card_id, start_at, end_at, all_day, "
      "kind, description, created_at, updated_at) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
  sqlite3_stmt* insert_stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), INSERT_SQL, -1, &insert_stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare insert milestone failed"); // LCOV_EXCL_LINE
  }

  for (const auto& milestone : milestones) {
    if (milestone.project_id != project_id || milestone.card_id != card_id) {
      sqlite3_finalize(insert_stmt);
      throw std::runtime_error("milestone project_id/card_id mismatch");
    }

    sqlite3_reset(insert_stmt);
    sqlite3_clear_bindings(insert_stmt);

    bind_text(insert_stmt, 1, milestone.milestone_id);
    bind_text(insert_stmt, 2, milestone.project_id);
    bind_text(insert_stmt, 3, milestone.card_id);
    bind_int64(insert_stmt, 4, milestone.start_at);
    bind_int64_optional(insert_stmt, 5, milestone.end_at);
    bind_bool(insert_stmt, 6, milestone.all_day);
    bind_text_optional(insert_stmt, 7, milestone.kind);
    bind_text_optional(insert_stmt, 8, milestone.description);
    bind_int64(insert_stmt, 9, milestone.created_at);
    bind_int64(insert_stmt, 10, milestone.updated_at);

    const int rc = sqlite3_step(insert_stmt);
    if (rc != SQLITE_DONE) {
      sqlite3_finalize(insert_stmt);
      throw_sqlite(db_.handle(), "insert milestone failed");
    }
  }

  sqlite3_finalize(insert_stmt);
}

void MilestoneRepo::delete_for_card(const std::string& project_id, const std::string& card_id) {
  static constexpr const char* SQL =
      "DELETE FROM milestones WHERE project_id = ? AND card_id = ?;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare delete milestones failed");
  }
  bind_text(stmt, 1, project_id);
  bind_text(stmt, 2, card_id);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "delete milestones failed"); // LCOV_EXCL_LINE
  }
}

std::vector<holder::model::Milestone> MilestoneRepo::list_for_card(
    const std::string& project_id,
    const std::string& card_id
) const {
  static constexpr const char* SQL =
      "SELECT milestone_id, project_id, card_id, start_at, end_at, all_day, kind, description, "
      "created_at, updated_at FROM milestones WHERE project_id = ? AND card_id = ? "
      "ORDER BY start_at ASC;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare list milestones for card failed");
  }
  bind_text(stmt, 1, project_id);
  bind_text(stmt, 2, card_id);

  std::vector<holder::model::Milestone> out;
  while (true) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      out.push_back(read_milestone(stmt));
      continue;
    }
    if (rc == SQLITE_DONE) break;
    sqlite3_finalize(stmt); // LCOV_EXCL_LINE
    throw_sqlite(db_.handle(), "list milestones for card failed"); // LCOV_EXCL_LINE
  }
  sqlite3_finalize(stmt);
  return out;
} // LCOV_EXCL_LINE

std::vector<holder::model::Milestone> MilestoneRepo::list_in_range(
    const std::string& project_id,
    long long from,
    long long to
) const {
  static constexpr const char* SQL =
      "SELECT m.milestone_id, m.project_id, m.card_id, m.start_at, m.end_at, m.all_day, "
      "m.kind, m.description, m.created_at, m.updated_at "
      "FROM milestones m "
      "JOIN cards c ON c.card_id = m.card_id "
      "WHERE m.project_id = ? AND c.deleted_at IS NULL AND m.start_at >= ? AND m.start_at <= ? "
      "ORDER BY m.start_at ASC;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare list milestones in range failed");
  }
  bind_text(stmt, 1, project_id);
  bind_int64(stmt, 2, from);
  bind_int64(stmt, 3, to);

  std::vector<holder::model::Milestone> out;
  while (true) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      out.push_back(read_milestone(stmt));
      continue;
    }
    if (rc == SQLITE_DONE) break;
    sqlite3_finalize(stmt); // LCOV_EXCL_LINE
    throw_sqlite(db_.handle(), "list milestones in range failed"); // LCOV_EXCL_LINE
  }
  sqlite3_finalize(stmt);
  return out;
} // LCOV_EXCL_LINE

} // namespace holder::card
