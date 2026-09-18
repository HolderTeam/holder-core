#include "platform/DatabaseRebuild.h"

#include "ai/AiMessagePaths.h"
#include "ai/AiThreadManifest.h"
#include "card/CardPaths.h"
#include "index/FtsIndexer.h"
#include "platform/Migrations.h"
#include "project/ProjectManifest.h"
#include "project/StartupRecovery.h"
#include "resource/ResourcePaths.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <process.h>
#endif

namespace holder::platform {
namespace {

constexpr int kReadinessVersion = 1;

// A fixed ".tmp" name would let two concurrent writers race on the same temp file
// before either atomic rename happens, corrupting it. Make each writer's temp file
// unique.
std::string unique_temp_suffix() {
  static std::atomic<unsigned long long> counter{0};
#ifdef _WIN32
  const auto pid = static_cast<unsigned long>(::_getpid());
#else
  const auto pid = static_cast<unsigned long>(::getpid());
#endif
  return "." + std::to_string(pid) + "." + std::to_string(counter.fetch_add(1));
}

bool is_sqlite_corruption_failure(int code) {
  switch (code & 0xff) {
  case SQLITE_ERROR:
  case SQLITE_CORRUPT:
  case SQLITE_FORMAT:
  case SQLITE_NOTADB:
    return true;
  // The remaining default outcomes require a failure inside a fixed SQLite
  // operation on a handle owned entirely by inspect_database_health().
  default: // LCOV_EXCL_LINE
    return false; // LCOV_EXCL_LINE
  }
}

std::string health_name(DatabaseHealth health) {
  switch (health) {
  case DatabaseHealth::Missing: return "missing";
  case DatabaseHealth::Healthy: return "healthy";
  case DatabaseHealth::Corrupt: return "corrupt";
  // rebuild_database_projection rejects IoError before constructing a report.
  case DatabaseHealth::IoError: return "io_error"; // LCOV_EXCL_LINE
  }
  return "unknown"; // LCOV_EXCL_LINE
}

std::vector<std::filesystem::path> normalized_roots(
    std::vector<std::filesystem::path> roots
) {
  for (auto& root : roots) {
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(root, ec);
    if (!ec) root = canonical;
  }
  std::sort(roots.begin(), roots.end());
  roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
  return roots;
}

std::map<std::string, long long> durable_counts(Db& db) {
  static const std::vector<std::string> tables = {
      "projects", "cards", "card_links", "milestones", "resources", "resource_metadata",
      "storage_locations", "assets", "asset_placements", "ai_threads", "ai_messages",
  };
  std::map<std::string, long long> result;
  for (const auto& table : tables) {
    sqlite3_stmt* stmt = nullptr;
    const auto sql = "SELECT COUNT(*) FROM " + table + ";";
    if (sqlite3_prepare_v2(db.handle(), sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
      throw std::runtime_error("failed to count " + table + ": " + sqlite3_errmsg(db.handle()));
    }
    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
      sqlite3_finalize(stmt);
      throw std::runtime_error("failed to count " + table);
    }
    result[table] = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
  }
  return result;
}

void validate_database(Db& db, int expected_schema_version) {
  auto check = [&](const std::string& pragma, const std::string& expected) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db.handle(), pragma.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
      throw std::runtime_error("database validation prepare failed");
    }
    const int rc = sqlite3_step(stmt);
    const std::string value = rc == SQLITE_ROW && sqlite3_column_text(stmt, 0)
                                  ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0))
                                  : std::string();
    sqlite3_finalize(stmt);
    if (value != expected) throw std::runtime_error("database validation failed: " + pragma);
  };
  check("PRAGMA integrity_check;", "ok");

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db.handle(), "PRAGMA foreign_key_check;", -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error("foreign key validation prepare failed");
  }
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) throw std::runtime_error("foreign key validation failed");
  if (expected_schema_version > 0) {
    Migrations::ensure_schema_version(db, expected_schema_version);
  }
}

std::filesystem::path unique_backup_dir(
    const std::filesystem::path& backup_root,
    bool corrupt
) {
  const auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch()
  ).count();
  for (int suffix = 0; suffix < 1000; ++suffix) {
    const auto name = std::string(corrupt ? "quarantine-" : "backup-") +
                      std::to_string(timestamp) + (suffix == 0 ? "" : "-" + std::to_string(suffix));
    const auto candidate = backup_root / name;
    if (!std::filesystem::exists(candidate)) return candidate;
  }
  // Requires 1,000 pre-existing same-second backup names. The bound prevents
  // an infinite loop but is not a practical runtime state.
  throw std::runtime_error("unable to allocate database backup directory"); // LCOV_EXCL_LINE
}

void move_if_exists(const std::filesystem::path& from, const std::filesystem::path& to) {
  if (std::filesystem::exists(from)) std::filesystem::rename(from, to);
}

void use_delete_journal_for_rebuild(Db& db) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(
          db.handle(), "PRAGMA journal_mode = DELETE;", -1, &stmt, nullptr
      ) != SQLITE_OK) {
    // This fixed pragma runs on a newly opened private database before hooks;
    // failure requires an SQLite runtime/allocation fault.
    throw std::runtime_error("failed to prepare rebuild journal mode"); // LCOV_EXCL_LINE
  }
  const int rc = sqlite3_step(stmt);
  const std::string mode = rc == SQLITE_ROW && sqlite3_column_text(stmt, 0)
                               ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0))
                               : std::string();
  sqlite3_finalize(stmt);
  if (mode != "delete") {
    throw std::runtime_error("failed to switch rebuild database out of WAL mode"); // LCOV_EXCL_LINE
  }
}

void remove_rebuild_sidecars(const std::filesystem::path& database_path) {
  for (const auto& path : {
           std::filesystem::path(database_path.string() + "-wal"),
           std::filesystem::path(database_path.string() + "-shm"),
       }) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) {
      throw std::runtime_error(
          "failed to remove rebuild database sidecar " + path.string() + ": " + ec.message()
      );
    }
  }
}

void sync_directory(const std::filesystem::path& path) {
#ifndef _WIN32
  const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) throw std::runtime_error("failed to open database directory for fsync");
  const int rc = ::fsync(fd);
  ::close(fd);
  if (rc != 0) throw std::runtime_error("failed to fsync database directory");
#else
  (void)path;
#endif
}

void require_file_rows(Db& db, const std::string& label, const std::string& sql) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db.handle(), sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error("prepare durable ownership audit failed for " + label);
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const auto* root_text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    const auto* rel_text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    if (!root_text || !rel_text ||
        !std::filesystem::is_regular_file(std::filesystem::path(root_text) / rel_text)) {
      sqlite3_finalize(stmt);
      throw std::runtime_error(label + " exists only in SQLite or has a missing durable file");
    }
  }
  sqlite3_finalize(stmt);
}

void populate_report(DatabaseRebuildReport& report, const std::map<std::string, long long>& counts) {
  report.projects = static_cast<std::size_t>(counts.at("projects"));
  report.cards = static_cast<std::size_t>(counts.at("cards"));
  report.ai_threads = static_cast<std::size_t>(counts.at("ai_threads"));
  report.ai_messages = static_cast<std::size_t>(counts.at("ai_messages"));
  report.resources = static_cast<std::size_t>(counts.at("resources"));
  report.assets = static_cast<std::size_t>(counts.at("assets"));
  report.placements = static_cast<std::size_t>(counts.at("asset_placements"));
  report.locations = static_cast<std::size_t>(counts.at("storage_locations"));
}

} // namespace

std::string DatabaseRebuildReport::to_json() const {
  return nlohmann::json{
      {"ok", true}, {"dry_run", dry_run}, {"previous_health", previous_health},
      {"projects", projects}, {"cards", cards}, {"ai_threads", ai_threads},
      {"ai_messages", ai_messages}, {"resources", resources}, {"assets", assets},
      {"placements", placements}, {"locations", locations},
      {"quarantined_cards", quarantined_cards},
      {"regenerated", {"card_tags", "full_text_search", "git_sync_status"}},
      {"reset", {"ai_run_history", "model_cooldowns", "transient_retry_state"}},
      {"warnings", nlohmann::json::array()},
      {"backup_path", backup_path.empty() ? nlohmann::json(nullptr)
                                            : nlohmann::json(backup_path.string())},
  }.dump(2);
}

DatabaseHealthResult inspect_database_health(const std::filesystem::path& path) {
  if (!std::filesystem::exists(path)) return {DatabaseHealth::Missing, "database file is absent"};
  sqlite3* db = nullptr;
  const int open_rc = sqlite3_open_v2(path.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
  if (open_rc != SQLITE_OK) {
    const int code = db ? sqlite3_extended_errcode(db) : open_rc;
    const std::string message = db ? sqlite3_errmsg(db) : "sqlite open failed";
    if (db) sqlite3_close(db);
    const bool corrupt = code == SQLITE_CORRUPT || code == SQLITE_NOTADB;
    return {corrupt ? DatabaseHealth::Corrupt : DatabaseHealth::IoError, message};
  }
  sqlite3_stmt* stmt = nullptr;
  const int prepare_rc = sqlite3_prepare_v2(db, "PRAGMA quick_check;", -1, &stmt, nullptr);
  if (prepare_rc != SQLITE_OK) {
    const int code = sqlite3_extended_errcode(db);
    const std::string message = sqlite3_errmsg(db);
    sqlite3_close(db);
    // Apple SQLite can report a malformed database as generic SQLITE_ERROR here,
    // while newer SQLite versions normally use SQLITE_NOTADB. The statement is a
    // fixed, valid pragma, so a non-operational prepare failure means the database
    // itself cannot be interpreted and is safe to classify as corrupt.
    return {
        is_sqlite_corruption_failure(code) ? DatabaseHealth::Corrupt
                                           : DatabaseHealth::IoError,
        message,
    };
  }
  const int step_rc = sqlite3_step(stmt);
  const std::string result = step_rc == SQLITE_ROW && sqlite3_column_text(stmt, 0)
                                 ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0))
                                 : std::string();
  const int code = sqlite3_extended_errcode(db);
  const std::string message = sqlite3_errmsg(db);
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  if (step_rc == SQLITE_ROW && result == "ok") return {DatabaseHealth::Healthy, "ok"};
  // Healthy, malformed, and open-I/O cases are covered. The remaining
  // quick_check step outcomes require a SQLite/storage fault on this internally
  // owned read-only handle, where tests have no safe injection seam. GCC also
  // assigns synthetic counters to the compound condition and aggregate end.
  if (!result.empty() || is_sqlite_corruption_failure(code) || // LCOV_EXCL_LINE
      is_sqlite_corruption_failure(step_rc)) { // LCOV_EXCL_LINE
    return {
        DatabaseHealth::Corrupt,
        !result.empty() ? result : (!message.empty() ? message : "quick_check failed"), // LCOV_EXCL_LINE
    }; // LCOV_EXCL_LINE
  }
  return {DatabaseHealth::IoError, !message.empty() ? message : "quick_check could not complete"}; // LCOV_EXCL_LINE
}

bool database_rebuild_is_ready(const std::filesystem::path& readiness_path) {
  if (!std::filesystem::is_regular_file(readiness_path)) return false;
  try {
    std::ifstream input(readiness_path, std::ios::binary);
    const auto body = nlohmann::json::parse(input);
    return body.value("version", 0) == kReadinessVersion &&
           body.value("durable_owner_generation", 0) == 1;
  } catch (const std::exception&) {
    return false;
  }
}

void mark_database_rebuild_ready(const std::filesystem::path& readiness_path) {
  std::filesystem::create_directories(readiness_path.parent_path());
  auto temporary = readiness_path;
  temporary += ".tmp" + unique_temp_suffix();
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("failed to write database rebuild readiness marker");
    output << nlohmann::json{
        {"version", kReadinessVersion}, // LCOV_EXCL_LINE - GCC assigns no counter to this executed initializer.
        {"durable_owner_generation", 1} // LCOV_EXCL_LINE - GCC gives this initializer no counter.
    }.dump(2) << '\n';
    output.flush();
    if (!output) throw std::runtime_error("failed to flush database rebuild readiness marker");
  }
#ifndef _WIN32
  if (::chmod(temporary.c_str(), S_IRUSR | S_IWUSR) != 0) {
    // temporary was just created by this process in a writable directory.
    throw std::runtime_error("failed to restrict database rebuild readiness marker"); // LCOV_EXCL_LINE
  }
#endif
  std::error_code ec;
  std::filesystem::rename(temporary, readiness_path, ec);
#ifdef _WIN32
  if (ec) {
    std::error_code status_ec;
    const bool existing_regular_file =
      std::filesystem::is_regular_file(readiness_path, status_ec);

    if (!status_ec && existing_regular_file) {
      std::filesystem::remove(readiness_path, ec);
      if (!ec) {
        std::filesystem::rename(temporary, readiness_path, ec);
      }
    }
  }
#endif
  if (ec) {
    std::filesystem::remove(temporary);
    throw std::runtime_error(
        "failed to replace database rebuild readiness marker: " + ec.message()
    );
  }
}

// Reads quarantine_log_path's existing JSON array (best-effort -- a missing or unreadable log
// starts fresh rather than blocking the rebuild that's trying to report into it) and atomically
// rewrites it with new_entries appended. Entries accumulate across every rebuild that ever
// quarantines something, so a device's full quarantine history stays inspectable in one place.
void append_quarantine_log(const std::filesystem::path& quarantine_log_path, const nlohmann::json& new_entries) {
  if (new_entries.empty()) return;
  nlohmann::json existing = nlohmann::json::array();
  if (std::filesystem::is_regular_file(quarantine_log_path)) {
    try {
      std::ifstream input(quarantine_log_path, std::ios::binary);
      auto parsed = nlohmann::json::parse(input);
      if (parsed.is_array()) existing = std::move(parsed);
    } catch (const std::exception&) { // LCOV_EXCL_START - corrupt-log recovery is covered; GCC omits empty handler.
      // A corrupt quarantine log must not itself block quarantining -- start a fresh one rather
      // than throw. The old (unparseable) file is overwritten below.
    }
    // LCOV_EXCL_STOP
  }
  for (const auto& entry : new_entries) existing.push_back(entry);

  std::filesystem::create_directories(quarantine_log_path.parent_path());
  auto temporary = quarantine_log_path;
  temporary += ".tmp" + unique_temp_suffix();
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("failed to write quarantine log");
    output << existing.dump(2) << '\n';
    output.flush();
    if (!output) throw std::runtime_error("failed to flush quarantine log");
  }
  std::error_code ec;
  std::filesystem::rename(temporary, quarantine_log_path, ec);
  if (ec) { // LCOV_EXCL_START - filesystem rename faults are not injectable through std::filesystem.
    std::filesystem::remove(temporary);
    throw std::runtime_error("failed to replace quarantine log: " + ec.message());
  } // LCOV_EXCL_STOP
}

// A card whose durable file is genuinely missing (not the trash-path case above, which is
// resolved before this ever runs) is quarantined rather than treated as fatal: the row is
// removed from the live projection and durably logged to quarantine_log_path, but every other
// card -- a project can easily have hundreds -- still opens normally. One card that's actually
// lost must never be a reason the other 999 become inaccessible. Returns the number quarantined,
// for the caller's report.
std::size_t quarantine_cards_with_missing_files(Db& db, const std::filesystem::path& quarantine_log_path) {
  sqlite3_stmt* card_stmt = nullptr;
  if (sqlite3_prepare_v2(
          db.handle(),
          "SELECT p.root_path, c.card_id, c.project_id, c.title, c.deleted_at FROM cards c "
          "JOIN projects p USING(project_id);",
          -1, &card_stmt, nullptr
      ) != SQLITE_OK) {
    throw std::runtime_error("prepare durable ownership audit failed for card");
  }
  std::vector<std::string> missing_ids;
  nlohmann::json log_entries = nlohmann::json::array();
  while (sqlite3_step(card_stmt) == SQLITE_ROW) {
    const std::filesystem::path root =
        reinterpret_cast<const char*>(sqlite3_column_text(card_stmt, 0));
    const std::string id = reinterpret_cast<const char*>(sqlite3_column_text(card_stmt, 1));
    const std::string project_id = reinterpret_cast<const char*>(sqlite3_column_text(card_stmt, 2));
    const auto* title_text = reinterpret_cast<const char*>(sqlite3_column_text(card_stmt, 3));
    const bool deleted = sqlite3_column_type(card_stmt, 4) != SQLITE_NULL;
    const auto rel = deleted ? holder::core::card_trash_rel_path(id) : holder::core::card_rel_path(id);
    if (!std::filesystem::is_regular_file(root / rel)) {
      missing_ids.push_back(id);
      log_entries.push_back({
          {"card_id", id},
          {"project_id", project_id},
          {"title", title_text != nullptr ? std::string(title_text) : std::string()},
          {"was_trashed", deleted},
          {"quarantined_at",
           std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch()
           ).count()},
      });
    }
  }
  sqlite3_finalize(card_stmt);

  for (const auto& id : missing_ids) {
    sqlite3_stmt* delete_stmt = nullptr;
    if (sqlite3_prepare_v2(db.handle(), "DELETE FROM cards WHERE card_id = ?;", -1, &delete_stmt, nullptr) != // LCOV_EXCL_START - SQLite prepare failure requires engine fault injection.
        SQLITE_OK) {
      throw std::runtime_error("prepare card quarantine delete failed");
    } // LCOV_EXCL_STOP
    sqlite3_bind_text(delete_stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(delete_stmt);
    sqlite3_finalize(delete_stmt);
    if (rc != SQLITE_DONE) throw std::runtime_error("card quarantine delete failed");
  }
  append_quarantine_log(quarantine_log_path, log_entries);
  return missing_ids.size();
}

std::size_t audit_core_durable_ownership(Db& db, const std::filesystem::path& quarantine_log_path) {
  require_file_rows(
      db, "project metadata",
      "SELECT root_path, '.holder/privacy.json' FROM projects "
      "UNION ALL SELECT root_path, '.holder/project.json' FROM projects;"
  );
  // Cards are quarantined rather than audited-and-thrown -- see quarantine_cards_with_missing_files's
  // doc comment. This also folds in the trash-path fix: a trashed card's stored rel_path is never
  // updated to its post-trash location (CardStore::trash moves the file to card_trash_rel_path()
  // and CardRepo::soft_delete only touches deleted_at/updated_at), so the expected path is always
  // recomputed from card_id + deleted_at rather than trusting the stored column -- the same way
  // CardStore, Rebuilder, and the AI message audit just below already do.
  const std::size_t quarantined = quarantine_cards_with_missing_files(db, quarantine_log_path);

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(
          db.handle(),
          "SELECT p.root_path, m.message_id, m.deleted_at FROM ai_messages m "
          "JOIN ai_threads t ON t.thread_id=m.thread_id "
          "JOIN projects p ON p.project_id=t.project_id;", -1, &stmt, nullptr
      ) != SQLITE_OK) {
    throw std::runtime_error("prepare AI message ownership audit failed");
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const std::filesystem::path root = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    const std::string id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    const bool deleted = sqlite3_column_type(stmt, 2) != SQLITE_NULL;
    const auto rel = deleted ? holder::core::ai_message_trash_rel_path(id)
                             : holder::core::ai_message_rel_path(id);
    if (!std::filesystem::is_regular_file(root / rel)) {
      sqlite3_finalize(stmt);
      throw std::runtime_error("AI message exists only in SQLite");
    }
  }
  sqlite3_finalize(stmt);

  if (sqlite3_prepare_v2(
          db.handle(),
          "SELECT p.root_path, t.thread_id FROM ai_threads t JOIN projects p USING(project_id);",
          -1, &stmt, nullptr
      ) != SQLITE_OK) {
    throw std::runtime_error("prepare AI thread ownership audit failed");
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const std::filesystem::path root = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    const std::string id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    if (!std::filesystem::is_regular_file(root / holder::ai::ai_thread_manifest_rel_path(id))) {
      sqlite3_finalize(stmt);
      throw std::runtime_error("AI thread exists only in SQLite");
    }
  }
  sqlite3_finalize(stmt);

  if (sqlite3_prepare_v2(
          db.handle(),
          "SELECT p.root_path, r.resource_id FROM resources r JOIN projects p USING(project_id);",
          -1, &stmt, nullptr
      ) != SQLITE_OK) {
    throw std::runtime_error("prepare Resource ownership audit failed");
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const std::filesystem::path root = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    const std::string id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    if (!std::filesystem::is_regular_file(root / holder::resource::resource_rel_path(id))) {
      sqlite3_finalize(stmt);
      throw std::runtime_error("Resource exists only in SQLite");
    }
  }
  sqlite3_finalize(stmt);

  if (sqlite3_prepare_v2(
          db.handle(),
          "SELECT p.root_path, l.location_id FROM storage_locations l "
          "JOIN projects p USING(project_id);", -1, &stmt, nullptr
      ) != SQLITE_OK) {
    throw std::runtime_error("prepare Location ownership audit failed");
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const std::filesystem::path root = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    const std::string id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    if (!std::filesystem::is_regular_file(root / holder::resource::location_rel_path(id))) {
      sqlite3_finalize(stmt);
      throw std::runtime_error("storage Location exists only in SQLite");
    }
  }
  sqlite3_finalize(stmt);
  return quarantined;
}

DatabaseRebuildReport rebuild_database_projection(const DatabaseRebuildRequest& input) {
  auto request = input;
  request.project_roots = normalized_roots(std::move(request.project_roots));
  if (request.database_path.empty() || request.schema_sql.empty()) {
    throw std::invalid_argument("database path and schema SQL are required");
  }
  const auto health = inspect_database_health(request.database_path);
  if (health.health == DatabaseHealth::IoError) {
    throw std::runtime_error("database I/O failure is not safe to rebuild: " + health.detail);
  }
  if ((health.health == DatabaseHealth::Corrupt ||
       (health.health == DatabaseHealth::Missing && !request.project_roots.empty())) &&
      !request.durable_ownership_ready) {
    throw std::runtime_error("database was preserved because durable ownership is not ready");
  }
  if (health.health == DatabaseHealth::Corrupt ||
      (health.health == DatabaseHealth::Missing && !request.project_roots.empty())) {
    for (const auto& [label, path] : request.required_authorities) {
      if (!std::filesystem::is_regular_file(path)) {
        throw std::runtime_error("database recovery authority is missing its " + label + ": " + path.string());
      }
    }
  }

  std::set<std::string> project_ids;
  for (const auto& root : request.project_roots) {
    if (!std::filesystem::is_directory(root)) {
      throw std::runtime_error("registered project root is unavailable: " + root.string());
    }
    const auto project = holder::project::read_project_manifest(root);
    if (!project_ids.insert(project.project_id).second) {
      throw std::runtime_error("duplicate project_id in discovered roots: " + project.project_id);
    }
  }

  const auto quarantine_log_path = request.database_path.parent_path() / "quarantined-cards.json";
  std::map<std::string, long long> old_counts;
  std::size_t quarantined_cards = 0;
  if (health.health == DatabaseHealth::Healthy) {
    Db old_db;
    old_db.open(request.database_path);
    quarantined_cards = audit_core_durable_ownership(old_db, quarantine_log_path);
    if (request.hooks.audit_existing) request.hooks.audit_existing(old_db);
    old_counts = durable_counts(old_db);
    old_db.close();
  }

  DatabaseRebuildReport report;
  report.dry_run = request.dry_run;
  report.previous_health = health_name(health.health);
  report.quarantined_cards = quarantined_cards;
  auto temporary = request.database_path;
  temporary += ".rebuild.tmp";
  for (const auto& artifact : {
           temporary,
           std::filesystem::path(temporary.string() + "-wal"),
           std::filesystem::path(temporary.string() + "-shm"),
       }) {
    if (std::filesystem::exists(artifact)) {
      throw std::runtime_error("stale rebuild temporary database exists: " + artifact.string());
    }
  }

  std::map<std::string, long long> new_counts;
  try {
    Db rebuilt;
    rebuilt.open(temporary);
    // Rebuild is an offline, single-connection operation. Using a rollback
    // journal ensures the finished database is one self-contained file before
    // the atomic rename. In particular, Apple SQLite may persist empty WAL/SHM
    // sidecars after the final connection closes.
    use_delete_journal_for_rebuild(rebuilt);
    rebuilt.exec(request.schema_sql);
    if (request.hooks.restore_before_projects) request.hooks.restore_before_projects(rebuilt);
    holder::index::FtsIndexer fts(rebuilt);
    holder::project::recover_project_roots(
        rebuilt, &fts, request.project_roots,
        // Strict recovery rejects encrypted projects whose durable key material is absent,
        // so this fallback generator is deliberately unreachable. The callback is still
        // required by recover_project_roots' shared API.
        [] { return std::string("unused-in-strict-recovery"); }, // LCOV_EXCL_LINE
        true
    );
    if (request.hooks.restore_after_projects) request.hooks.restore_after_projects(rebuilt);
    validate_database(rebuilt, request.expected_schema_version);
    if (request.hooks.validate_rebuilt) request.hooks.validate_rebuilt(rebuilt);
    new_counts = durable_counts(rebuilt);
    if (!old_counts.empty() && old_counts != new_counts) {
      throw std::runtime_error("rebuilt database durable object counts do not match source database");
    }
    rebuilt.close();
    remove_rebuild_sidecars(temporary);
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    std::filesystem::remove(temporary.string() + "-wal", ignored);
    std::filesystem::remove(temporary.string() + "-shm", ignored);
    throw;
  }
  populate_report(report, new_counts);
  if (request.dry_run) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    std::filesystem::remove(temporary.string() + "-wal", ignored);
    std::filesystem::remove(temporary.string() + "-shm", ignored);
    return report;
  }

  std::filesystem::path backup_dir;
  if (health.health != DatabaseHealth::Missing) {
    backup_dir = unique_backup_dir(request.backup_root, health.health == DatabaseHealth::Corrupt);
    std::filesystem::create_directories(backup_dir);
    move_if_exists(request.database_path, backup_dir / "holder.db");
    move_if_exists(request.database_path.string() + "-wal", backup_dir / "holder.db-wal");
    move_if_exists(request.database_path.string() + "-shm", backup_dir / "holder.db-shm");
  }
  try {
    std::filesystem::rename(temporary, request.database_path);
    sync_directory(request.database_path.parent_path());
    const auto final_health = inspect_database_health(request.database_path);
    if (final_health.health != DatabaseHealth::Healthy) {
      // The same file was fully validated immediately before an atomic rename;
      // only external mutation/storage failure can invalidate it here.
      throw std::runtime_error("replacement database failed final health check: " + final_health.detail); // LCOV_EXCL_LINE
    }
  } catch (...) { // LCOV_EXCL_LINE
    // Recovery from an external failure during rename/fsync/final re-open. The
    // normal replacement and healthy-backup paths are integration-tested.
    std::error_code ignored;                                  // LCOV_EXCL_LINE
    std::filesystem::remove(request.database_path, ignored);  // LCOV_EXCL_LINE
    if (!backup_dir.empty() && health.health == DatabaseHealth::Healthy) { // LCOV_EXCL_LINE
      move_if_exists(backup_dir / "holder.db", request.database_path); // LCOV_EXCL_LINE
      move_if_exists(backup_dir / "holder.db-wal", request.database_path.string() + "-wal"); // LCOV_EXCL_LINE
      move_if_exists(backup_dir / "holder.db-shm", request.database_path.string() + "-shm"); // LCOV_EXCL_LINE
    }
    throw; // LCOV_EXCL_LINE
  } // LCOV_EXCL_LINE
  report.backup_path = backup_dir;
  return report;
}

} // namespace holder::platform
