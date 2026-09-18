#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "sync/ProjectSyncOperation.h"

#include "card/CardRepo.h"
#include "card/CardStore.h"
#include "core_test_helpers.h"
#include "git/GitRepo.h"
#include "index/FtsIndexer.h"
#include "model/Card.h"
#include "model/Project.h"
#include "project/ProjectRepo.h"
#include "project/ProjectSyncRepo.h"
#include "project/Rebuilder.h"

#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void create_project(
    holder::platform::Db& db,
    const std::string& project_id,
    const std::filesystem::path& root,
    const std::optional<std::string>& remote
) {
  std::filesystem::create_directories(root);
  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Project";
  project.root_path = root.string();
  project.git_remote_url = remote;
  project.privacy_mode = "plain";
  project.created_at = 1;
  project.updated_at = 1;
  holder::project::ProjectRepo(db).create(project);
}

class RecordingGitOps final : public holder::git::GitOps {
 public:
  bool fail_pull = false;
  holder::git::PushResult push_result{
      holder::git::PushStatus::Pushed, 1, 0, "abc123", {}
  };
  std::filesystem::path root;
  std::vector<std::string> calls;

  void open_or_init(const std::filesystem::path& path) override {
    root = path;
    calls.push_back("open");
  }
  void write_file(const std::filesystem::path&, const std::string&) override {}
  void stage_path(const std::filesystem::path&) override {}
  void remove_path(const std::filesystem::path&) override {}
  void commit(const std::string&) override {}
  void set_remote(const std::string&, const std::string&) override { calls.push_back("remote"); }
  void remove_remote(const std::string&) override {}
  void pull_remote_ff_only(const std::string&) override {
    calls.push_back("pull");
    if (fail_pull) throw std::runtime_error("pull failed for tests");
  }
  holder::git::RemoteProbeResult probe_remote(const std::string&) override { return {}; }
  holder::git::PushResult push_branch(const std::string&, const std::string&, bool) override {
    calls.push_back("push");
    return push_result;
  }
  std::filesystem::path repo_dir() const override { return root; }
};

} // namespace

TEST_CASE("ProjectSyncOperation reports and records an unset remote", "[sync][operation]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::index::FtsIndexer fts(db);
  create_project(db, "proj-1", dir / "project", std::nullopt);
  RecordingGitOps git;

  const auto result = holder::sync::run_project_sync(
      db,
      &fts,
      git,
      "proj-1",
      {.pull = true, .push = false, .push_after_failed_pull = false, .branch = "",
       .set_upstream = true, .now = 100}
  );

  REQUIRE_FALSE(result.succeeded());
  REQUIRE(result.pull.attempted);
  REQUIRE(result.pull.status == holder::sync::PullPhaseStatus::RemoteUnset);
  REQUIRE_FALSE(result.push.attempted);
  REQUIRE(git.calls.empty());
  const auto state = holder::project::ProjectSyncRepo(db).get("proj-1");
  REQUIRE(state.has_value());
  REQUIRE(state->last_pull_status == "remote_unset");
  REQUIRE(state->last_sync_error == "Remote URL is not configured.");
}

TEST_CASE("ProjectSyncOperation records an unset remote for a push-only request", "[sync][operation]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::index::FtsIndexer fts(db);
  create_project(db, "proj-1", dir / "project", std::nullopt);
  RecordingGitOps git;

  const auto result = holder::sync::run_project_sync(
      db,
      &fts,
      git,
      "proj-1",
      {.pull = false, .push = true, .push_after_failed_pull = false, .branch = "",
       .set_upstream = true, .now = 100}
  );

  REQUIRE_FALSE(result.succeeded());
  REQUIRE_FALSE(result.pull.attempted);
  REQUIRE(result.push.attempted);
  REQUIRE(result.push.status == holder::git::PushStatus::RemoteUnset);
  REQUIRE(result.push.error_message == "Remote URL is not configured.");
  REQUIRE(git.calls.empty());
  const auto state = holder::project::ProjectSyncRepo(db).get("proj-1");
  REQUIRE(state.has_value());
  REQUIRE(state->last_push_status == holder::git::push_status_name(holder::git::PushStatus::RemoteUnset));
}

TEST_CASE("ProjectSyncOperation rejects a request that selects neither pull nor push", "[sync][operation]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::index::FtsIndexer fts(db);
  create_project(db, "proj-1", dir / "project", std::nullopt);
  RecordingGitOps git;

  REQUIRE_THROWS_WITH(
      holder::sync::run_project_sync(
          db,
          &fts,
          git,
          "proj-1",
          {.pull = false, .push = false, .push_after_failed_pull = false, .branch = "",
           .set_upstream = true, .now = 100}
      ),
      "project sync must request pull, push, or both"
  );
  REQUIRE(git.calls.empty());
}

namespace {

// Changes a project's root_path through a second connection at a precise moment: when the
// sync connection prepares its second SELECT. run_project_sync reads the project, waits for the
// repository lock, then reads it again, so the second read is exactly the point at which another
// process could have moved the project while the lock was contended. Doing it from the
// authorizer keeps the test single-threaded and deterministic.
struct RootChangeInjector {
  sqlite3* other = nullptr;
  std::string project_id;
  std::string new_root;
  int selects = 0;
};

int change_root_on_second_select(void* data, int action, const char*, const char*, const char*, const char*) {
  auto* injector = static_cast<RootChangeInjector*>(data);
  if (action == SQLITE_SELECT && ++injector->selects == 2) {
    const std::string sql = "UPDATE projects SET root_path = '" + injector->new_root +
                            "' WHERE project_id = '" + injector->project_id + "';";
    sqlite3_exec(injector->other, sql.c_str(), nullptr, nullptr, nullptr);
  }
  return SQLITE_OK;
}

} // namespace

TEST_CASE("ProjectSyncOperation aborts when the project root changes while waiting for the lock",
          "[sync][operation]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::index::FtsIndexer fts(db);
  create_project(db, "proj-1", dir / "project", "https://example.invalid/repo.git");
  RecordingGitOps git;

  holder::platform::Db other;
  other.open(dir / "holder.db");
  RootChangeInjector injector{other.handle(), "proj-1", (dir / "moved").string(), 0};
  REQUIRE(sqlite3_set_authorizer(db.handle(), change_root_on_second_select, &injector) == SQLITE_OK);

  REQUIRE_THROWS_WITH(
      holder::sync::run_project_sync(
          db,
          &fts,
          git,
          "proj-1",
          {.pull = true, .push = false, .push_after_failed_pull = false, .branch = "",
           .set_upstream = true, .now = 100}
      ),
      "project root changed while waiting for sync: proj-1"
  );
  sqlite3_set_authorizer(db.handle(), nullptr, nullptr);
  // Nothing may run against the repository once the root is known to have moved.
  REQUIRE(git.calls.empty());
}

TEST_CASE("ProjectSyncOperation stops before push when pull fails", "[sync][operation]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::index::FtsIndexer fts(db);
  create_project(db, "proj-1", dir / "project", "https://example.com/repo.git");
  RecordingGitOps git;
  git.fail_pull = true;

  const auto result = holder::sync::run_project_sync(
      db,
      &fts,
      git,
      "proj-1",
      {.pull = true, .push = true, .push_after_failed_pull = false, .branch = "",
       .set_upstream = true, .now = 200}
  );

  REQUIRE_FALSE(result.succeeded());
  REQUIRE(result.pull.status == holder::sync::PullPhaseStatus::Failed);
  REQUIRE(result.pull.error_message == "pull failed for tests");
  REQUIRE_FALSE(result.push.attempted);
  REQUIRE(std::find(git.calls.begin(), git.calls.end(), "push") == git.calls.end());
  const auto state = holder::project::ProjectSyncRepo(db).get("proj-1");
  REQUIRE(state.has_value());
  REQUIRE(state->last_pull_status == "failed");
  REQUIRE_FALSE(state->last_push_status.has_value());
}

TEST_CASE("ProjectSyncOperation pulls before a successful push", "[sync][operation]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::index::FtsIndexer fts(db);
  create_project(db, "proj-1", dir / "project", "https://example.com/repo.git");
  RecordingGitOps git;

  const auto result = holder::sync::run_project_sync(
      db,
      &fts,
      git,
      "proj-1",
      {.pull = true, .push = true, .push_after_failed_pull = false, .branch = "",
       .set_upstream = true, .now = 300}
  );

  REQUIRE(result.succeeded());
  REQUIRE(result.pull.status == holder::sync::PullPhaseStatus::Succeeded);
  REQUIRE(result.push.attempted);
  REQUIRE(result.push.status == holder::git::PushStatus::Pushed);
  const auto pull = std::find(git.calls.begin(), git.calls.end(), "pull");
  const auto push = std::find(git.calls.begin(), git.calls.end(), "push");
  REQUIRE(pull != git.calls.end());
  REQUIRE(push != git.calls.end());
  REQUIRE(pull < push);
  const auto state = holder::project::ProjectSyncRepo(db).get("proj-1");
  REQUIRE(state.has_value());
  REQUIRE(state->last_pull_status == "succeeded");
  REQUIRE(state->last_push_status == "pushed");
}

TEST_CASE("ProjectSyncOperation ignores a failing activity-metrics refresh after a successful pull",
          "[sync][operation]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::index::FtsIndexer fts(db);
  create_project(db, "proj-1", dir / "project", "https://example.invalid/repo.git");
  // A .git entry that is not a valid gitfile makes the post-sync repository inspection throw,
  // while the stubbed git operations report a clean pull.
  {
    std::ofstream(dir / "project" / ".git") << "this is not a gitfile";
  }
  RecordingGitOps git;

  const auto result = holder::sync::run_project_sync(
      db,
      &fts,
      git,
      "proj-1",
      {.pull = true, .push = false, .push_after_failed_pull = false, .branch = "",
       .set_upstream = true, .now = 100}
  );

  REQUIRE(result.succeeded());
  REQUIRE(result.pull.status == holder::sync::PullPhaseStatus::Succeeded);
  const auto state = holder::project::ProjectSyncRepo(db).get("proj-1");
  REQUIRE(state.has_value());
  REQUIRE(state->last_pull_status == "succeeded");
}

TEST_CASE("ProjectSyncOperation fast-forward pull rebuilds the project index", "[sync][operation]") {
  const auto dir = holder::test::make_temp_dir();
  const auto remote_root = dir / "remote";
  const auto local_root = dir / "local";

  auto remote_db = holder::test::open_db_with_schema(dir / "remote.db");
  holder::index::FtsIndexer remote_fts(remote_db);
  create_project(remote_db, "proj-1", remote_root, std::nullopt);
  holder::git::RealGitOps remote_git;
  holder::model::Card first;
  first.card_id = "11111111-1111-4111-8111-111111111111";
  first.project_id = "proj-1";
  first.title = "First";
  holder::card::CardStore(remote_db, &remote_fts, nullptr, &remote_git).create(first, "first");

  holder::git::GitRepo clone;
  clone.open_or_init(local_root);
  clone.set_remote("origin", remote_root.string());
  clone.pull_remote_ff_only("origin");

  auto local_db = holder::test::open_db_with_schema(dir / "local.db");
  holder::index::FtsIndexer local_fts(local_db);
  create_project(local_db, "proj-1", local_root, remote_root.string());
  holder::store::Rebuilder(local_db, &local_fts)
      .rebuild_project(holder::project::ProjectRepo(local_db).get("proj-1").value());

  holder::model::Card second;
  second.card_id = "22222222-2222-4222-8222-222222222222";
  second.project_id = "proj-1";
  second.title = "Second";
  holder::card::CardStore(remote_db, &remote_fts, nullptr, &remote_git).create(second, "second");

  holder::git::RealGitOps local_git;
  const auto result = holder::sync::run_project_sync(
      local_db,
      &local_fts,
      local_git,
      "proj-1",
      {.pull = true, .push = false, .push_after_failed_pull = false, .branch = "",
       .set_upstream = true, .now = 400}
  );

  REQUIRE(result.succeeded());
  REQUIRE(result.pull.status == holder::sync::PullPhaseStatus::Succeeded);
  REQUIRE(result.pull.conflicts_resolved == 0);
  REQUIRE(holder::card::CardRepo(local_db).list_all("proj-1").size() == 2);
}

TEST_CASE("Project sync names every persisted pull phase", "[sync]") {
  REQUIRE(
      std::string(holder::sync::pull_phase_status_name(holder::sync::PullPhaseStatus::NotAttempted)) ==
      "not_attempted"
  );
}
