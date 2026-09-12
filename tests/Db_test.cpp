#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "platform/Db.h"
#include "platform/DatabaseRebuild.h"
#include "platform/Tx.h"
#include "ai/AiMessagePaths.h"
#include "ai/AiThreadManifest.h"
#include "project/ProjectManifest.h"
#include "resource/ResourcePaths.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path make_temp_dir() {
  const auto base = std::filesystem::temp_directory_path();
  const auto suffix = std::to_string(
      static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())
  );
  auto dir = base / ("holder_db_test_" + suffix);
  std::filesystem::create_directories(dir);
  return dir;
}

std::string schema_sql() {
#ifdef SCHEMA_SQL_PATH
  std::ifstream input(SCHEMA_SQL_PATH);
  if (!input) throw std::runtime_error("schema.sql not found for tests");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
#else
  throw std::runtime_error("schema.sql not configured for tests");
#endif
}

void write_file(const std::filesystem::path& path, const std::string& body = "fixture") {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) throw std::runtime_error("failed to write test fixture");
  output << body;
}

int deny_sqlite_read(
    void* data, int action, const char* detail1, const char*, const char*, const char*
) {
  const auto* table = static_cast<const std::string*>(data);
  if (action == SQLITE_READ && detail1 != nullptr && *table == detail1) return SQLITE_DENY;
  return SQLITE_OK;
}

int deny_sqlite_pragma(
    void* data, int action, const char* detail1, const char*, const char*, const char*
) {
  const auto* pragma = static_cast<const std::string*>(data);
  if (action == SQLITE_PRAGMA && detail1 != nullptr && *pragma == detail1) return SQLITE_DENY;
  return SQLITE_OK;
}

struct DenySelectAfter {
  int remaining;
};

int deny_nth_select(
    void* data, int action, const char*, const char*, const char*, const char*
) {
  auto* state = static_cast<DenySelectAfter*>(data);
  if (action == SQLITE_SELECT && --state->remaining == 0) return SQLITE_DENY;
  return SQLITE_OK;
}

int interrupt_sqlite(void*) {
  return 1;
}

int create_sidecar_on_close(unsigned event, void* data, void*, void*) {
  if (event != SQLITE_TRACE_CLOSE) return 0;
  const auto& sidecar = *static_cast<const std::filesystem::path*>(data);
  std::error_code ignored;
  std::filesystem::create_directories(sidecar, ignored);
  std::ofstream(sidecar / "child", std::ios::binary) << "fixture";
  return 0;
}

void write_plain_project_manifest(
    const std::filesystem::path& root,
    const std::string& project_id
) {
  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Durable project";
  project.root_path = root.string();
  project.privacy_mode = "plain";
  project.created_at = 1;
  project.updated_at = 1;
  write_file(root / holder::project::kProjectBootstrapPath,
             holder::project::render_project_bootstrap(project));
  write_file(root / holder::project::kProjectManifestPath,
             holder::project::render_project_manifest(project));
}

} // namespace

TEST_CASE("Db move constructor transfers ownership", "[db]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "one.db";

  holder::platform::Db first;
  first.open(db_path);
  sqlite3* original_handle = first.handle();
  REQUIRE(original_handle != nullptr);

  holder::platform::Db moved(std::move(first));
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  REQUIRE(first.handle() == nullptr);
  REQUIRE(moved.handle() == original_handle);
  REQUIRE(moved.path() == db_path);
  REQUIRE_NOTHROW(moved.exec("CREATE TABLE IF NOT EXISTS t1 (id INTEGER);"));
}

TEST_CASE("Db move assignment transfers ownership and allows self move", "[db]") {
  const auto dir = make_temp_dir();
  const auto src_path = dir / "src.db";
  const auto dst_path = dir / "dst.db";

  holder::platform::Db source;
  source.open(src_path);
  sqlite3* source_handle = source.handle();
  REQUIRE(source_handle != nullptr);

  holder::platform::Db target;
  target.open(dst_path);
  REQUIRE(target.handle() != nullptr);

  target = std::move(source);
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  REQUIRE(source.handle() == nullptr);
  REQUIRE(target.handle() == source_handle);
  REQUIRE(target.path() == src_path);
  REQUIRE_NOTHROW(target.exec("CREATE TABLE IF NOT EXISTS t2 (id INTEGER);"));

  sqlite3* stable = target.handle();
  target = std::move(target);
  REQUIRE(target.handle() == stable);
}

TEST_CASE("Db open failure throws sqlite open error", "[db]") {
  const auto dir = make_temp_dir();
  const auto impossible_path = dir / "missing-parent" / "holder.db";

  holder::platform::Db db;
  REQUIRE_THROWS_WITH(
      db.open(impossible_path),
      Catch::Matchers::ContainsSubstring("sqlite open failed")
  );
}

TEST_CASE("Db exec failure includes sqlite message", "[db]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "bad-sql.db";

  holder::platform::Db db;
  db.open(db_path);

  REQUIRE_THROWS_WITH(
      db.exec("THIS IS NOT VALID SQL;"),
      Catch::Matchers::ContainsSubstring("sqlite exec failed")
  );
}

TEST_CASE("Database health distinguishes corruption from operational failures", "[db][rebuild]") {
  const auto dir = make_temp_dir();
  const auto malformed_path = dir / "malformed.db";
  {
    std::ofstream malformed(malformed_path, std::ios::binary | std::ios::trunc);
    REQUIRE(malformed.is_open());
    malformed << "not a sqlite database";
  }

  const auto malformed = holder::platform::inspect_database_health(malformed_path);
  REQUIRE(malformed.health == holder::platform::DatabaseHealth::Corrupt);

  const auto directory_path = dir / "directory.db";
  std::filesystem::create_directory(directory_path);
  const auto inaccessible = holder::platform::inspect_database_health(directory_path);
  REQUIRE(inaccessible.health == holder::platform::DatabaseHealth::IoError);
}

TEST_CASE("Database rebuild readiness markers are durable and reject malformed state", "[db][rebuild]") {
  const auto dir = make_temp_dir();
  const auto marker = dir / "server" / "rebuild-ready.json";
  REQUIRE_FALSE(holder::platform::database_rebuild_is_ready(marker));

  write_file(marker, "not json");
  REQUIRE_FALSE(holder::platform::database_rebuild_is_ready(marker));
  write_file(marker, R"({"version":1,"durable_owner_generation":0})");
  REQUIRE_FALSE(holder::platform::database_rebuild_is_ready(marker));

  holder::platform::mark_database_rebuild_ready(marker);
  REQUIRE(holder::platform::database_rebuild_is_ready(marker));

  const auto directory_target = dir / "marker-is-directory";
  std::filesystem::create_directory(directory_target);
  REQUIRE_THROWS(holder::platform::mark_database_rebuild_ready(directory_target));
}

TEST_CASE("Database durable ownership audit checks every Git-owned object kind", "[db][rebuild]") {
  const auto dir = make_temp_dir();
  const auto root = dir / "project";
  holder::platform::Db db;
  db.open(dir / "holder.db");
  db.exec(schema_sql());
  db.exec(
      "INSERT INTO projects(project_id,name,root_path,privacy_mode,created_at,updated_at) VALUES("
      "'project-1234','Project','" + root.string() + "','plain',1,1);"
  );

  REQUIRE_THROWS(holder::platform::audit_core_durable_ownership(db));
  write_plain_project_manifest(root, "project-1234");

  db.exec(
      "INSERT INTO ai_threads(thread_id,project_id,title,created_at,updated_at) VALUES("
      "'thread-1234','project-1234','Thread',1,1);"
      "INSERT INTO ai_messages(message_id,thread_id,role,source,content,created_at) VALUES("
      "'message-1234','thread-1234','user','local','Body',1);"
  );
  REQUIRE_THROWS(holder::platform::audit_core_durable_ownership(db));
  write_file(root / holder::core::ai_message_rel_path("message-1234"));
  REQUIRE_THROWS(holder::platform::audit_core_durable_ownership(db));
  write_file(root / holder::ai::ai_thread_manifest_rel_path("thread-1234"));

  db.exec(
      "INSERT INTO resources(resource_id,project_id,type,label,created_at,updated_at) VALUES("
      "'resource-1234','project-1234','thing','Resource',1,1);"
  );
  REQUIRE_THROWS(holder::platform::audit_core_durable_ownership(db));
  write_file(root / holder::resource::resource_rel_path("resource-1234"));

  db.exec(
      "INSERT INTO storage_locations(location_id,project_id,name,provider,config_json,created_at,updated_at) "
      "VALUES('location-1234','project-1234','Location','local_directory','{}',1,1);"
  );
  REQUIRE_THROWS(holder::platform::audit_core_durable_ownership(db));
  write_file(root / holder::resource::location_rel_path("location-1234"));
  REQUIRE_NOTHROW(holder::platform::audit_core_durable_ownership(db));
}

TEST_CASE("Database durable ownership audit reports prepare failures for every query",
          "[db][rebuild]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  db.exec(schema_sql());

  for (std::string table :
       {"projects", "ai_messages", "ai_threads", "resources", "storage_locations"}) {
    REQUIRE(sqlite3_set_authorizer(db.handle(), deny_sqlite_read, &table) == SQLITE_OK);
    REQUIRE_THROWS(holder::platform::audit_core_durable_ownership(db));
    REQUIRE(sqlite3_set_authorizer(db.handle(), nullptr, nullptr) == SQLITE_OK);
  }

  bool saw_thread_prepare = false;
  for (int select = 1; select <= 12 && !saw_thread_prepare; ++select) {
    DenySelectAfter state{select};
    REQUIRE(sqlite3_set_authorizer(db.handle(), deny_nth_select, &state) == SQLITE_OK);
    try {
      holder::platform::audit_core_durable_ownership(db);
    } catch (const std::exception& error) {
      saw_thread_prepare =
          std::string(error.what()).find("prepare AI thread ownership") !=
          std::string::npos;
    }
    REQUIRE(sqlite3_set_authorizer(db.handle(), nullptr, nullptr) == SQLITE_OK);
  }
  CHECK(saw_thread_prepare);
}

TEST_CASE("Database rebuild reports aggregate and validation preparation failures",
          "[db][rebuild]") {
  const auto dir = make_temp_dir();

  SECTION("old durable count query cannot prepare") {
    const auto database = dir / "old-count.db";
    holder::platform::Db db;
    db.open(database);
    db.exec(schema_sql());
    db.close();
    holder::platform::DatabaseRebuildRequest request;
    request.database_path = database;
    request.backup_root = dir / "backups";
    request.schema_sql = schema_sql();
    request.hooks.audit_existing = [](holder::platform::Db& old) {
      old.exec("DROP TABLE resource_metadata;");
    };
    REQUIRE_THROWS(holder::platform::rebuild_database_projection(request));
  }

  SECTION("old durable count query is interrupted while stepping") {
    const auto database = dir / "old-count-step.db";
    holder::platform::Db db;
    db.open(database);
    db.exec(schema_sql());
    db.close();
    holder::platform::DatabaseRebuildRequest request;
    request.database_path = database;
    request.backup_root = dir / "backups";
    request.schema_sql = schema_sql();
    request.hooks.audit_existing = [](holder::platform::Db& old) {
      sqlite3_progress_handler(old.handle(), 1, interrupt_sqlite, nullptr);
    };
    REQUIRE_THROWS_WITH(
        holder::platform::rebuild_database_projection(request),
        Catch::Matchers::ContainsSubstring("failed to count projects")
    );
  }

  SECTION("integrity pragma cannot prepare") {
    std::string denied = "integrity_check";
    holder::platform::DatabaseRebuildRequest request;
    request.database_path = dir / "integrity.db";
    request.schema_sql = schema_sql();
    request.dry_run = true;
    request.hooks.restore_after_projects = [&](holder::platform::Db& rebuilt) {
      REQUIRE(sqlite3_set_authorizer(rebuilt.handle(), deny_sqlite_pragma, &denied) == SQLITE_OK);
    };
    REQUIRE_THROWS(holder::platform::rebuild_database_projection(request));
  }

  SECTION("foreign key pragma cannot prepare") {
    std::string denied = "foreign_key_check";
    holder::platform::DatabaseRebuildRequest request;
    request.database_path = dir / "foreign-key.db";
    request.schema_sql = schema_sql();
    request.dry_run = true;
    request.hooks.restore_after_projects = [&](holder::platform::Db& rebuilt) {
      REQUIRE(sqlite3_set_authorizer(rebuilt.handle(), deny_sqlite_pragma, &denied) == SQLITE_OK);
    };
    REQUIRE_THROWS(holder::platform::rebuild_database_projection(request));
  }

  SECTION("rebuild sidecar cannot be removed") {
    holder::platform::DatabaseRebuildRequest request;
    request.database_path = dir / "sidecar.db";
    request.schema_sql = schema_sql();
    request.dry_run = true;
    auto sidecar = std::filesystem::path(request.database_path.string() +
                                         ".rebuild.tmp-wal");
    request.hooks.validate_rebuilt = [&](holder::platform::Db& rebuilt) {
      REQUIRE(sqlite3_trace_v2(rebuilt.handle(), SQLITE_TRACE_CLOSE,
                               create_sidecar_on_close, &sidecar) == SQLITE_OK);
    };
    REQUIRE_THROWS_WITH(
        holder::platform::rebuild_database_projection(request),
        Catch::Matchers::ContainsSubstring("failed to remove rebuild database sidecar")
    );
  }
}

TEST_CASE("Database rebuild rejects unsafe inputs before replacing the database", "[db][rebuild]") {
  const auto dir = make_temp_dir();
  holder::platform::DatabaseRebuildRequest request;
  REQUIRE_THROWS(holder::platform::rebuild_database_projection(request));

  request.database_path = dir / "database-is-directory";
  request.schema_sql = schema_sql();
  std::filesystem::create_directory(request.database_path);
  REQUIRE_THROWS(holder::platform::rebuild_database_projection(request));

  const auto corrupt = dir / "corrupt.db";
  write_file(corrupt, "not sqlite");
  const auto root = dir / "project";
  std::filesystem::create_directories(root);
  request.database_path = corrupt;
  request.project_roots = {root};
  request.durable_ownership_ready = false;
  REQUIRE_THROWS(holder::platform::rebuild_database_projection(request));

  request.durable_ownership_ready = true;
  request.required_authorities = {{"key store", dir / "missing-authority"}};
  REQUIRE_THROWS(holder::platform::rebuild_database_projection(request));

  request.database_path = dir / "missing.db";
  request.required_authorities.clear();
  request.project_roots = {dir / "missing-project-root"};
  REQUIRE_THROWS(holder::platform::rebuild_database_projection(request));

  const auto first_root = dir / "first-project";
  const auto second_root = dir / "second-project";
  write_plain_project_manifest(first_root, "same-project-id");
  write_plain_project_manifest(second_root, "same-project-id");
  request.project_roots = {first_root, second_root};
  REQUIRE_THROWS(holder::platform::rebuild_database_projection(request));

  request.project_roots.clear();
  write_file(std::filesystem::path(request.database_path.string() + ".rebuild.tmp"));
  REQUIRE_THROWS(holder::platform::rebuild_database_projection(request));
}

TEST_CASE("Database rebuild detects a projection whose durable counts changed", "[db][rebuild]") {
  const auto dir = make_temp_dir();
  const auto database = dir / "holder.db";
  {
    holder::platform::Db db;
    db.open(database);
    db.exec(schema_sql());
  }

  holder::platform::DatabaseRebuildRequest request;
  request.database_path = database;
  request.backup_root = dir / "backups";
  request.schema_sql = schema_sql();
  request.hooks.restore_before_projects = [](holder::platform::Db& rebuilt) {
    rebuilt.exec(
        "INSERT INTO projects(project_id,name,root_path,privacy_mode,created_at,updated_at) "
        "VALUES('unexpected-project','Unexpected','/tmp/unexpected','plain',1,1);"
    );
  };
  REQUIRE_THROWS(holder::platform::rebuild_database_projection(request));
  REQUIRE_FALSE(std::filesystem::exists(database.string() + ".rebuild.tmp"));
}

TEST_CASE("Tx destructor swallows rollback failure", "[db][tx]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "tx-rollback.db";

  holder::platform::Db db;
  db.open(db_path);

  REQUIRE_NOTHROW([&]() {
    holder::platform::Tx tx(db);
    db.close();
  }());
}
