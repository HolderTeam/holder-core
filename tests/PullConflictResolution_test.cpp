#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
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

#include <algorithm>
#include <fstream>

namespace {

using holder::test::EnvGuard;
using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;

constexpr const char* kSharedCardId = "550e8400-e29b-41d4-a716-446655440000";

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
  card.card_id = kSharedCardId;
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
      .update_content(kSharedCardId, "remote edit", std::nullopt, 2);

  // ...and local independently edits the same card via its own DB row -- same project_id/
  // project_key_id, so decryption works identically, exactly the real cross-device scenario.
  auto local_db = open_db_with_schema(dir / "local.db");
  holder::index::FtsIndexer local_fts(local_db);
  holder::project::ProjectRepo local_projects(local_db);
  holder::model::Project local_project = seeded_project;
  local_project.root_path = local_dir.string();
  local_project.id_scheme = holder::model::IdScheme::Uuid7;
  local_projects.create(local_project);
  holder::store::Rebuilder(local_db, &local_fts).rebuild_project(local_project);
  holder::card::CardStore(local_db, &local_fts, nullptr, &local_git)
      .update_content(kSharedCardId, "local edit", std::nullopt, 2);

  bool diverged_seen = false;
  holder::git::NonFastForwardPullError diverged("", "", "");
  try {
    local_git.pull_remote_ff_only("origin");
  } catch (const holder::git::NonFastForwardPullError& e) {
    diverged_seen = true;
    diverged = e;
  }
  REQUIRE(diverged_seen);

  auto stale_project = local_project;
  stale_project.id_scheme = holder::model::IdScheme::Uuid4;
  const int resolved = holder::sync::resolve_pull_conflicts(
      local_db, &local_fts, stale_project, local_git, diverged, 3
  );
  REQUIRE(resolved == 1);

  const auto cards = holder::card::CardRepo(local_db).list_all(project_id);
  const auto duplicate = std::find_if(cards.begin(), cards.end(), [](const auto& candidate) {
    return candidate.card_id != kSharedCardId;
  });
  REQUIRE(duplicate != cards.end());
  REQUIRE(duplicate->card_id.size() == 36);
  REQUIRE(duplicate->card_id[14] == '7');
  REQUIRE(duplicate->title == "Shared (conflicted copy)");

  holder::card::CardStore local_store(local_db, &local_fts, nullptr, &local_git);
  const auto content = local_store.get_content(*duplicate);
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
  card.card_id = kSharedCardId;
  card.project_id = project_id;
  card.title = "Shared";
  holder::card::CardStore(remote_db, &remote_fts, nullptr, &remote_git).create(card, "base");

  holder::git::RealGitOps local_git;
  local_git.open_or_init(local_dir);
  local_git.set_remote("origin", remote_dir.string());
  local_git.pull_remote_ff_only("origin");

  remote_git.write_file(
      holder::core::card_rel_path(kSharedCardId),
      "remote edit -- doesn't matter, never decrypted in this test"
  );
  remote_git.stage_path(holder::core::card_rel_path(kSharedCardId));
  remote_git.commit("remote edit");

  // Corrupt the local side's copy directly, in the working tree, then commit it -- this is what
  // makes the (still real, git-level) divergence's local content undecryptable.
  local_git.write_file(holder::core::card_rel_path(kSharedCardId), "not a real encrypted envelope");
  local_git.stage_path(holder::core::card_rel_path(kSharedCardId));
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
      local_db, &local_fts, local_project, local_git, diverged, 3
  );
  REQUIRE(resolved == 0);
}

TEST_CASE("resolve_pull_conflicts creates UUIDv4 copies for UUIDv4 projects", "[sync]") {
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
  card.card_id = kSharedCardId;
  card.project_id = project_id;
  card.title = "Shared";
  holder::card::CardStore(remote_db, &remote_fts, nullptr, &remote_git).create(card, "base");

  holder::git::RealGitOps local_git;
  local_git.open_or_init(local_dir);
  local_git.set_remote("origin", remote_dir.string());
  local_git.pull_remote_ff_only("origin");

  holder::card::CardStore(remote_db, &remote_fts, nullptr, &remote_git)
      .update_content(kSharedCardId, "remote edit", std::nullopt, 2);

  auto local_db = open_db_with_schema(dir / "local.db");
  holder::index::FtsIndexer local_fts(local_db);
  holder::project::ProjectRepo local_projects(local_db);
  create_plain_project(local_projects, project_id, local_dir);
  const auto local_project = local_projects.get(project_id).value();
  holder::store::Rebuilder(local_db, &local_fts).rebuild_project(local_project);
  holder::card::CardStore(local_db, &local_fts, nullptr, &local_git)
      .update_content(kSharedCardId, "local edit", std::nullopt, 2);

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
      local_db, &local_fts, local_project, local_git, diverged, 3
  );
  REQUIRE(resolved == 1);

  const auto cards = holder::card::CardRepo(local_db).list_all(project_id);
  const auto duplicate = std::find_if(cards.begin(), cards.end(), [](const auto& candidate) {
    return candidate.card_id != kSharedCardId;
  });
  REQUIRE(duplicate != cards.end());
  REQUIRE(duplicate->card_id.size() == 36);
  REQUIRE(duplicate->card_id[14] == '4');
}

TEST_CASE("resolve_pull_conflicts rejects a project that no longer exists", "[sync]") {
  const auto dir = make_temp_dir();
  auto db = open_db_with_schema(dir / "holder.db");
  holder::index::FtsIndexer fts(db);

  // The caller holds a Project value, but the DB row is gone (for example deleted by another
  // process while the pull was running), so there is no id scheme to mint copies with.
  holder::model::Project project;
  project.project_id = "proj-gone";
  project.name = "Gone";
  project.root_path = (dir / "repo").string();
  project.privacy_mode = "plain";
  holder::git::RealGitOps git;

  REQUIRE_THROWS_WITH(
      holder::sync::resolve_pull_conflicts(
          db, &fts, project, git, holder::git::NonFastForwardPullError("", "", ""), 3
      ),
      "project not found: proj-gone"
  );
}

TEST_CASE("resolve_pull_conflicts skips a conflict it cannot copy into the index", "[sync]") {
  // Same divergence as the UUIDv4 test above, but the project's recorded root is then pointed at
  // a regular file, so CardStore::create cannot open a repository there. One unwritable copy must
  // not fail the whole pull: the conflict is skipped and nothing is reported as resolved.
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
  card.card_id = kSharedCardId;
  card.project_id = project_id;
  card.title = "Shared";
  holder::card::CardStore(remote_db, &remote_fts, nullptr, &remote_git).create(card, "base");

  holder::git::RealGitOps local_git;
  local_git.open_or_init(local_dir);
  local_git.set_remote("origin", remote_dir.string());
  local_git.pull_remote_ff_only("origin");

  holder::card::CardStore(remote_db, &remote_fts, nullptr, &remote_git)
      .update_content(kSharedCardId, "remote edit", std::nullopt, 2);

  auto local_db = open_db_with_schema(dir / "local.db");
  holder::index::FtsIndexer local_fts(local_db);
  holder::project::ProjectRepo local_projects(local_db);
  create_plain_project(local_projects, project_id, local_dir);
  const auto local_project = local_projects.get(project_id).value();
  holder::store::Rebuilder(local_db, &local_fts).rebuild_project(local_project);
  holder::card::CardStore(local_db, &local_fts, nullptr, &local_git)
      .update_content(kSharedCardId, "local edit", std::nullopt, 2);

  bool diverged_seen = false;
  holder::git::NonFastForwardPullError diverged("", "", "");
  try {
    local_git.pull_remote_ff_only("origin");
  } catch (const holder::git::NonFastForwardPullError& e) {
    diverged_seen = true;
    diverged = e;
  }
  REQUIRE(diverged_seen);

  const auto not_a_directory = dir / "not-a-directory";
  std::ofstream(not_a_directory) << "plain file";
  holder::project::ProjectRepo(local_db).update_root_path(project_id, not_a_directory.string(), 3);

  int resolved = -1;
  REQUIRE_NOTHROW(
      resolved = holder::sync::resolve_pull_conflicts(
          local_db, &local_fts, local_project, local_git, diverged, 3
      )
  );
  REQUIRE(resolved == 0);
  const auto cards = holder::card::CardRepo(local_db).list_all(project_id);
  REQUIRE(cards.size() == 1);
  REQUIRE(cards.front().card_id == kSharedCardId);
}
