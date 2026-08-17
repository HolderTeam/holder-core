#include "ai/AiProviderCredentialRepo.h"

#include <sqlite3.h>

#include <optional>
#include <stdexcept>
#include <string>

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

holder::model::AiProviderCredential read_row(sqlite3_stmt* stmt) {
  holder::model::AiProviderCredential out;
  out.provider = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  out.api_key_preview = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  out.created_at = sqlite3_column_int64(stmt, 2);
  out.updated_at = sqlite3_column_int64(stmt, 3);
  return out;
} // LCOV_EXCL_LINE

} // namespace

AiProviderCredentialRepo::AiProviderCredentialRepo(holder::platform::Db& db)
    : db_(db) {}

std::vector<holder::model::AiProviderCredential> AiProviderCredentialRepo::list() const {
  static constexpr const char* SQL = "SELECT provider, api_key_preview, created_at, updated_at "
                                     "FROM ai_provider_credentials ORDER BY provider ASC;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare list provider credentials failed");
  }

  std::vector<holder::model::AiProviderCredential> rows;
  for (;;) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      rows.push_back(read_row(stmt));
      continue;
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
      throw_sqlite(db_.handle(), "list provider credentials failed");
    }
    break;
  }
  return rows;
} // LCOV_EXCL_LINE

std::optional<holder::model::AiProviderCredential> AiProviderCredentialRepo::get(
    const std::string& provider
) const {
  static constexpr const char* SQL = "SELECT provider, api_key_preview, created_at, updated_at "
                                     "FROM ai_provider_credentials WHERE provider = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare get provider credential failed");
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
    throw_sqlite(db_.handle(), "get provider credential failed");
  }
  return std::nullopt;
}

void AiProviderCredentialRepo::upsert(
    const std::string& provider,
    const std::string& api_key_preview,
    long long created_at,
    long long updated_at
) {
  static constexpr const char* SQL =
      "INSERT INTO ai_provider_credentials(provider, api_key_preview, created_at, updated_at) "
      "VALUES(?, ?, ?, ?) "
      "ON CONFLICT(provider) DO UPDATE SET "
      "api_key_preview = excluded.api_key_preview, updated_at = excluded.updated_at;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare upsert provider credential failed");
  }
  bind_text(stmt, 1, provider);
  bind_text(stmt, 2, api_key_preview);
  sqlite3_bind_int64(stmt, 3, created_at);
  sqlite3_bind_int64(stmt, 4, updated_at);

  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    throw_sqlite(db_.handle(), "upsert provider credential failed");
  }
  sqlite3_finalize(stmt);
}

void AiProviderCredentialRepo::remove(const std::string& provider) {
  static constexpr const char* SQL = "DELETE FROM ai_provider_credentials WHERE provider = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare delete provider credential failed");
  }
  bind_text(stmt, 1, provider);

  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    throw_sqlite(db_.handle(), "delete provider credential failed");
  }
  sqlite3_finalize(stmt);
}

} // namespace holder::ai
