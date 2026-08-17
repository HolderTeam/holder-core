#include "card/CardRepo.h"

#include <sqlite3.h>

#include <stdexcept>
#include <utility>

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

void bind_double(sqlite3_stmt* stmt, int idx, double value) {
  if (sqlite3_bind_double(stmt, idx, value) != SQLITE_OK) {
    throw std::runtime_error("sqlite bind_double failed"); // LCOV_EXCL_LINE
  }
}

holder::model::Card read_card(sqlite3_stmt* stmt) {
  holder::model::Card c;
  c.card_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  c.project_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  c.title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
  c.rel_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));

  if (sqlite3_column_type(stmt, 4) == SQLITE_NULL) {
    c.parent_card_id.reset();
  } else {
    c.parent_card_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
  }

  c.sort_key = sqlite3_column_double(stmt, 5);
  c.created_at = sqlite3_column_int64(stmt, 6);
  c.updated_at = sqlite3_column_int64(stmt, 7);

  if (sqlite3_column_type(stmt, 8) == SQLITE_NULL) {
    c.deleted_at.reset();
  } else {
    c.deleted_at = sqlite3_column_int64(stmt, 8);
  }

  return c;
} // LCOV_EXCL_LINE

} // namespace

CardRepo::CardRepo(holder::platform::Db& db)
    : db_(db) {}

void CardRepo::create(const holder::model::Card& card) {
  static constexpr const char* SQL =
      "INSERT INTO cards(card_id, project_id, title, rel_path, parent_card_id, "
      "sort_key, created_at, updated_at, deleted_at) "
      "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?);";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare insert card failed");
  }

  bind_text(stmt, 1, card.card_id);
  bind_text(stmt, 2, card.project_id);
  bind_text(stmt, 3, card.title);
  bind_text(stmt, 4, card.rel_path);
  bind_text_optional(stmt, 5, card.parent_card_id);
  bind_double(stmt, 6, card.sort_key);
  bind_int64(stmt, 7, card.created_at);
  bind_int64(stmt, 8, card.updated_at);
  bind_int64_optional(stmt, 9, card.deleted_at);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt); // LCOV_EXCL_LINE
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "insert card failed");
  }
}

std::optional<holder::model::Card> CardRepo::get(const std::string& card_id) const {
  static constexpr const char* SQL =
      "SELECT card_id, project_id, title, rel_path, parent_card_id, sort_key, "
      "created_at, updated_at, deleted_at "
      "FROM cards WHERE card_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare get card failed");
  }

  bind_text(stmt, 1, card_id);

  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    auto card = read_card(stmt);
    sqlite3_finalize(stmt);
    return card;
  }

  sqlite3_finalize(stmt); // LCOV_EXCL_LINE
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "get card failed");
  }
  return std::nullopt;
}

std::vector<holder::model::Card> CardRepo::list_roots(const std::string& project_id) const {
  static constexpr const char* SQL =
      "SELECT card_id, project_id, title, rel_path, parent_card_id, sort_key, "
      "created_at, updated_at, deleted_at "
      "FROM cards WHERE project_id = ? AND parent_card_id IS NULL "
      "ORDER BY sort_key ASC, updated_at DESC;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare list cards failed");
  }
  bind_text(stmt, 1, project_id);

  std::vector<holder::model::Card> out;
  while (true) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      out.push_back(read_card(stmt));
      continue;
    }
    if (rc == SQLITE_DONE) break;
    sqlite3_finalize(stmt); // LCOV_EXCL_LINE
    throw_sqlite(db_.handle(), "list cards failed"); // LCOV_EXCL_LINE
  }

  sqlite3_finalize(stmt); // LCOV_EXCL_LINE
  return out;
} // LCOV_EXCL_LINE

std::vector<holder::model::Card> CardRepo::list_children(
    const std::string& project_id,
    const std::string& parent_card_id
) const {
  static constexpr const char* SQL =
      "SELECT card_id, project_id, title, rel_path, parent_card_id, sort_key, "
      "created_at, updated_at, deleted_at "
      "FROM cards WHERE project_id = ? AND parent_card_id = ? "
      "ORDER BY sort_key ASC, updated_at DESC;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare list child cards failed");
  }
  bind_text(stmt, 1, project_id);
  bind_text(stmt, 2, parent_card_id);

  std::vector<holder::model::Card> out;
  while (true) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      out.push_back(read_card(stmt));
      continue;
    }
    if (rc == SQLITE_DONE) break;
    sqlite3_finalize(stmt); // LCOV_EXCL_LINE
    throw_sqlite(db_.handle(), "list child cards failed"); // LCOV_EXCL_LINE
  }
  sqlite3_finalize(stmt); // LCOV_EXCL_LINE
  return out;
} // LCOV_EXCL_LINE

std::vector<holder::model::Card> CardRepo::list_all(const std::string& project_id) const {
  static constexpr const char* SQL =
      "SELECT card_id, project_id, title, rel_path, parent_card_id, sort_key, "
      "created_at, updated_at, deleted_at "
      "FROM cards WHERE project_id = ? "
      "ORDER BY sort_key ASC, updated_at DESC;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare list all cards failed");
  }
  bind_text(stmt, 1, project_id);

  std::vector<holder::model::Card> out;
  while (true) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      out.push_back(read_card(stmt));
      continue;
    }
    if (rc == SQLITE_DONE) break;
    sqlite3_finalize(stmt); // LCOV_EXCL_LINE
    throw_sqlite(db_.handle(), "list all cards failed"); // LCOV_EXCL_LINE
  }
  sqlite3_finalize(stmt); // LCOV_EXCL_LINE
  return out;
} // LCOV_EXCL_LINE

int CardRepo::count_all_not_deleted(const std::string& project_id) const {
  static constexpr const char* SQL =
      "SELECT COUNT(*) FROM cards WHERE project_id = ? AND deleted_at IS NULL;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare count all cards failed");
  }
  bind_text(stmt, 1, project_id);
  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    const int count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
  }
  sqlite3_finalize(stmt); // LCOV_EXCL_LINE
  if (rc != SQLITE_DONE) { // LCOV_EXCL_LINE
    throw_sqlite(db_.handle(), "count all cards failed"); // LCOV_EXCL_LINE
  }
  return 0; // LCOV_EXCL_LINE
}

int CardRepo::count_roots_not_deleted(const std::string& project_id) const {
  static constexpr const char* SQL =
      "SELECT COUNT(*) FROM cards "
      "WHERE project_id = ? AND parent_card_id IS NULL AND deleted_at IS NULL;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare count root cards failed");
  }
  bind_text(stmt, 1, project_id);
  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    const int count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
  }
  sqlite3_finalize(stmt); // LCOV_EXCL_LINE
  if (rc != SQLITE_DONE) { // LCOV_EXCL_LINE
    throw_sqlite(db_.handle(), "count root cards failed"); // LCOV_EXCL_LINE
  }
  return 0; // LCOV_EXCL_LINE
}

int CardRepo::count_children_not_deleted(
    const std::string& project_id,
    const std::string& parent_card_id
) const {
  static constexpr const char* SQL =
      "SELECT COUNT(*) FROM cards "
      "WHERE project_id = ? AND parent_card_id = ? AND deleted_at IS NULL;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare count child cards failed");
  }
  bind_text(stmt, 1, project_id);
  bind_text(stmt, 2, parent_card_id);
  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    const int count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
  }
  sqlite3_finalize(stmt); // LCOV_EXCL_LINE
  if (rc != SQLITE_DONE) { // LCOV_EXCL_LINE
    throw_sqlite(db_.handle(), "count child cards failed"); // LCOV_EXCL_LINE
  }
  return 0; // LCOV_EXCL_LINE
}

double CardRepo::next_sort_key(
    const std::string& project_id,
    const std::optional<std::string>& parent_card_id
) const {
  static constexpr const char* SQL_ROOT =
      "SELECT COALESCE(MAX(sort_key), -1.0) + 1.0 "
      "FROM cards WHERE project_id = ? AND parent_card_id IS NULL AND deleted_at IS NULL;";
  static constexpr const char* SQL_CHILDREN =
      "SELECT COALESCE(MAX(sort_key), -1.0) + 1.0 "
      "FROM cards WHERE project_id = ? AND parent_card_id = ? AND deleted_at IS NULL;";

  const char* sql = parent_card_id.has_value() ? SQL_CHILDREN : SQL_ROOT;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare next sort_key failed");
  }

  bind_text(stmt, 1, project_id);
  if (parent_card_id.has_value()) {
    bind_text(stmt, 2, parent_card_id.value());
  }

  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    const double next = sqlite3_column_double(stmt, 0);
    sqlite3_finalize(stmt);
    return next;
  }

  sqlite3_finalize(stmt); // LCOV_EXCL_LINE
  if (rc != SQLITE_DONE) { // LCOV_EXCL_LINE
    throw_sqlite(db_.handle(), "next sort_key query failed"); // LCOV_EXCL_LINE
  }
  return 0.0; // LCOV_EXCL_LINE
}

void CardRepo::update_title(
    const std::string& card_id,
    const std::string& title,
    long long updated_at
) {
  static constexpr const char* SQL =
      "UPDATE cards SET title = ?, updated_at = ? WHERE card_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare update card title failed");
  }

  bind_text(stmt, 1, title);
  bind_int64(stmt, 2, updated_at);
  bind_text(stmt, 3, card_id);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "update card title failed");
  }
}

void CardRepo::touch_updated(const std::string& card_id, long long updated_at) {
  static constexpr const char* SQL = "UPDATE cards SET updated_at = ? WHERE card_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare touch card failed");
  }

  bind_int64(stmt, 1, updated_at);
  bind_text(stmt, 2, card_id);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "touch card failed");
  }
}

void CardRepo::soft_delete(const std::string& card_id, long long deleted_at, long long updated_at) {
  static constexpr const char* SQL =
      "UPDATE cards SET deleted_at = ?, updated_at = ? WHERE card_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare soft delete card failed");
  }

  bind_int64(stmt, 1, deleted_at);
  bind_int64(stmt, 2, updated_at);
  bind_text(stmt, 3, card_id);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "soft delete card failed");
  }
}

void CardRepo::restore(const std::string& card_id, long long updated_at) {
  static constexpr const char* SQL =
      "UPDATE cards SET deleted_at = NULL, updated_at = ? WHERE card_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare restore card failed");
  }

  bind_int64(stmt, 1, updated_at);
  bind_text(stmt, 2, card_id);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "restore card failed");
  }
}

void CardRepo::remove(const std::string& card_id) {
  static constexpr const char* SQL = "DELETE FROM cards WHERE card_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare delete card failed");
  }

  bind_text(stmt, 1, card_id);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "delete card failed");
  }
}

void CardRepo::move(
    const std::string& card_id,
    const std::optional<std::string>& parent_card_id,
    double sort_key,
    long long updated_at
) {
  static constexpr const char* SQL =
      "UPDATE cards SET parent_card_id = ?, sort_key = ?, updated_at = ? WHERE card_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare move card failed");
  }

  bind_text_optional(stmt, 1, parent_card_id);
  bind_double(stmt, 2, sort_key);
  bind_int64(stmt, 3, updated_at);
  bind_text(stmt, 4, card_id);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "move card failed");
  }
}

} // namespace holder::card
