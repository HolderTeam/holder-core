#include "card/TagRepo.h"

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

void bind_int64(sqlite3_stmt* stmt, int idx, long long value) {
  if (sqlite3_bind_int64(stmt, idx, static_cast<sqlite3_int64>(value)) != SQLITE_OK) {
    throw std::runtime_error("sqlite bind_int64 failed"); // LCOV_EXCL_LINE
  }
}

} // namespace

TagRepo::TagRepo(holder::platform::Db& db)
    : db_(db) {}

void TagRepo::set_tags_for_card(
    const std::string& project_id,
    const std::string& card_id,
    const std::vector<std::string>& tags,
    long long now
) {
  static constexpr const char* DELETE_SQL =
      "DELETE FROM card_tags WHERE project_id = ? AND card_id = ?;";
  sqlite3_stmt* delete_stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), DELETE_SQL, -1, &delete_stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare delete tags failed");
  }
  bind_text(delete_stmt, 1, project_id);
  bind_text(delete_stmt, 2, card_id);
  const int delete_rc = sqlite3_step(delete_stmt);
  sqlite3_finalize(delete_stmt);
  if (delete_rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "delete tags failed"); // LCOV_EXCL_LINE
  }

  if (!tags.empty()) {
    static constexpr const char* INSERT_SQL =
        "INSERT INTO card_tags(project_id, card_id, tag, created_at) VALUES(?, ?, ?, ?);";
    sqlite3_stmt* insert_stmt = nullptr;
    if (sqlite3_prepare_v2(db_.handle(), INSERT_SQL, -1, &insert_stmt, nullptr) != SQLITE_OK) {
      throw_sqlite(db_.handle(), "prepare insert tag failed"); // LCOV_EXCL_LINE
    }
    for (const auto& tag : tags) {
      sqlite3_reset(insert_stmt);
      sqlite3_clear_bindings(insert_stmt);
      bind_text(insert_stmt, 1, project_id);
      bind_text(insert_stmt, 2, card_id);
      bind_text(insert_stmt, 3, tag);
      bind_int64(insert_stmt, 4, now);
      const int rc = sqlite3_step(insert_stmt);
      if (rc != SQLITE_DONE) {
        sqlite3_finalize(insert_stmt);
        throw_sqlite(db_.handle(), "insert tag failed");
      }
    }
    sqlite3_finalize(insert_stmt);
  }
}

void TagRepo::delete_tags_for_card(const std::string& project_id, const std::string& card_id) {
  static constexpr const char* SQL = "DELETE FROM card_tags WHERE project_id = ? AND card_id = ?;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare delete tags failed");
  }
  bind_text(stmt, 1, project_id);
  bind_text(stmt, 2, card_id);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "delete tags failed"); // LCOV_EXCL_LINE
  }
}

std::vector<std::string> TagRepo::list_tags_for_card(
    const std::string& project_id,
    const std::string& card_id
) const {
  static constexpr const char* SQL =
      "SELECT tag FROM card_tags WHERE project_id = ? AND card_id = ? ORDER BY tag ASC;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare list tags for card failed");
  }
  bind_text(stmt, 1, project_id);
  bind_text(stmt, 2, card_id);

  std::vector<std::string> out;
  while (true) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      out.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
      continue;
    }
    if (rc == SQLITE_DONE) break;
    sqlite3_finalize(stmt); // LCOV_EXCL_LINE
    throw_sqlite(db_.handle(), "list tags for card failed"); // LCOV_EXCL_LINE
  }
  sqlite3_finalize(stmt);
  return out;
} // LCOV_EXCL_LINE

std::vector<std::string> TagRepo::list_card_ids_with_tag(
    const std::string& project_id,
    const std::string& tag
) const {
  static constexpr const char* SQL =
      "SELECT card_id FROM card_tags WHERE project_id = ? AND tag = ? ORDER BY created_at DESC;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare list cards with tag failed");
  }
  bind_text(stmt, 1, project_id);
  bind_text(stmt, 2, tag);

  std::vector<std::string> out;
  while (true) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      out.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
      continue;
    }
    if (rc == SQLITE_DONE) break;
    sqlite3_finalize(stmt); // LCOV_EXCL_LINE
    throw_sqlite(db_.handle(), "list cards with tag failed"); // LCOV_EXCL_LINE
  }
  sqlite3_finalize(stmt);
  return out;
} // LCOV_EXCL_LINE

std::vector<std::pair<std::string, int>> TagRepo::list_project_tags(
    const std::string& project_id
) const {
  static constexpr const char* SQL =
      "SELECT tag, COUNT(*) AS c FROM card_tags WHERE project_id = ? "
      "GROUP BY tag ORDER BY c DESC, tag ASC;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare list project tags failed");
  }
  bind_text(stmt, 1, project_id);

  std::vector<std::pair<std::string, int>> out;
  while (true) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      out.emplace_back(
          reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)),
          sqlite3_column_int(stmt, 1)
      );
      continue;
    }
    if (rc == SQLITE_DONE) break;
    sqlite3_finalize(stmt); // LCOV_EXCL_LINE
    throw_sqlite(db_.handle(), "list project tags failed"); // LCOV_EXCL_LINE
  }
  sqlite3_finalize(stmt);
  return out;
} // LCOV_EXCL_LINE

} // namespace holder::card
