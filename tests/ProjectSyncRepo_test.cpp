#include "core_test_helpers.h"
#include "project/ProjectRepo.h"
#include "project/ProjectSyncRepo.h"
#include <sqlite3.h>

namespace {
int sqlite_interrupt_cb(void* data) {
  auto* enabled = static_cast<int*>(data);
  return (enabled != nullptr && *enabled != 0) ? 1 : 0;
}

int deny_project_sync_insert(void*, int action, const char* detail1, const char*, const char*, const char*) {
  if (action == SQLITE_INSERT && detail1 != nullptr &&
      std::string(detail1) == "project_sync_state") {
    return SQLITE_DENY;
  }
  return SQLITE_OK;
}
} // namespace

TEST_CASE("ProjectSyncRepo records push retries and clears on success", "[sync][repo]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);

  holder::project::ProjectRepo projects(db);
  holder::model::Project project;
  project.project_id = "proj-1";
  project.name = "Project";
  project.root_path = (dir / "repo").string();
  project.created_at = 1;
  project.updated_at = 1;
  projects.create(project);

  holder::project::ProjectSyncRepo sync(db);

  sync.record_push_result(
      "proj-1",
      "auth_failed",
      false,
      std::optional<std::string>{"bad auth"},
      100
  );
  auto first = sync.get("proj-1");
  REQUIRE(first.has_value());
  REQUIRE(first->retry_count == 1);
  REQUIRE(first->next_retry_at.has_value());
  REQUIRE(first->next_retry_at.value() == 160);
  REQUIRE(first->last_sync_error.has_value());
  REQUIRE(first->last_sync_error.value() == "bad auth");
  REQUIRE(first->last_push_status.has_value());
  REQUIRE(first->last_push_status.value() == "auth_failed");

  sync.record_push_result(
      "proj-1",
      "unknown_error",
      false,
      std::optional<std::string>{"still broken"},
      200
  );
  auto second = sync.get("proj-1");
  REQUIRE(second.has_value());
  REQUIRE(second->retry_count == 2);
  REQUIRE(second->next_retry_at.has_value());
  REQUIRE(second->next_retry_at.value() == 500);

  sync.record_push_result("proj-1", "pushed", true, std::nullopt, 300);
  auto success = sync.get("proj-1");
  REQUIRE(success.has_value());
  REQUIRE(success->retry_count == 0);
  REQUIRE_FALSE(success->next_retry_at.has_value());
  REQUIRE_FALSE(success->last_sync_error.has_value());
  REQUIRE(success->last_push_at.has_value());
  REQUIRE(success->last_push_at.value() == 300);
}

TEST_CASE("ProjectSyncRepo records pull status and errors", "[sync][repo]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);

  holder::project::ProjectRepo projects(db);
  holder::model::Project project;
  project.project_id = "proj-1";
  project.name = "Project";
  project.root_path = (dir / "repo").string();
  project.created_at = 1;
  project.updated_at = 1;
  projects.create(project);

  holder::project::ProjectSyncRepo sync(db);
  sync.record_pull_result(
      "proj-1",
      "failed",
      false,
      std::optional<std::string>{"ff-only failed"},
      400
  );
  auto failed = sync.get("proj-1");
  REQUIRE(failed.has_value());
  REQUIRE(failed->last_pull_status.has_value());
  REQUIRE(failed->last_pull_status.value() == "failed");
  REQUIRE(failed->last_sync_error.has_value());
  REQUIRE(failed->last_sync_error.value() == "ff-only failed");
  REQUIRE(failed->last_sync_error_at.has_value());
  REQUIRE(failed->last_sync_error_at.value() == 400);
  REQUIRE(failed->pull_retry_count == 1);
  REQUIRE(failed->next_pull_retry_at.has_value());
  REQUIRE(failed->next_pull_retry_at.value() == 460);

  sync.record_pull_result("proj-1", "succeeded", true, std::nullopt, 450);
  auto ok = sync.get("proj-1");
  REQUIRE(ok.has_value());
  REQUIRE(ok->last_pull_at.has_value());
  REQUIRE(ok->last_pull_at.value() == 450);
  REQUIRE_FALSE(ok->last_sync_error.has_value());
  REQUIRE(ok->pull_retry_count == 0);
  REQUIRE_FALSE(ok->next_pull_retry_at.has_value());
}

TEST_CASE("ProjectSyncRepo stores uncommitted and unpushed counters", "[sync][repo]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);

  holder::project::ProjectRepo projects(db);
  holder::model::Project project;
  project.project_id = "proj-1";
  project.name = "Project";
  project.root_path = (dir / "repo").string();
  project.created_at = 1;
  project.updated_at = 1;
  projects.create(project);

  holder::project::ProjectSyncRepo sync(db);
  sync.update_activity_counts(
      "proj-1",
      {.uncommitted_changes_count = 3, .unpushed_commits_count = 7, .updated_at = 500}
  );

  const auto state = sync.get("proj-1");
  REQUIRE(state.has_value());
  REQUIRE(state->uncommitted_changes_count == 3);
  REQUIRE(state->unpushed_commits_count == 7);
}

TEST_CASE("ProjectSyncRepo upgrades old sync table with pull retry columns", "[sync][repo]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);

  holder::project::ProjectRepo projects(db);
  holder::model::Project project;
  project.project_id = "proj-1";
  project.name = "Project";
  project.root_path = (dir / "repo").string();
  project.created_at = 1;
  project.updated_at = 1;
  projects.create(project);

  // Simulate pre-upgrade schema with no pull_retry_count/next_pull_retry_at.
  db.exec("CREATE TABLE IF NOT EXISTS project_sync_state ("
          " project_id TEXT PRIMARY KEY REFERENCES projects(project_id) ON DELETE CASCADE,"
          " last_commit_at INTEGER NULL,"
          " last_push_at INTEGER NULL,"
          " last_pull_at INTEGER NULL,"
          " uncommitted_changes_count INTEGER NOT NULL DEFAULT 0,"
          " unpushed_commits_count INTEGER NOT NULL DEFAULT 0,"
          " last_push_status TEXT NULL,"
          " last_pull_status TEXT NULL,"
          " last_sync_error TEXT NULL,"
          " last_sync_error_at INTEGER NULL,"
          " retry_count INTEGER NOT NULL DEFAULT 0,"
          " next_retry_at INTEGER NULL,"
          " updated_at INTEGER NOT NULL DEFAULT 0"
          ");");

  holder::project::ProjectSyncRepo sync(db);
  sync.record_pull_result("proj-1", "failed", false, std::optional<std::string>{"boom"}, 500);

  const auto state = sync.get("proj-1");
  REQUIRE(state.has_value());
  REQUIRE(state->pull_retry_count == 1);
  REQUIRE(state->next_pull_retry_at.has_value());
  REQUIRE(state->next_pull_retry_at.value() == 560);
}

TEST_CASE("ProjectSyncRepo backoff caps at max retry schedule", "[sync][repo]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);

  holder::project::ProjectRepo projects(db);
  holder::model::Project project;
  project.project_id = "proj-1";
  project.name = "Project";
  project.root_path = (dir / "repo").string();
  project.created_at = 1;
  project.updated_at = 1;
  projects.create(project);

  holder::project::ProjectSyncRepo sync(db);

  for (int retry = 1; retry <= 5; ++retry) {
    const long long now = 1000 + retry * 10;
    sync.record_push_result("proj-1", "failed", false, std::optional<std::string>{"err"}, now);
  }

  const auto state = sync.get("proj-1");
  REQUIRE(state.has_value());
  REQUIRE(state->retry_count == 5);
  REQUIRE(state->next_retry_at.has_value());
  REQUIRE(state->next_retry_at.value() == (1000 + 5 * 10 + 1800));
}

TEST_CASE("ProjectSyncRepo uses default sync errors for empty messages", "[sync][repo]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);

  holder::project::ProjectRepo projects(db);
  holder::model::Project project;
  project.project_id = "proj-1";
  project.name = "Project";
  project.root_path = (dir / "repo").string();
  project.created_at = 1;
  project.updated_at = 1;
  projects.create(project);

  holder::project::ProjectSyncRepo sync(db);
  sync.record_push_result("proj-1", "failed", false, std::nullopt, 100);

  auto push_failed = sync.get("proj-1");
  REQUIRE(push_failed.has_value());
  REQUIRE(push_failed->last_sync_error.has_value());
  REQUIRE(push_failed->last_sync_error.value() == "push failed");

  sync.record_pull_result("proj-1", "failed", false, std::optional<std::string>{""}, 200);
  auto pull_failed = sync.get("proj-1");
  REQUIRE(pull_failed.has_value());
  REQUIRE(pull_failed->last_sync_error.has_value());
  REQUIRE(pull_failed->last_sync_error.value() == "pull failed");
}

TEST_CASE("ProjectSyncRepo remove deletes state row", "[sync][repo]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);

  holder::project::ProjectRepo projects(db);
  holder::model::Project project;
  project.project_id = "proj-1";
  project.name = "Project";
  project.root_path = (dir / "repo").string();
  project.created_at = 1;
  project.updated_at = 1;
  projects.create(project);

  holder::project::ProjectSyncRepo sync(db);
  sync.record_push_result("proj-1", "pushed", true, std::nullopt, 100);
  REQUIRE(sync.get("proj-1").has_value());

  sync.remove("proj-1");
  REQUIRE_FALSE(sync.get("proj-1").has_value());
}

TEST_CASE("ProjectSyncRepo throws when sqlite handle is closed", "[sync][repo]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);

  holder::project::ProjectRepo projects(db);
  holder::model::Project project;
  project.project_id = "proj-1";
  project.name = "Project";
  project.root_path = (dir / "repo").string();
  project.created_at = 1;
  project.updated_at = 1;
  projects.create(project);

  holder::project::ProjectSyncRepo sync(db);
  db.close();

  REQUIRE_THROWS(sync.get("proj-1"));
  REQUIRE_THROWS(sync.update_activity_counts(
      "proj-1",
      {.uncommitted_changes_count = 1, .unpushed_commits_count = 1, .updated_at = 1}
  ));
  REQUIRE_THROWS(sync.remove("proj-1"));
}

TEST_CASE("ProjectSyncRepo read/write/delete throw on interrupted sqlite step", "[sync][repo]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);

  holder::project::ProjectRepo projects(db);
  holder::model::Project project;
  project.project_id = "proj-1";
  project.name = "Project";
  project.root_path = (dir / "repo").string();
  project.created_at = 1;
  project.updated_at = 1;
  projects.create(project);

  holder::project::ProjectSyncRepo sync(db);

  sync.update_activity_counts(
      "proj-1",
      {.uncommitted_changes_count = 1, .unpushed_commits_count = 1, .updated_at = 1}
  );

  int interrupt_on = 1;
  sqlite3_progress_handler(db.handle(), 1, sqlite_interrupt_cb, &interrupt_on);

  REQUIRE_THROWS(sync.get("proj-1"));
  REQUIRE_THROWS(sync.record_push_result("proj-1", "failed", false, std::nullopt, 2));
  REQUIRE_THROWS(sync.remove("proj-1"));

  sqlite3_progress_handler(db.handle(), 0, nullptr, nullptr);
}

TEST_CASE("ProjectSyncRepo upsert prepare fails when insert is not authorized", "[sync][repo]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);

  holder::project::ProjectRepo projects(db);
  holder::model::Project project;
  project.project_id = "proj-1";
  project.name = "Project";
  project.root_path = (dir / "repo").string();
  project.created_at = 1;
  project.updated_at = 1;
  projects.create(project);

  holder::project::ProjectSyncRepo sync(db);
  REQUIRE(sqlite3_set_authorizer(db.handle(), deny_project_sync_insert, nullptr) == SQLITE_OK);
  REQUIRE_THROWS(sync.record_push_result("proj-1", "failed", false, std::nullopt, 2));
  REQUIRE(sqlite3_set_authorizer(db.handle(), nullptr, nullptr) == SQLITE_OK);
}

TEST_CASE("ProjectSyncRepo upsert step fails when trigger aborts insert", "[sync][repo]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);

  holder::project::ProjectRepo projects(db);
  holder::model::Project project;
  project.project_id = "proj-1";
  project.name = "Project";
  project.root_path = (dir / "repo").string();
  project.created_at = 1;
  project.updated_at = 1;
  projects.create(project);

  holder::project::ProjectSyncRepo sync(db);
  db.exec("CREATE TRIGGER block_project_sync_insert "
          "BEFORE INSERT ON project_sync_state "
          "BEGIN SELECT RAISE(ABORT, 'blocked insert'); END;");

  REQUIRE_THROWS(sync.record_push_result("proj-1", "failed", false, std::nullopt, 3));
}
