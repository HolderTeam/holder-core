#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "card/CardFrontMatter.h"
#include "card/CardHierarchy.h"
#include "card/CardMutation.h"
#include "card/CardPaths.h"
#include "card/CardStore.h"
#include "core_test_helpers.h"
#include "platform/Tx.h"
#include "privacy/ProjectPrivacy.h"
#include "project/Rebuilder.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <future>
#include <thread>

namespace {
const std::string ancestor_id = "00000000-0000-4000-8000-000000000001";
const std::string before_id = "00000000-0000-4000-8000-000000000002";
const std::string parent_id = "00000000-0000-4000-8000-000000000003";
const std::string after_id = "00000000-0000-4000-8000-000000000004";
const std::string child_z_id = "00000000-0000-4000-8000-000000000005";
const std::string child_a_id = "00000000-0000-4000-8000-000000000006";
const std::string child_b_id = "00000000-0000-4000-8000-000000000007";
const std::string grandchild_id = "00000000-0000-4000-8000-000000000008";
const std::string old_trash_id = "00000000-0000-4000-8000-000000000009";
const std::string child1_id = "00000000-0000-4000-8000-000000000010";
const std::string child2_id = "00000000-0000-4000-8000-000000000011";
const std::string child_id = "00000000-0000-4000-8000-000000000012";
const std::string new_child_id = "00000000-0000-4000-8000-000000000013";
const std::string missing_id = "00000000-0000-4000-8000-000000000014";
class FaultGit final : public holder::git::GitOps {
 public:
  holder::git::RealGitOps real;
  int writes_until_failure = 0;
  bool fail_stage = false;
  bool fail_commit = false;
  bool fail_after_commit = false;
  void open_or_init(const std::filesystem::path& p) override { real.open_or_init(p); }
  void write_file(const std::filesystem::path& p, const std::string& bytes) override {
    if (writes_until_failure && --writes_until_failure == 0)
      throw std::runtime_error("injected write failure");
    real.write_file(p, bytes);
  }
  void stage_path(const std::filesystem::path& p) override {
    if (fail_stage) throw std::runtime_error("injected stage failure");
    real.stage_path(p);
  }
  void remove_path(const std::filesystem::path& p) override { real.remove_path(p); }
  void commit(const std::string& message) override {
    if (fail_commit) throw std::runtime_error("injected commit failure");
    real.commit(message);
    if (fail_after_commit) throw std::runtime_error("injected post-commit failure");
  }
  void set_remote(const std::string& n, const std::string& u) override { real.set_remote(n, u); }
  void remove_remote(const std::string& n) override { real.remove_remote(n); }
  void pull_remote_ff_only(const std::string& n) override { real.pull_remote_ff_only(n); }
  holder::git::RemoteProbeResult probe_remote(const std::string& n) override {
    return real.probe_remote(n);
  }
  holder::git::PushResult push_branch(const std::string& n, const std::string& b, bool u) override {
    return real.push_branch(n, b, u);
  }
  std::filesystem::path repo_dir() const override { return real.repo_dir(); }
};

struct Fixture {
  std::filesystem::path dir = holder::test::make_temp_dir();
  holder::platform::Db db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::index::FtsIndexer fts{db};
  FaultGit git;
  holder::card::CardStore store{db, &fts, nullptr, &git};
  holder::card::CardRepo cards{db};
  holder::model::Project project;
  holder::core::RealFs fs;
  Fixture() {
    project.project_id = "project-lifecycle";
    project.name = "Lifecycle";
    project.root_path = (dir / "repo").string();
    project.privacy_mode = "plain";
    project.created_at = project.updated_at = 1;
    holder::project::ProjectRepo(db).create(project);
  }
  void add(
      const std::string& id,
      std::optional<std::string> parent = {},
      double key = 1.0,
      const std::string& title = ""
  ) {
    holder::model::Card card;
    card.card_id = id;
    card.project_id = project.project_id;
    card.title = title.empty() ? id : title;
    card.parent_card_id = parent;
    card.created_at = card.updated_at = 1;
    store.create(card, "body " + id + "\n#keep\n", key);
  }
  std::string bytes(const std::string& id, bool trash = false) {
    return fs.read_file(
        std::filesystem::path(project.root_path) /
        (trash ? holder::core::card_trash_rel_path(id) : holder::core::card_rel_path(id))
    );
  }
  std::vector<std::string> order(std::optional<std::string> parent = {}) {
    auto all = parent ? cards.list_children(project.project_id, *parent)
                      : cards.list_roots(project.project_id);
    all.erase(
        std::remove_if(
            all.begin(),
            all.end(),
            [](const auto& c) {
              return c.deleted_at.has_value();
            }
        ),
        all.end()
    );
    std::sort(all.begin(), all.end(), holder::card::card_tree_less);
    std::vector<std::string> result;
    for (const auto& c : all)
      result.push_back(c.card_id);
    return result;
  }
  std::string head() {
    holder::git::GitRepo repo;
    repo.open_existing(project.root_path);
    return *repo.head_oid();
  }
  void rebuild() { holder::store::Rebuilder(db, &fts).rebuild_project(project); }
};
} // namespace

TEST_CASE(
    "Trash replaces root or nested parent with its ordered live children",
    "[cardstore][lifecycle]"
) {
  Fixture f;
  std::optional<std::string> ancestor;
  SECTION("root") {}
  SECTION("nested") {
    f.add(ancestor_id);
    ancestor = ancestor_id;
  }
  f.add(before_id, ancestor, 1);
  f.add(parent_id, ancestor, 2);
  f.add(after_id, ancestor, 3);
  f.add(child_z_id, parent_id, 1, "Zulu");
  f.add(child_a_id, parent_id, 1, "alpha");
  f.add(child_b_id, parent_id, 1, "Alpha");
  f.add(grandchild_id, child_a_id);
  f.add(old_trash_id, parent_id);
  f.store.trash(old_trash_id, 5);
  const auto old_trash = f.bytes(old_trash_id, true);
  const auto grandchild = f.bytes(grandchild_id);
  const auto before_head = f.head();
  f.store.trash(parent_id, 10);
  REQUIRE(
      f.order(ancestor) ==
      std::vector<std::string>{before_id, child_a_id, child_b_id, child_z_id, after_id}
  );
  REQUIRE(f.cards.get(child_a_id)->parent_card_id == ancestor);
  REQUIRE(f.cards.get(grandchild_id)->parent_card_id == child_a_id);
  REQUIRE(f.bytes(grandchild_id) == grandchild);
  REQUIRE(f.bytes(old_trash_id, true) == old_trash);
  holder::git::GitRepo history;
  history.open_existing(f.project.root_path);
  REQUIRE(history.commit_parent_oids(f.head()) == std::vector<std::string>{before_head});
  for (const auto& id : {child_a_id, child_b_id, child_z_id}) {
    const auto file = holder::core::parse_card_file(f.bytes(id));
    REQUIRE(file.card.parent_card_id == ancestor);
    REQUIRE(file.card.updated_at == 10);
    REQUIRE(file.body == "body " + std::string(id) + "\n#keep\n");
    REQUIRE_FALSE(holder::card::TagRepo(f.db).list_tags_for_card(f.project.project_id, id).empty());
  }
  f.rebuild();
  REQUIRE(
      f.order(ancestor) ==
      std::vector<std::string>{before_id, child_a_id, child_b_id, child_z_id, after_id}
  );
  f.store.restore(parent_id, 11);
  REQUIRE(f.cards.get(child_a_id)->parent_card_id == ancestor);
  REQUIRE(f.order(parent_id).empty());
}

TEST_CASE(
    "Trash rebalances dense sibling keys without changing relative order",
    "[cardstore][lifecycle]"
) {
  Fixture f;
  const double left = 1e100;
  f.add(before_id, {}, left);
  f.add(parent_id, {}, left, "zz-parent");
  f.add(after_id, {}, std::nextafter(left, INFINITY));
  f.add(child1_id, parent_id, 1);
  f.add(child2_id, parent_id, 2);
  f.store.trash(parent_id, 20);
  REQUIRE(f.order() == std::vector<std::string>{before_id, child1_id, child2_id, after_id});
  REQUIRE(f.cards.get(before_id)->sort_key == 1);
  REQUIRE(f.cards.get(after_id)->sort_key == 4);
  f.rebuild();
  REQUIRE(f.order() == std::vector<std::string>{before_id, child1_id, child2_id, after_id});
}

TEST_CASE(
    "Restore chooses a reachable ancestor and never reclaims promoted children",
    "[cardstore][lifecycle]"
) {
  Fixture f;
  f.add(ancestor_id);
  f.add(parent_id, ancestor_id);
  f.add(child_id, parent_id);
  f.store.trash(child_id, 5);
  f.store.trash(parent_id, 6);
  SECTION("nearest live ancestor") {
    f.store.restore(child_id, 7);
    REQUIRE(f.cards.get(child_id)->parent_card_id == ancestor_id);
    f.store.restore(parent_id, 8);
    REQUIRE(f.cards.get(child_id)->parent_card_id == ancestor_id);
  }
  SECTION("missing parent falls back to roots") {
    f.store.hard_delete(parent_id);
    f.store.restore(child_id, 7);
    REQUIRE_FALSE(f.cards.get(child_id)->parent_card_id);
  }
  SECTION("no live ancestor falls back to roots") {
    f.store.trash(ancestor_id, 7);
    f.store.restore(child_id, 8);
    REQUIRE_FALSE(f.cards.get(child_id)->parent_card_id);
  }
  f.rebuild();
  REQUIRE_FALSE(f.cards.get(child_id)->deleted_at);
}

TEST_CASE(
    "Historical lifecycle restore promotes current children and validates old parents",
    "[cardstore][lifecycle][history]"
) {
  Fixture f;
  f.add(ancestor_id);
  f.add(parent_id, ancestor_id);
  const auto live = f.head();
  f.store.trash(parent_id, 2);
  const auto trash = f.head();
  f.store.restore(parent_id, 3);
  f.add(child_id, parent_id);
  f.store.restore_version(parent_id, trash, 4);
  REQUIRE(f.cards.get(child_id)->parent_card_id == ancestor_id);
  REQUIRE(f.cards.get(parent_id)->deleted_at == 4);
  f.store.trash(ancestor_id, 5);
  f.store.restore_version(parent_id, live, 6);
  REQUIRE_FALSE(f.cards.get(parent_id)->parent_card_id);
  REQUIRE_FALSE(f.cards.get(parent_id)->deleted_at);
  REQUIRE_FALSE(f.cards.get(child_id)->parent_card_id);
  f.rebuild();
  REQUIRE_FALSE(f.cards.get(parent_id)->parent_card_id);
}

TEST_CASE(
    "Lifecycle errors preserve files projections Git history and unrelated staged work",
    "[cardstore][lifecycle]"
) {
  Fixture f;
  f.add(parent_id);
  f.add(child_id, parent_id);
  f.git.real.write_file("unrelated.txt", "staged version");
  f.git.real.stage_path("unrelated.txt");
  f.git.real.write_file("unrelated.txt", "working version");
  const auto parent = f.bytes(parent_id);
  const auto child = f.bytes(child_id);
  const auto index = f.fs.read_file(std::filesystem::path(f.project.root_path) / ".git/index");
  const auto head = f.head();
  SECTION("child write fails after parent rename and write") { f.git.writes_until_failure = 2; }
  SECTION("staging fails") { f.git.fail_stage = true; }
  SECTION("commit fails") { f.git.fail_commit = true; }
  SECTION("failure after Git committed rolls back its reference") {
    f.git.fail_after_commit = true;
  }
  SECTION("SQLite rejects child placement") {
    f.db.exec(
        "CREATE TRIGGER reject_promotion BEFORE UPDATE ON cards WHEN OLD.card_id = '00000000-0000-4000-8000-000000000012' BEGIN SELECT RAISE(ABORT, 'injected SQL failure'); END;"
    );
  }
  SECTION("missing child fails preflight") {
    f.fs.remove(std::filesystem::path(f.project.root_path) / holder::core::card_rel_path(child_id));
  }
  REQUIRE_THROWS(f.store.trash(parent_id, 10));
  REQUIRE_FALSE(f.cards.get(parent_id)->deleted_at);
  REQUIRE(f.cards.get(child_id)->parent_card_id == parent_id);
  REQUIRE(f.bytes(parent_id) == parent);
  if (f.fs.exists(
          std::filesystem::path(f.project.root_path) / holder::core::card_rel_path(child_id)
      ))
    REQUIRE(f.bytes(child_id) == child);
  REQUIRE(f.head() == head);
  REQUIRE(f.fs.read_file(std::filesystem::path(f.project.root_path) / ".git/index") == index);
  REQUIRE_FALSE(holder::card::CardMutation::pending(f.project.root_path));
}

TEST_CASE(
    "Successful promotion commits only affected files and preserves the staged index",
    "[cardstore][lifecycle]"
) {
  Fixture f;
  f.add(parent_id);
  f.add(child_id, parent_id);
  f.git.real.write_file("unrelated.txt", "staged version");
  f.git.real.stage_path("unrelated.txt");
  f.git.real.write_file("unrelated.txt", "working version");
  f.store.trash(parent_id, 10);
  holder::git::GitRepo history;
  history.open_existing(f.project.root_path);
  REQUIRE_FALSE(history.read_blob_at(f.head(), "unrelated.txt"));
  REQUIRE(
      f.fs.read_file(std::filesystem::path(f.project.root_path) / "unrelated.txt") ==
      "working version"
  );
  // Committing the remaining index must contain its staged bytes, not the worktree.
  f.git.real.commit("Commit unrelated staged work");
  REQUIRE(history.read_blob_at(f.head(), "unrelated.txt") == "staged version");
}

TEST_CASE(
    "Interrupted lifecycle journals settle by whether the operation's commit landed",
    "[cardstore][lifecycle]"
) {
  Fixture f;
  f.add(parent_id);
  f.add(child_id, parent_id);
  const std::filesystem::path root = f.project.root_path;
  const auto original = f.bytes(parent_id);
  const auto head = f.head();
  const auto live_path = holder::core::card_rel_path(parent_id);
  const auto trash_path = holder::core::card_trash_rel_path(parent_id);
  const auto commit_trash = [&] {
    f.git.real.remove_path(live_path);
    f.git.real.stage_path(trash_path);
    f.git.real.commit("Interrupted lifecycle operation");
  };
  const auto commit_unrelated = [&] {
    f.git.real.write_file("unrelated.txt", "later work");
    f.git.real.stage_path("unrelated.txt");
    f.git.real.commit("Later unrelated work");
  };
  bool landed = false;
  {
    holder::card::CardMutation mutation(
        f.fs,
        f.project.root_path,
        {live_path, trash_path, holder::core::card_rel_path(child_id)},
        {{live_path, std::nullopt}, {trash_path, original}}
    );
    mutation.begin();
    f.fs.create_directories((root / trash_path).parent_path());
    f.fs.rename(root / live_path, root / trash_path);
    f.cards.soft_delete(parent_id, 10, 10);
    SECTION("interrupted before Git commit") {}
    SECTION("interrupted before Git commit, then another commit is made") {
      // The real operation never committed; only its own uncommitted bytes are undone.
      f.git.real.open_or_init(root);
      commit_unrelated();
    }
    SECTION("interrupted after Git commit") {
      f.git.real.open_or_init(root);
      commit_trash();
      landed = true;
    }
    SECTION("interrupted after Git commit, then another commit is made") {
      f.git.real.open_or_init(root);
      commit_trash();
      commit_unrelated();
      landed = true;
    }
    // Deliberately leave the durable journal, as abrupt termination would.
  }
  const auto head_after_interruption = f.head();
  REQUIRE(holder::card::CardMutation::pending(f.project.root_path));
  // Metadata reads stay plain SQLite lookups and do not trigger recovery.
  REQUIRE(f.store.get(parent_id)->deleted_at);
  REQUIRE(holder::card::CardMutation::pending(f.project.root_path));
  REQUIRE(f.store.get_content(*f.cards.get(child_id)) == "body " + child_id + "\n#keep\n");
  REQUIRE_FALSE(holder::card::CardMutation::pending(f.project.root_path));
  // Recovery never moves the branch.
  REQUIRE(f.head() == head_after_interruption);
  if (landed) {
    REQUIRE(f.cards.get(parent_id)->deleted_at);
    REQUIRE(f.bytes(parent_id, true) == original);
    REQUIRE_FALSE(f.fs.exists(root / live_path));
  } else {
    REQUIRE_FALSE(f.cards.get(parent_id)->deleted_at);
    REQUIRE(f.bytes(parent_id) == original);
    REQUIRE_FALSE(f.fs.exists(root / trash_path));
  }
  REQUIRE(f.cards.get(child_id)->parent_card_id == parent_id);
  f.rebuild();
  REQUIRE(static_cast<bool>(f.cards.get(parent_id)->deleted_at) == landed);
}

TEST_CASE(
    "Rebuilding inside an import transaction does not consume a recovery journal",
    "[cardstore][lifecycle]"
) {
  Fixture f;
  f.add(parent_id);
  const std::filesystem::path root = f.project.root_path;
  const auto live_path = holder::core::card_rel_path(parent_id);
  const auto trash_path = holder::core::card_trash_rel_path(parent_id);
  const auto original = f.bytes(parent_id);
  {
    holder::card::CardMutation mutation(
        f.fs,
        f.project.root_path,
        {live_path, trash_path},
        {{live_path, std::nullopt}, {trash_path, original}}
    );
    mutation.begin();
    f.fs.create_directories((root / trash_path).parent_path());
    f.fs.rename(root / live_path, root / trash_path);
  }
  {
    holder::platform::Tx tx(f.db);
    holder::store::Rebuilder(f.db, &f.fts).rebuild_project_in_transaction(f.project);
    tx.commit();
  }
  // The caller owns recovery: files are untouched and the journal remains for it.
  REQUIRE(holder::card::CardMutation::pending(f.project.root_path));
  REQUIRE(f.fs.exists(root / trash_path));
  f.rebuild();
  REQUIRE_FALSE(holder::card::CardMutation::pending(f.project.root_path));
  REQUIRE(f.bytes(parent_id) == original);
  REQUIRE_FALSE(f.cards.get(parent_id)->deleted_at);
}

TEST_CASE(
    "Core create move and lifecycle reject invalid ancestor chains",
    "[cardstore][lifecycle]"
) {
  Fixture f;
  f.add(parent_id);
  f.add(child_id, parent_id);
  REQUIRE_THROWS_WITH(f.store.move(parent_id, true, child_id, {}, 2), "move_would_create_cycle");
  f.store.trash(parent_id, 3);
  REQUIRE_THROWS_WITH(f.add(new_child_id, parent_id), "target_not_found");
  REQUIRE_THROWS_WITH(f.store.move(child_id, true, parent_id, {}, 4), "target_not_found");
  REQUIRE_THROWS_WITH(f.add(new_child_id, missing_id), "target_not_found");
  f.db.exec(
      "UPDATE cards SET parent_card_id = '00000000-0000-4000-8000-000000000012' WHERE card_id = '00000000-0000-4000-8000-000000000012'"
  );
  const auto head = f.head();
  REQUIRE_THROWS_WITH(f.store.trash(child_id, 5), "move_would_create_cycle");
  REQUIRE(f.head() == head);
}

TEST_CASE(
    "Encrypted promotion and restore rebuild with all children still reachable",
    "[cardstore][lifecycle][privacy]"
) {
  Fixture f;
  holder::test::EnvGuard keystore("HOLDER_TEST_KEYSTORE_DIR", (f.dir / "keystore").string());
  holder::project::ProjectRepo projects(f.db);
  f.db.exec(
      "UPDATE projects SET privacy_mode = 'encrypted_git' WHERE project_id = 'project-lifecycle'"
  );
  holder::privacy::ensure_encrypted_project_ready(
      f.git.real,
      projects,
      f.project.project_id,
      f.project.root_path,
      {},
      2,
      [] {
        return "lifecycle-key";
      }
  );
  f.project = *projects.get(f.project.project_id);
  f.add(parent_id);
  f.add(child_id, parent_id);
  f.store.trash(parent_id, 10);
  REQUIRE(f.bytes(parent_id, true).find("body parent") == std::string::npos);
  REQUIRE(f.bytes(child_id).find("body child") == std::string::npos);
  f.rebuild();
  REQUIRE_FALSE(f.cards.get(child_id)->parent_card_id);
  f.store.restore(parent_id, 11);
  REQUIRE(f.store.get_content(*f.cards.get(child_id)) == "body " + child_id + "\n#keep\n");
  REQUIRE_FALSE(f.cards.get(child_id)->parent_card_id);
}

TEST_CASE(
    "Create and move waiting behind trash validate the committed hierarchy",
    "[cardstore][lifecycle][concurrency]"
) {
  Fixture f;
  f.add(parent_id);
  f.add(child_id);
  std::promise<void> ready;
  auto ready_future = ready.get_future();
  std::future<std::string> worker;
  bool create = false;
  SECTION("create") { create = true; }
  SECTION("move") {}
  {
    auto lock = f.git.lock_operation(f.project.root_path);
    worker = std::async(std::launch::async, [&] {
      holder::platform::Db db;
      db.open(f.dir / "holder.db");
      holder::index::FtsIndexer fts(db);
      holder::card::CardStore store(db, &fts);
      ready.set_value();
      try {
        if (create) {
          holder::model::Card card;
          card.card_id = new_child_id;
          card.project_id = f.project.project_id;
          card.parent_card_id = parent_id;
          card.title = "new child";
          card.created_at = card.updated_at = 5;
          store.create(card, "body");
        } else {
          store.move(child_id, true, parent_id, {}, 5);
        }
        return std::string("unexpected success");
      } catch (const std::exception& ex) {
        return std::string(ex.what());
      }
    });
    ready_future.wait();
    // The worker can run only after the trash's whole project operation finishes.
    f.store.trash(parent_id, 4);
  }
  REQUIRE(worker.get() == "target_not_found");
  REQUIRE_FALSE(f.cards.get(child_id)->parent_card_id);
  REQUIRE_FALSE(f.cards.get(new_child_id));
}

TEST_CASE(
    "Promotion preserves child links milestones content and search visibility",
    "[cardstore][lifecycle]"
) {
  Fixture f;
  f.add(parent_id);
  f.add(child_id, parent_id);
  holder::card::LinkRepo links(f.db);
  holder::model::CardLink link;
  link.project_id = f.project.project_id;
  link.from_card_id = child_id;
  link.to_card_id = parent_id;
  link.to_type = "card";
  link.kind = "wiki";
  link.created_at = 2;
  links.upsert_links(f.project.project_id, child_id, {link});
  f.store.update_links(child_id, 2);
  holder::card::MilestoneRepo milestones(f.db);
  holder::model::Milestone milestone;
  milestone.milestone_id = "milestone-child";
  milestone.project_id = f.project.project_id;
  milestone.card_id = child_id;
  milestone.start_at = 100;
  milestone.created_at = milestone.updated_at = 2;
  milestones.replace_for_card(f.project.project_id, child_id, {milestone});
  f.store.update_milestones(child_id, 2);
  f.store.trash(parent_id, 3);
  for (int pass = 0; pass < 2; ++pass) {
    const auto outgoing = links.list_outgoing(f.project.project_id, child_id);
    REQUIRE(outgoing.size() == 1);
    REQUIRE(outgoing[0].to_card_id == parent_id);
    const auto dates = milestones.list_for_card(f.project.project_id, child_id);
    REQUIRE(dates.size() == 1);
    REQUIRE(dates[0].milestone_id == milestone.milestone_id);
    REQUIRE(dates[0].updated_at == 2);
    REQUIRE(
        holder::card::TagRepo(f.db).list_card_ids_with_tag(f.project.project_id, "keep") ==
        std::vector<std::string>{child_id}
    );
    REQUIRE(f.fts.search_cards(f.project.project_id, "body", 10, 0).size() == 1);
    f.rebuild();
  }
}

TEST_CASE(
    "Restore resolves a saved sort collision without reclaiming or reordering live children",
    "[cardstore][lifecycle]"
) {
  Fixture f;
  f.add(before_id, {}, 1);
  f.add(parent_id, {}, 2);
  f.add(after_id, {}, 3);
  f.add(child_id, parent_id);
  f.store.trash(parent_id, 4);
  REQUIRE(f.cards.get(child_id)->sort_key == 2);
  f.store.restore(parent_id, 5);
  REQUIRE(f.order() == std::vector<std::string>{before_id, parent_id, child_id, after_id});
  REQUIRE(f.cards.get(parent_id)->sort_key > 1);
  REQUIRE(f.cards.get(parent_id)->sort_key < 2);
  REQUIRE(f.cards.get(child_id)->sort_key == 2);
  f.rebuild();
  REQUIRE(f.order() == std::vector<std::string>{before_id, parent_id, child_id, after_id});
}

TEST_CASE(
    "Lifecycle operations work in a rebuilt project whose Git repository has no commits",
    "[cardstore][lifecycle]"
) {
  Fixture f;
  f.add(before_id, {}, 1);
  f.add(parent_id, {}, 2);
  f.add(after_id, {}, 3);
  f.add(child_id, parent_id);
  const std::filesystem::path root = f.project.root_path;
  std::filesystem::remove_all(root / ".git");
  holder::git::GitRepo fresh;
  fresh.open_or_init(root);
  REQUIRE_FALSE(fresh.head_oid());
  f.rebuild();

  SECTION("trash promotes children in the first commit") {
    f.store.trash(parent_id, 10);
    REQUIRE(f.order() == std::vector<std::string>{before_id, child_id, after_id});
    REQUIRE_FALSE(holder::card::CardMutation::pending(f.project.root_path));
    holder::git::GitRepo history;
    history.open_existing(f.project.root_path);
    REQUIRE(history.commit_parent_oids(f.head()).empty());
    f.rebuild();
    REQUIRE(f.order() == std::vector<std::string>{before_id, child_id, after_id});
  }
  SECTION("a failed first commit leaves the branch unborn") {
    f.git.fail_commit = true;
    REQUIRE_THROWS_WITH(f.store.trash(parent_id, 10), "injected commit failure");
    REQUIRE_FALSE(holder::card::CardMutation::pending(f.project.root_path));
    holder::git::GitRepo history;
    history.open_existing(f.project.root_path);
    REQUIRE_FALSE(history.head_oid());
    REQUIRE_FALSE(f.cards.get(parent_id)->deleted_at);
    REQUIRE(f.cards.get(child_id)->parent_card_id == parent_id);
  }
  SECTION("an interrupted first commit is rolled back to an unborn branch") {
    f.git.fail_after_commit = true;
    REQUIRE_THROWS_WITH(f.store.trash(parent_id, 10), "injected post-commit failure");
    REQUIRE_FALSE(holder::card::CardMutation::pending(f.project.root_path));
    holder::git::GitRepo history;
    history.open_existing(f.project.root_path);
    REQUIRE_FALSE(history.head_oid());
    REQUIRE(f.cards.get(child_id)->parent_card_id == parent_id);
  }
  SECTION("a journal left before the first commit never discards another first commit") {
    const auto path = holder::core::card_rel_path(parent_id);
    const auto original = f.bytes(parent_id);
    {
      holder::card::CardMutation mutation(f.fs, f.project.root_path, {path}, {{path, "partial"}});
      mutation.begin();
      f.fs.write_file(root / path, "partial");
      // Deliberately leave the durable journal, as abrupt termination would.
    }
    f.git.real.open_or_init(root);
    f.git.real.write_file("unrelated.txt", "first");
    f.git.real.stage_path("unrelated.txt");
    f.git.real.commit("Unrelated first commit");
    const auto first = f.head();
    f.rebuild();
    REQUIRE_FALSE(holder::card::CardMutation::pending(f.project.root_path));
    REQUIRE(f.head() == first);
    REQUIRE(f.bytes(parent_id) == original);
    f.store.trash(parent_id, 10);
    REQUIRE(f.order() == std::vector<std::string>{before_id, child_id, after_id});
  }
  SECTION("a journal left after the first commit keeps that commit") {
    const auto path = holder::core::card_rel_path(child_id);
    const auto updated = f.bytes(child_id) + "\nupdated\n";
    {
      holder::card::CardMutation mutation(f.fs, f.project.root_path, {path}, {{path, updated}});
      mutation.begin();
      f.git.real.open_or_init(root);
      f.git.real.write_file(path, updated);
      f.git.real.stage_path(path);
      f.git.real.commit("Interrupted lifecycle operation");
      // Deliberately leave the durable journal, as abrupt termination would.
    }
    const auto first = f.head();
    f.rebuild();
    REQUIRE_FALSE(holder::card::CardMutation::pending(f.project.root_path));
    REQUIRE(f.head() == first);
    REQUIRE(f.bytes(child_id) == updated);
  }
}

TEST_CASE("Promotion re-spaces only the siblings around tied sort keys", "[cardstore][lifecycle]") {
  Fixture f;
  f.add(ancestor_id, {}, 1);
  f.add(child1_id, {}, 2);
  f.add(before_id, {}, 3, "a");
  f.add(parent_id, {}, 3, "b");
  f.add(after_id, {}, 3, "c");
  f.add(child2_id, {}, 5);
  f.add(grandchild_id, {}, 6);
  f.add(child_id, parent_id);
  f.store.trash(parent_id, 20);
  REQUIRE(
      f.order() == std::vector<std::string>{
                       ancestor_id,
                       child1_id,
                       before_id,
                       child_id,
                       after_id,
                       child2_id,
                       grandchild_id
                   }
  );
  for (const auto& [id, key] : std::vector<std::pair<std::string, double>>{
           {ancestor_id, 1},
           {child1_id, 2},
           {child2_id, 5},
           {grandchild_id, 6}
       }) {
    REQUIRE(f.cards.get(id)->sort_key == key);
    REQUIRE(f.cards.get(id)->updated_at == 1);
  }
  f.rebuild();
  REQUIRE(f.order()[3] == child_id);
}
