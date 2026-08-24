#include "resource/LocationRepo.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <stdexcept>

namespace holder::resource {
namespace {

void bind_text(sqlite3_stmt* stmt, int index, const std::string& value) {
  if (sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
    throw std::runtime_error("sqlite bind text failed");
  }
}

void bind_int64(sqlite3_stmt* stmt, int index, long long value) {
  if (sqlite3_bind_int64(stmt, index, value) != SQLITE_OK) {
    throw std::runtime_error("sqlite bind integer failed");
  }
}

std::string text_column(sqlite3_stmt* stmt, int index) {
  const auto* value = sqlite3_column_text(stmt, index);
  return value ? reinterpret_cast<const char*>(value) : std::string();
}

holder::model::Location read_location(sqlite3_stmt* stmt) {
  holder::model::Location location;
  location.location_id = text_column(stmt, 0);
  location.project_id = text_column(stmt, 1);
  location.name = text_column(stmt, 2);
  location.provider = text_column(stmt, 3);
  location.configuration =
      nlohmann::json::parse(text_column(stmt, 4)).get<std::map<std::string, std::string>>();
  location.created_at = sqlite3_column_int64(stmt, 5);
  location.updated_at = sqlite3_column_int64(stmt, 6);
  return location;
}

} // namespace

LocationRepo::LocationRepo(holder::platform::Db& db)
    : db_(db) {}

void LocationRepo::put(const holder::model::Location& location) {
  if (location.location_id.empty() || location.project_id.empty() || location.name.empty() ||
      location.provider.empty()) {
    throw std::invalid_argument("location identity, project, name and provider are required");
  }
  static constexpr const char* SQL =
      "INSERT INTO storage_locations(location_id, project_id, name, provider, config_json, "
      "created_at, updated_at) VALUES(?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(location_id) DO UPDATE SET project_id=excluded.project_id, name=excluded.name, "
      "provider=excluded.provider, config_json=excluded.config_json, updated_at=excluded.updated_at;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error("prepare put location failed");
  }
  bind_text(stmt, 1, location.location_id);
  bind_text(stmt, 2, location.project_id);
  bind_text(stmt, 3, location.name);
  bind_text(stmt, 4, location.provider);
  bind_text(stmt, 5, nlohmann::json(location.configuration).dump());
  bind_int64(stmt, 6, location.created_at);
  bind_int64(stmt, 7, location.updated_at);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) throw std::runtime_error("put location failed");
}

std::optional<holder::model::Location> LocationRepo::get(const std::string& location_id) const {
  static constexpr const char* SQL =
      "SELECT location_id, project_id, name, provider, config_json, created_at, updated_at "
      "FROM storage_locations WHERE location_id = ?;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error("prepare get location failed");
  }
  bind_text(stmt, 1, location_id);
  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    auto location = read_location(stmt);
    sqlite3_finalize(stmt);
    return location;
  }
  sqlite3_finalize(stmt);
  if (rc == SQLITE_DONE) return std::nullopt;
  throw std::runtime_error("get location failed");
}

std::vector<holder::model::Location> LocationRepo::list(const std::string& project_id) const {
  static constexpr const char* SQL =
      "SELECT location_id, project_id, name, provider, config_json, created_at, updated_at "
      "FROM storage_locations WHERE project_id = ? ORDER BY updated_at DESC, location_id;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error("prepare list locations failed");
  }
  bind_text(stmt, 1, project_id);
  std::vector<holder::model::Location> out;
  while (true) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) {
      sqlite3_finalize(stmt);
      throw std::runtime_error("list locations failed");
    }
    out.push_back(read_location(stmt));
  }
  sqlite3_finalize(stmt);
  return out;
}

bool LocationRepo::is_in_use(const std::string& location_id) const {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(
          db_.handle(),
          "SELECT 1 FROM asset_placements WHERE location_id = ? LIMIT 1;",
          -1,
          &stmt,
          nullptr
      ) != SQLITE_OK) {
    throw std::runtime_error("prepare location use check failed");
  }
  bind_text(stmt, 1, location_id);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc == SQLITE_ROW) return true;
  if (rc == SQLITE_DONE) return false;
  throw std::runtime_error("location use check failed");
}

void LocationRepo::remove(const std::string& location_id) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(
          db_.handle(), "DELETE FROM storage_locations WHERE location_id = ?;", -1, &stmt, nullptr
      ) != SQLITE_OK) {
    throw std::runtime_error("prepare remove location failed");
  }
  bind_text(stmt, 1, location_id);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) throw std::runtime_error("remove location failed");
}

void LocationRepo::remove_project(const std::string& project_id) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(
          db_.handle(), "DELETE FROM storage_locations WHERE project_id = ?;", -1, &stmt, nullptr
      ) != SQLITE_OK) {
    throw std::runtime_error("prepare remove project locations failed");
  }
  bind_text(stmt, 1, project_id);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) throw std::runtime_error("remove project locations failed");
}

} // namespace holder::resource
