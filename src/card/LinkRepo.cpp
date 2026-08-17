#include "card/LinkRepo.h"

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

holder::model::CardLink read_link(sqlite3_stmt* stmt) {
  holder::model::CardLink link;
  link.project_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  link.from_card_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  link.to_card_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
  link.to_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
  link.kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
  if (sqlite3_column_type(stmt, 5) == SQLITE_NULL) {
    link.label.reset();
  } else {
    link.label = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
  }
  link.created_at = sqlite3_column_int64(stmt, 6);
  return link;
} // LCOV_EXCL_LINE

} // namespace

LinkRepo::LinkRepo(holder::platform::Db& db)
    : db_(db) {}

void LinkRepo::upsert_links(
    const std::string& project_id,
    const std::string& from_card_id,
    const std::vector<holder::model::CardLink>& links
) {
  static constexpr const char* SQL =
      "INSERT INTO card_links(project_id, from_card_id, to_card_id, to_type, kind, label, created_at) "
      "VALUES(?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(project_id, from_card_id, to_card_id, to_type, kind) DO UPDATE SET "
      "label=excluded.label, created_at=excluded.created_at;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare upsert links failed");
  }

  for (const auto& link : links) {
    if (link.project_id != project_id || link.from_card_id != from_card_id) {
      sqlite3_finalize(stmt);
      throw std::runtime_error("link project_id/from_card_id mismatch");
    }

    const std::string to_type = link.to_type.empty() ? "card" : link.to_type;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);

    bind_text(stmt, 1, link.project_id);
    bind_text(stmt, 2, link.from_card_id);
    bind_text(stmt, 3, link.to_card_id);
    bind_text(stmt, 4, to_type);
    bind_text(stmt, 5, link.kind);
    bind_text_optional(stmt, 6, link.label);
    bind_int64(stmt, 7, link.created_at);

    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
      sqlite3_finalize(stmt);
      throw_sqlite(db_.handle(), "upsert link failed");
    }
  }

  sqlite3_finalize(stmt);
}

std::vector<holder::model::CardLink> LinkRepo::list_outgoing(
    const std::string& project_id,
    const std::string& from_card_id
) const {
  static constexpr const char* SQL =
      "SELECT project_id, from_card_id, to_card_id, to_type, kind, label, created_at "
      "FROM card_links WHERE project_id = ? AND from_card_id = ? "
      "ORDER BY created_at DESC;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare list outgoing links failed");
  }

  bind_text(stmt, 1, project_id);
  bind_text(stmt, 2, from_card_id);

  std::vector<holder::model::CardLink> out;
  while (true) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      out.push_back(read_link(stmt));
      continue;
    }
    if (rc == SQLITE_DONE) break;
    sqlite3_finalize(stmt); // LCOV_EXCL_LINE
    throw_sqlite(db_.handle(), "list outgoing links failed"); // LCOV_EXCL_LINE
  }

  sqlite3_finalize(stmt);
  return out;
} // LCOV_EXCL_LINE

std::vector<holder::model::CardLink> LinkRepo::list_backlinks(
    const std::string& project_id,
    const std::string& to_card_id
) const {
  static constexpr const char* SQL =
      "SELECT project_id, from_card_id, to_card_id, to_type, kind, label, created_at "
      "FROM card_links WHERE project_id = ? AND to_card_id = ? "
      "ORDER BY created_at DESC;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare list backlinks failed");
  }

  bind_text(stmt, 1, project_id);
  bind_text(stmt, 2, to_card_id);

  std::vector<holder::model::CardLink> out;
  while (true) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      out.push_back(read_link(stmt));
      continue;
    }
    if (rc == SQLITE_DONE) break;
    sqlite3_finalize(stmt); // LCOV_EXCL_LINE
    throw_sqlite(db_.handle(), "list backlinks failed"); // LCOV_EXCL_LINE
  }

  sqlite3_finalize(stmt);
  return out;
} // LCOV_EXCL_LINE

std::vector<holder::model::CardLink> LinkRepo::list_backlinks_typed(
    const std::string& project_id,
    const std::string& to_card_id,
    const std::string& to_type
) const {
  static constexpr const char* SQL =
      "SELECT project_id, from_card_id, to_card_id, to_type, kind, label, created_at "
      "FROM card_links WHERE project_id = ? AND to_card_id = ? AND to_type = ? "
      "ORDER BY created_at DESC;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare list backlinks typed failed");
  }

  bind_text(stmt, 1, project_id);
  bind_text(stmt, 2, to_card_id);
  bind_text(stmt, 3, to_type);

  std::vector<holder::model::CardLink> out;
  while (true) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      out.push_back(read_link(stmt));
      continue;
    }
    if (rc == SQLITE_DONE) break;
    sqlite3_finalize(stmt); // LCOV_EXCL_LINE
    throw_sqlite(db_.handle(), "list backlinks typed failed"); // LCOV_EXCL_LINE
  }

  sqlite3_finalize(stmt);
  return out;
} // LCOV_EXCL_LINE

void LinkRepo::delete_link(
    const std::string& project_id,
    const std::string& from_card_id,
    const std::string& to_card_id,
    const std::optional<std::string>& to_type,
    const std::optional<std::string>& kind
) {
  const char* sql_with_kind_type =
      "DELETE FROM card_links WHERE project_id = ? AND from_card_id = ? "
      "AND to_card_id = ? AND to_type = ? AND kind = ?;";
  const char* sql_with_kind = "DELETE FROM card_links WHERE project_id = ? AND from_card_id = ? "
                              "AND to_card_id = ? AND kind = ?;";
  const char* sql_without_kind_type =
      "DELETE FROM card_links WHERE project_id = ? AND from_card_id = ? "
      "AND to_card_id = ? AND to_type = ?;";
  const char* sql_without_kind =
      "DELETE FROM card_links WHERE project_id = ? AND from_card_id = ? AND to_card_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  const bool has_kind = kind.has_value();
  const bool has_type = to_type.has_value();
  const char* sql = sql_without_kind;
  if (has_kind && has_type) {
    sql = sql_with_kind_type;
  } else if (has_kind) {
    sql = sql_with_kind;
  } else if (has_type) {
    sql = sql_without_kind_type;
  }
  if (sqlite3_prepare_v2(db_.handle(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare delete link failed");
  }

  bind_text(stmt, 1, project_id);
  bind_text(stmt, 2, from_card_id);
  bind_text(stmt, 3, to_card_id);
  if (has_kind && has_type) {
    bind_text(stmt, 4, to_type.value());
    bind_text(stmt, 5, kind.value());
  } else if (has_kind) {
    bind_text(stmt, 4, kind.value());
  } else if (has_type) {
    bind_text(stmt, 4, to_type.value());
  }

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "delete link failed");
  }
}

void LinkRepo::delete_links_to_typed(
    const std::string& project_id,
    const std::string& to_card_id,
    const std::string& to_type
) {
  static constexpr const char* SQL =
      "DELETE FROM card_links WHERE project_id = ? AND to_card_id = ? AND to_type = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare delete links to failed");
  }

  bind_text(stmt, 1, project_id);
  bind_text(stmt, 2, to_card_id);
  bind_text(stmt, 3, to_type);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "delete links to failed");
  }
}

void LinkRepo::delete_links_from(const std::string& project_id, const std::string& from_card_id) {
  static constexpr const char* SQL =
      "DELETE FROM card_links WHERE project_id = ? AND from_card_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare delete links failed");
  }

  bind_text(stmt, 1, project_id);
  bind_text(stmt, 2, from_card_id);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "delete links failed");
  }
}

} // namespace holder::card
