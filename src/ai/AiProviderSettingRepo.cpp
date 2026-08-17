#include "ai/AiProviderSettingRepo.h"

#include <sqlite3.h>

#include <stdexcept>

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

holder::model::AiProviderSetting read_row(sqlite3_stmt* stmt) {
  holder::model::AiProviderSetting out;
  out.provider = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  out.enabled = sqlite3_column_int(stmt, 1) != 0;
  out.updated_at = sqlite3_column_int64(stmt, 2);
  return out;
} // LCOV_EXCL_LINE

} // namespace

AiProviderSettingRepo::AiProviderSettingRepo(holder::platform::Db& db)
    : db_(db) {}

std::vector<holder::model::AiProviderSetting> AiProviderSettingRepo::list() const {
  static constexpr const char* SQL = "SELECT provider, enabled, updated_at "
                                     "FROM ai_provider_settings "
                                     "ORDER BY provider ASC;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare list provider settings failed");
  }

  std::vector<holder::model::AiProviderSetting> rows;
  for (;;) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      rows.push_back(read_row(stmt));
      continue;
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
      throw_sqlite(db_.handle(), "list provider settings failed");
    }
    break;
  }
  return rows;
} // LCOV_EXCL_LINE

std::optional<holder::model::AiProviderSetting> AiProviderSettingRepo::get(
    const std::string& provider
) const {
  static constexpr const char* SQL = "SELECT provider, enabled, updated_at "
                                     "FROM ai_provider_settings "
                                     "WHERE provider = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare get provider setting failed");
  }
  bind_text(stmt, 1, provider);

  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    auto out = read_row(stmt);
    sqlite3_finalize(stmt);
    return out;
  }
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "get provider setting failed");
  }
  return std::nullopt;
}

void AiProviderSettingRepo::upsert(
    const std::string& provider,
    bool enabled,
    long long updated_at
) {
  static constexpr const char* SQL =
      "INSERT INTO ai_provider_settings(provider, enabled, updated_at) "
      "VALUES(?, ?, ?) "
      "ON CONFLICT(provider) DO UPDATE SET "
      "enabled = excluded.enabled, updated_at = excluded.updated_at;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare upsert provider setting failed");
  }
  bind_text(stmt, 1, provider);
  sqlite3_bind_int(stmt, 2, enabled ? 1 : 0);
  sqlite3_bind_int64(stmt, 3, updated_at);

  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    throw_sqlite(db_.handle(), "upsert provider setting failed");
  }
  sqlite3_finalize(stmt);
}

void AiProviderSettingRepo::remove(const std::string& provider) {
  static constexpr const char* SQL = "DELETE FROM ai_provider_settings WHERE provider = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare delete provider setting failed");
  }
  bind_text(stmt, 1, provider);

  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    throw_sqlite(db_.handle(), "delete provider setting failed");
  }
  sqlite3_finalize(stmt);
}

} // namespace holder::ai
