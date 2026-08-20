#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "sync/PullConflictResolution.h"

#include "card/CardPaths.h"
#include "card/CardRepo.h"
#include "card/CardStore.h"
#include "core_test_helpers.h"
#include "git/GitOps.h"
#include "index/FtsIndexer.h"
#include "model/Card.h"
#include "model/Project.h"
#include "privacy/ProjectPrivacy.h"
#include "project/ProjectRepo.h"
#include "project/Rebuilder.h"

#include <fstream>

namespace {

using holder::test::EnvGuard;
using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;

void create_plain_project(
    holder::project::ProjectRepo& projects,
    const std::string& project_id,
    const std::filesystem::path& root_path
) {
  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Project";
  project.root_path = root_path.string();
  project.privacy_mode = "plain";
  project.created_at = 1;
  project.updated_at = 1;
  projects.create(project);
}

} // namespace

TEST_CASE(
    "resolve_pull_conflicts decrypts the pre-merge local card for an encrypted_git project",
    "[sync]"
) {
  const auto dir = make_temp_dir();
  EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  const std::string project_id = "proj-1";
  const auto remote_dir = dir / "remote";
  const auto local_dir = dir / "local";

  // Seed the encrypted project and its base card on what becomes "remote".
  auto remote_db = open_db_with_schema(dir / "remote.db");
  holder::index::FtsIndexer remote_fts(remote_db);
  holder::project::ProjectRepo remote_projects(remote_db);
  holder::git::RealGitOps remote_git;

  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Project";
  project.root_path = remote_dir.string();
  project.privacy_mode = "encrypted_git";
  project.created_at = 1;
  project.updated_at = 1;
  remote_projects.create(project);
  holder::privacy::ensure_encrypted_project_ready(
      remote_git,
      remote_projects,
      project_id,
      project.root_path,
      std::nullopt,
      1,
      []() { return "test-key-1"; }
  );
  const auto seeded_project = remote_projects.get(project_id).value();

  holder::model::Card card;
  card.card_id = "shared-card";
  card.project_id = project_id;
  card.title = "Shared";
  holder::card::CardStore(remote_db, &remote_fts, nullptr, &remote_git).create(card, "base");

  // Local clones that base state.
  holder::git::RealGitOps local_git;
  local_git.open_or_init(local_dir);
  local_git.set_remote("origin", remote_dir.string());
  local_git.pull_remote_ff_only("origin");

  // Diverge: remote edits the card...
  holder::card::CardStore(remote_db, &remote_fts, nullptr, &remote_git)
      .update_content("shared-card", "remote edit", std::nullopt, 2);

  // ...and local independently edits the same card via its own DB row -- same project_id/
  // project_key_id, so decryption works identically, exactly the real cross-device scenario.
  auto local_db = open_db_with_schema(dir / "local.db");
  holder::index::FtsIndexer local_fts(local_db);
  holder::project::ProjectRepo local_projects(local_db);
  holder::model::Project local_project = seeded_project;
  local_project.root_path = local_dir.string();
  local_projects.create(local_project);
  holder::store::Rebuilder(local_db, &local_fts).rebuild_project(local_project);
  holder::card::CardStore(local_db, &local_fts, nullptr, &local_git)
      .update_content("shared-card", "local edit", std::nullopt, 2);

  bool diverged_seen = false;
  holder::git::NonFastForwardPullError diverged("", "", "");
  try {
    local_git.pull_remote_ff_only("origin");
  } catch (const holder::git::NonFastForwardPullError& e) {
    diverged_seen = true;
    diverged = e;
  }
  REQUIRE(diverged_seen);

  const int resolved = holder::sync::resolve_pull_conflicts(
      local_db,
      &local_fts,
      local_project,
      local_git,
      diverged,
      3,
      []() { return "conflicted-copy-id"; }
  );
  REQUIRE(resolved == 1);

  const auto duplicate = holder::card::CardRepo(local_db).get("conflicted-copy-id");
  REQUIRE(duplicate.has_value());
  REQUIRE(duplicate->title == "Shared (conflicted copy)");

  holder::card::CardStore local_store(local_db, &local_fts, nullptr, &local_git);
  const auto content = local_store.get_content(duplicate.value());
  REQUIRE(content.has_value());
  REQUIRE(*content == "local edit");
}

TEST_CASE(
    "resolve_pull_conflicts skips a conflict whose pre-merge blob can't be decrypted",
    "[sync]"
) {
  // Same divergence shape as the test above, but the local side's pre-merge blob is corrupted
  // (not a real encrypted envelope) -- resolve_pull_conflicts must skip it rather than throw and
  // fail the whole pull over one unreadable conflict.
  const auto dir = make_temp_dir();
  EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  const std::string project_id = "proj-1";
  const auto remote_dir = dir / "remote";
  const auto local_dir = dir / "local";

  auto remote_db = open_db_with_schema(dir / "remote.db");
  holder::index::FtsIndexer remote_fts(remote_db);
  holder::project::ProjectRepo remote_projects(remote_db);
  holder::git::RealGitOps remote_git;

  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Project";
  project.root_path = remote_dir.string();
  project.privacy_mode = "encrypted_git";
  project.created_at = 1;
  project.updated_at = 1;
  remote_projects.create(project);
  holder::privacy::ensure_encrypted_project_ready(
      remote_git,
      remote_projects,
      project_id,
      project.root_path,
      std::nullopt,
      1,
      []() { return "test-key-1"; }
  );
  const auto seeded_project = remote_projects.get(project_id).value();

  holder::model::Card card;
  card.card_id = "shared-card";
  card.project_id = project_id;
  card.title = "Shared";
  holder::card::CardStore(remote_db, &remote_fts, nullptr, &remote_git).create(card, "base");

  holder::git::RealGitOps local_git;
  local_git.open_or_init(local_dir);
  local_git.set_remote("origin", remote_dir.string());
  local_git.pull_remote_ff_only("origin");

  remote_git.write_file(
      holder::core::card_rel_path("shared-card"),
      "remote edit -- doesn't matter, never decrypted in this test"
  );
  remote_git.stage_path(holder::core::card_rel_path("shared-card"));
  remote_git.commit("remote edit");

  // Corrupt the local side's copy directly, in the working tree, then commit it -- this is what
  // makes the (still real, git-level) divergence's local content undecryptable.
  local_git.write_file(holder::core::card_rel_path("shared-card"), "not a real encrypted envelope");
  local_git.stage_path(holder::core::card_rel_path("shared-card"));
  local_git.commit("local corruption");

  auto local_db = open_db_with_schema(dir / "local.db");
  holder::index::FtsIndexer local_fts(local_db);
  holder::project::ProjectRepo local_projects(local_db);
  holder::model::Project local_project = seeded_project;
  local_project.root_path = local_dir.string();
  local_projects.create(local_project);

  bool diverged_seen = false;
  holder::git::NonFastForwardPullError diverged("", "", "");
  try {
    local_git.pull_remote_ff_only("origin");
  } catch (const holder::git::NonFastForwardPullError& e) {
    diverged_seen = true;
    diverged = e;
  }
  REQUIRE(diverged_seen);

  const int resolved = holder::sync::resolve_pull_conflicts(
      local_db,
      &local_fts,
      local_project,
      local_git,
      diverged,
      3,
      []() { return "conflicted-copy-id"; }
  );
  REQUIRE(resolved == 0);
  REQUIRE_FALSE(holder::card::CardRepo(local_db).get("conflicted-copy-id").has_value());
}

TEST_CASE("resolve_pull_conflicts skips a conflict whose duplicate card_id collides", "[sync]") {
  const auto dir = make_temp_dir();
  const std::string project_id = "proj-1";
  const auto remote_dir = dir / "remote";
  const auto local_dir = dir / "local";

  auto remote_db = open_db_with_schema(dir / "remote.db");
  holder::index::FtsIndexer remote_fts(remote_db);
  holder::project::ProjectRepo remote_projects(remote_db);
  holder::git::RealGitOps remote_git;
  create_plain_project(remote_projects, project_id, remote_dir);

  holder::model::Card card;
  card.card_id = "shared-card";
  card.project_id = project_id;
  card.title = "Shared";
  holder::card::CardStore(remote_db, &remote_fts, nullptr, &remote_git).create(card, "base");

  holder::git::RealGitOps local_git;
  local_git.open_or_init(local_dir);
  local_git.set_remote("origin", remote_dir.string());
  local_git.pull_remote_ff_only("origin");

  holder::card::CardStore(remote_db, &remote_fts, nullptr, &remote_git)
      .update_content("shared-card", "remote edit", std::nullopt, 2);

  auto local_db = open_db_with_schema(dir / "local.db");
  holder::index::FtsIndexer local_fts(local_db);
  holder::project::ProjectRepo local_projects(local_db);
  create_plain_project(local_projects, project_id, local_dir);
  const auto local_project = local_projects.get(project_id).value();
  holder::store::Rebuilder(local_db, &local_fts).rebuild_project(local_project);
  holder::card::CardStore(local_db, &local_fts, nullptr, &local_git)
      .update_content("shared-card", "local edit", std::nullopt, 2);

  bool diverged_seen = false;
  holder::git::NonFastForwardPullError diverged("", "", "");
  try {
    local_git.pull_remote_ff_only("origin");
  } catch (const holder::git::NonFastForwardPullError& e) {
    diverged_seen = true;
    diverged = e;
  }
  REQUIRE(diverged_seen);

  // Force a card_id collision: "uuid_v4" hands back the id of a card that already exists.
  const int resolved = holder::sync::resolve_pull_conflicts(
      local_db,
      &local_fts,
      local_project,
      local_git,
      diverged,
      3,
      []() { return "shared-card"; }
  );
  REQUIRE(resolved == 0);
}
