#include "index/FtsIndexer.h"

#include <sqlite3.h>

#include <stdexcept>

namespace holder::index {
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

} // namespace

FtsIndexer::FtsIndexer(holder::platform::Db& db)
    : db_(db) {}

void FtsIndexer::upsert_card(
    const std::string& card_id,
    const std::string& project_id,
    const std::string& title,
    const std::string& body
) {
  static constexpr const char* SQL_DELETE = "DELETE FROM cards_fts WHERE card_id = ?;";
  static constexpr const char* SQL_INSERT =
      "INSERT INTO cards_fts(card_id, project_id, title, body) "
      "VALUES(?, ?, ?, ?);";

  sqlite3_stmt* del = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL_DELETE, -1, &del, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare delete cards_fts failed");
  }
  bind_text(del, 1, card_id);
  const int rc_del = sqlite3_step(del);
  sqlite3_finalize(del);
  if (rc_del != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "delete cards_fts failed");
  }

  sqlite3_stmt* ins = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL_INSERT, -1, &ins, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare insert cards_fts failed");
  }
  bind_text(ins, 1, card_id);
  bind_text(ins, 2, project_id);
  bind_text(ins, 3, title);
  bind_text(ins, 4, body);
  const int rc_ins = sqlite3_step(ins);
  sqlite3_finalize(ins);
  if (rc_ins != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "insert cards_fts failed"); // LCOV_EXCL_LINE
  }
}

void FtsIndexer::delete_card(const std::string& card_id) {
  static constexpr const char* SQL = "DELETE FROM cards_fts WHERE card_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare delete cards_fts failed");
  }

  bind_text(stmt, 1, card_id);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "delete cards_fts failed");
  }
}

void FtsIndexer::upsert_message(
    const std::string& message_id,
    const std::string& thread_id,
    const std::string& project_id,
    const std::string& content
) {
  static constexpr const char* SQL_DELETE = "DELETE FROM ai_fts WHERE message_id = ?;";
  static constexpr const char* SQL_INSERT =
      "INSERT INTO ai_fts(message_id, thread_id, project_id, content) "
      "VALUES(?, ?, ?, ?);";

  sqlite3_stmt* del = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL_DELETE, -1, &del, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare delete ai_fts failed");
  }
  bind_text(del, 1, message_id);
  const int rc_del = sqlite3_step(del);
  sqlite3_finalize(del);
  if (rc_del != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "delete ai_fts failed");
  }

  sqlite3_stmt* ins = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL_INSERT, -1, &ins, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare insert ai_fts failed");
  }
  bind_text(ins, 1, message_id);
  bind_text(ins, 2, thread_id);
  bind_text(ins, 3, project_id);
  bind_text(ins, 4, content);
  const int rc_ins = sqlite3_step(ins);
  sqlite3_finalize(ins);
  if (rc_ins != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "insert ai_fts failed"); // LCOV_EXCL_LINE
  }
}

void FtsIndexer::delete_message(const std::string& message_id) {
  static constexpr const char* SQL = "DELETE FROM ai_fts WHERE message_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare delete ai_fts failed");
  }

  bind_text(stmt, 1, message_id);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "delete ai_fts failed");
  }
}

std::vector<FtsIndexer::SearchRow> FtsIndexer::search_cards(
    const std::string& project_id,
    const std::string& query,
    int limit,
    int offset
) {
  static constexpr const char* SQL = "SELECT c.card_id, c.title, c.updated_at, c.created_at, "
                                     "bm25(cards_fts) AS score, "
                                     "snippet(cards_fts, 2, '[', ']', '...', 10) "
                                     "FROM cards_fts "
                                     "JOIN cards c ON c.card_id = cards_fts.card_id "
                                     "WHERE cards_fts.project_id = ? AND cards_fts MATCH ? "
                                     "ORDER BY score "
                                     "LIMIT ? OFFSET ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare search cards_fts failed");
  }

  bind_text(stmt, 1, project_id);
  bind_text(stmt, 2, query);
  if (sqlite3_bind_int(stmt, 3, limit) != SQLITE_OK ||
      sqlite3_bind_int(stmt, 4, offset) != SQLITE_OK) {
    sqlite3_finalize(stmt); // LCOV_EXCL_LINE
    throw std::runtime_error("sqlite bind_int failed"); // LCOV_EXCL_LINE
  }

  std::vector<SearchRow> out;
  while (true) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      SearchRow row;
      row.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
      row.title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
      row.updated_at = sqlite3_column_int64(stmt, 2);
      row.created_at = sqlite3_column_int64(stmt, 3);
      row.rank = sqlite3_column_double(stmt, 4);
      const auto* text = sqlite3_column_text(stmt, 5);
      const std::string raw = text ? reinterpret_cast<const char*>(text) : "";
      if (raw.empty()) {
        row.snippet = ""; // LCOV_EXCL_LINE
      } else if (raw.front() == '[' || raw.find('[') != std::string::npos) {
        row.snippet = raw;
      } else {
        row.snippet = "[" + raw + "]"; // LCOV_EXCL_LINE
      }
      out.push_back(std::move(row));
      continue;
    }
    if (rc == SQLITE_DONE) break;
    sqlite3_finalize(stmt);
    throw_sqlite(db_.handle(), "search cards_fts failed");
  }

  sqlite3_finalize(stmt);
  return out;
}

std::vector<FtsIndexer::SearchRow> FtsIndexer::search_messages(
    const std::string& project_id,
    const std::string& query,
    int limit,
    int offset
) {
  static constexpr const char* SQL = "SELECT m.message_id, m.created_at, "
                                     "bm25(ai_fts) AS score, "
                                     "snippet(ai_fts, 3, '[', ']', '...', 10) "
                                     "FROM ai_fts "
                                     "JOIN ai_messages m ON m.message_id = ai_fts.message_id "
                                     "WHERE ai_fts.project_id = ? AND ai_fts MATCH ? "
                                     "ORDER BY score "
                                     "LIMIT ? OFFSET ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare search ai_fts failed");
  }

  bind_text(stmt, 1, project_id);
  bind_text(stmt, 2, query);
  if (sqlite3_bind_int(stmt, 3, limit) != SQLITE_OK ||
      sqlite3_bind_int(stmt, 4, offset) != SQLITE_OK) {
    sqlite3_finalize(stmt); // LCOV_EXCL_LINE
    throw std::runtime_error("sqlite bind_int failed"); // LCOV_EXCL_LINE
  }

  std::vector<SearchRow> out;
  while (true) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      SearchRow row;
      row.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
      row.created_at = sqlite3_column_int64(stmt, 1);
      row.rank = sqlite3_column_double(stmt, 2);
      const auto* text = sqlite3_column_text(stmt, 3);
      const std::string raw = text ? reinterpret_cast<const char*>(text) : "";
      if (raw.empty()) {
        row.snippet = ""; // LCOV_EXCL_LINE
      } else if (raw.front() == '[' || raw.find('[') != std::string::npos) {
        row.snippet = raw;
      } else {
        row.snippet = "[" + raw + "]"; // LCOV_EXCL_LINE
      }
      out.push_back(std::move(row));
      continue;
    }
    if (rc == SQLITE_DONE) break;
    sqlite3_finalize(stmt);
    throw_sqlite(db_.handle(), "search ai_fts failed");
  }

  sqlite3_finalize(stmt);
  return out;
}

} // namespace holder::index
