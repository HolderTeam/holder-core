#include "project/ProjectRepo.h"

#include <sqlite3.h>

#include <optional>
#include <stdexcept>
#include <utility>

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
  if (!value.has_value()) {
    if (sqlite3_bind_null(stmt, idx) != SQLITE_OK) {
      throw std::runtime_error("sqlite bind_null failed"); // LCOV_EXCL_LINE
    }
    return;
  }
  bind_text(stmt, idx, value.value());
}

void bind_int64(sqlite3_stmt* stmt, int idx, long long value) {
  if (sqlite3_bind_int64(stmt, idx, static_cast<sqlite3_int64>(value)) != SQLITE_OK) {
    throw std::runtime_error("sqlite bind_int64 failed"); // LCOV_EXCL_LINE
  }
}

holder::model::Project read_project(sqlite3_stmt* stmt) {
  holder::model::Project p;
  p.project_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  p.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  p.root_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
  if (sqlite3_column_type(stmt, 3) != SQLITE_NULL) {
    p.git_remote_url = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
  } else {
    p.git_remote_url.reset();
  }
  if (sqlite3_column_type(stmt, 4) != SQLITE_NULL) {
    p.git_provider = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
  } else {
    p.git_provider.reset();
  }
  p.privacy_mode = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
  if (sqlite3_column_type(stmt, 6) != SQLITE_NULL) {
    p.project_key_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
  } else {
    p.project_key_id.reset();
  }
  p.created_at = sqlite3_column_int64(stmt, 7);
  p.updated_at = sqlite3_column_int64(stmt, 8);
  return p;
} // LCOV_EXCL_LINE

} // namespace

ProjectRepo::ProjectRepo(holder::platform::Db& db)
    : db_(db) {}

void ProjectRepo::create(const holder::model::Project& project) {
  static constexpr const char* SQL =
      "INSERT INTO projects(project_id, name, root_path, git_remote_url, git_provider, "
      "privacy_mode, project_key_id, created_at, updated_at) "
      "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?);";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare insert project failed");
  }

  bind_text(stmt, 1, project.project_id);
  bind_text(stmt, 2, project.name);
  bind_text(stmt, 3, project.root_path);
  bind_text_optional(stmt, 4, project.git_remote_url);
  bind_text_optional(stmt, 5, project.git_provider);
  bind_text(stmt, 6, project.privacy_mode);
  bind_text_optional(stmt, 7, project.project_key_id);
  bind_int64(stmt, 8, project.created_at);
  bind_int64(stmt, 9, project.updated_at);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "insert project failed"); // LCOV_EXCL_LINE
  }
}

std::optional<holder::model::Project> ProjectRepo::get(const std::string& project_id) const {
  static constexpr const char* SQL =
      "SELECT project_id, name, root_path, git_remote_url, git_provider, privacy_mode, "
      "project_key_id, created_at, updated_at "
      "FROM projects WHERE project_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare get project failed");
  }

  bind_text(stmt, 1, project_id);

  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    auto project = read_project(stmt);
    sqlite3_finalize(stmt);
    return project;
  }

  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "get project failed");
  }
  return std::nullopt;
}

std::vector<holder::model::Project> ProjectRepo::list() const {
  static constexpr const char* SQL =
      "SELECT project_id, name, root_path, git_remote_url, git_provider, privacy_mode, "
      "project_key_id, created_at, updated_at "
      "FROM projects ORDER BY updated_at DESC;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare list projects failed");
  }

  std::vector<holder::model::Project> out;
  while (true) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      out.push_back(read_project(stmt));
      continue;
    }
    if (rc == SQLITE_DONE) break;
    sqlite3_finalize(stmt);
    throw_sqlite(db_.handle(), "list projects failed");
  }

  sqlite3_finalize(stmt);
  return out;
}

void ProjectRepo::update_name(
    const std::string& project_id,
    const std::string& name,
    long long updated_at
) {
  static constexpr const char* SQL =
      "UPDATE projects SET name = ?, updated_at = ? WHERE project_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare update project name failed");
  }

  bind_text(stmt, 1, name);
  bind_int64(stmt, 2, updated_at);
  bind_text(stmt, 3, project_id);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "update project name failed"); // LCOV_EXCL_LINE
  }
}

void ProjectRepo::update_root_path(
    const std::string& project_id,
    const std::string& root_path,
    long long updated_at
) {
  static constexpr const char* SQL =
      "UPDATE projects SET root_path = ?, updated_at = ? WHERE project_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare update project root failed");
  }

  bind_text(stmt, 1, root_path);
  bind_int64(stmt, 2, updated_at);
  bind_text(stmt, 3, project_id);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "update project root failed"); // LCOV_EXCL_LINE
  }
}

void ProjectRepo::update_git_remote(
    const std::string& project_id,
    const std::optional<std::string>& git_remote_url,
    long long updated_at
) {
  static constexpr const char* SQL =
      "UPDATE projects SET git_remote_url = ?, updated_at = ? WHERE project_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare update git remote failed");
  }

  bind_text_optional(stmt, 1, git_remote_url);
  bind_int64(stmt, 2, updated_at);
  bind_text(stmt, 3, project_id);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "update git remote failed"); // LCOV_EXCL_LINE
  }
}

void ProjectRepo::update_git_provider(
    const std::string& project_id,
    const std::optional<std::string>& git_provider,
    long long updated_at
) {
  static constexpr const char* SQL =
      "UPDATE projects SET git_provider = ?, updated_at = ? WHERE project_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare update git provider failed");
  }

  bind_text_optional(stmt, 1, git_provider);
  bind_int64(stmt, 2, updated_at);
  bind_text(stmt, 3, project_id);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "update git provider failed"); // LCOV_EXCL_LINE
  }
}

void ProjectRepo::update_privacy_mode(
    const std::string& project_id,
    const std::string& privacy_mode,
    long long updated_at
) {
  static constexpr const char* SQL =
      "UPDATE projects SET privacy_mode = ?, updated_at = ? WHERE project_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare update privacy mode failed");
  }

  bind_text(stmt, 1, privacy_mode);
  bind_int64(stmt, 2, updated_at);
  bind_text(stmt, 3, project_id);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "update privacy mode failed"); // LCOV_EXCL_LINE
  }
}

void ProjectRepo::update_project_key_id(
    const std::string& project_id,
    const std::optional<std::string>& project_key_id,
    long long updated_at
) {
  static constexpr const char* SQL =
      "UPDATE projects SET project_key_id = ?, updated_at = ? WHERE project_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare update project key id failed");
  }

  bind_text_optional(stmt, 1, project_key_id);
  bind_int64(stmt, 2, updated_at);
  bind_text(stmt, 3, project_id);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "update project key id failed"); // LCOV_EXCL_LINE
  }
}

void ProjectRepo::touch_updated(const std::string& project_id, long long updated_at) {
  static constexpr const char* SQL = "UPDATE projects SET updated_at = ? WHERE project_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare touch project failed");
  }

  bind_int64(stmt, 1, updated_at);
  bind_text(stmt, 2, project_id);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "touch project failed"); // LCOV_EXCL_LINE
  }
}

void ProjectRepo::remove(const std::string& project_id) {
  static constexpr const char* SQL = "DELETE FROM projects WHERE project_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare delete project failed");
  }

  bind_text(stmt, 1, project_id);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "delete project failed"); // LCOV_EXCL_LINE
  }
}

} // namespace holder::project
