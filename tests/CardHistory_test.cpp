#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "card/CardFrontMatter.h"
#include "card/CardPaths.h"
#include "git/GitRepo.h"
#include "history/CardHistory.h"
#include "model/Card.h"
#include "model/Project.h"
#include "privacy/ProjectPrivacy.h"
#include "core_test_helpers.h"

#include <git2.h>

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace {

std::filesystem::path history_temp_dir() {
  const auto suffix = std::to_string(static_cast<unsigned long long>(
      std::chrono::steady_clock::now().time_since_epoch().count()
  ));
  const auto path = std::filesystem::temp_directory_path() / ("holder_history_test_" + suffix);
  std::filesystem::create_directories(path);
  return path;
}

std::string card_file(const std::string& card_id, const std::string& title, const std::string& body) {
  holder::model::Card card;
  card.card_id = card_id;
  card.project_id = "project-history";
  card.title = title;
  card.rel_path = holder::core::card_rel_path(card_id);
  card.created_at = 1;
  card.updated_at = 2;
  return holder::core::render_card_front_matter(card, {}, {}) + body;
}

void write_commit(
    holder::git::GitRepo& repo,
    const std::string& card_id,
    const std::string& title,
    const std::string& body,
    const std::string& message
) {
  const auto path = holder::core::card_rel_path(card_id);
  repo.write_file(path, card_file(card_id, title, body));
  repo.stage_path(path);
  repo.commit(message);
}

std::string commit_staged_at(
    const std::filesystem::path& root,
    const std::string& message,
    const std::string& author_name,
    const std::string& author_email,
    git_time_t committed_at,
    const std::vector<std::string>& parent_oids,
    const char* update_ref
) {
  git_repository* raw = nullptr;
  REQUIRE(git_repository_open(&raw, root.string().c_str()) == 0);
  git_index* index = nullptr;
  REQUIRE(git_repository_index(&index, raw) == 0);
  git_oid tree_oid{};
  REQUIRE(git_index_write_tree(&tree_oid, index) == 0);
  REQUIRE(git_index_write(index) == 0);
  git_index_free(index);
  git_tree* tree = nullptr;
  REQUIRE(git_tree_lookup(&tree, raw, &tree_oid) == 0);
  git_signature* signature = nullptr;
  REQUIRE(git_signature_new(&signature, author_name.c_str(), author_email.c_str(), committed_at, 0) == 0);

  std::vector<git_commit*> parents;
  std::vector<const git_commit*> parent_pointers;
  for (const auto& parent_oid_text : parent_oids) {
    git_oid parent_oid{};
    REQUIRE(git_oid_fromstr(&parent_oid, parent_oid_text.c_str()) == 0);
    git_commit* parent = nullptr;
    REQUIRE(git_commit_lookup(&parent, raw, &parent_oid) == 0);
    parents.push_back(parent);
    parent_pointers.push_back(parent);
  }

  git_oid commit_oid{};
  REQUIRE(git_commit_create(
      &commit_oid,
      raw,
      update_ref,
      signature,
      signature,
      nullptr,
      message.c_str(),
      tree,
      parent_pointers.size(),
      parent_pointers.empty() ? nullptr : parent_pointers.data()
  ) == 0);
  for (auto* parent : parents) git_commit_free(parent);
  git_signature_free(signature);
  git_tree_free(tree);
  git_repository_free(raw);
  return std::string(git_oid_tostr_s(&commit_oid));
}

std::string write_commit_at(
    holder::git::GitRepo& repo,
    const std::filesystem::path& root,
    const std::string& card_id,
    const std::string& title,
    const std::string& body,
    const std::string& message,
    const std::string& author_name,
    const std::string& author_email,
    git_time_t committed_at
) {
  const auto path = holder::core::card_rel_path(card_id);
  repo.write_file(path, card_file(card_id, title, body));
  repo.stage_path(path);
  const auto head_oid = repo.head_oid();
  return commit_staged_at(
      root,
      message,
      author_name,
      author_email,
      committed_at,
      head_oid.has_value() ? std::vector<std::string>{*head_oid} : std::vector<std::string>{},
      "HEAD"
  );
}

void write_encrypted_commit(
    holder::git::GitRepo& repo,
    const holder::model::Project& project,
    const std::string& card_id,
    const std::string& title,
    const std::string& body,
    const std::string& message
) {
  const auto path = holder::core::card_rel_path(card_id);
  const auto encrypted = holder::privacy::encrypt_project_blob(
      project.project_id, project.project_key_id.value(), card_file(card_id, title, body)
  );
  repo.write_file(path, encrypted);
  repo.stage_path(path);
  repo.commit(message);
}

} // namespace

TEST_CASE("Card history lists card-only commits and groups adjacent edits", "[history][git]") {
  const auto root = history_temp_dir();
  const std::string card_id = "abcd-history-card";
  holder::git::GitRepo repo;
  repo.open_or_init(root);
  write_commit(repo, card_id, "Knife", "First line\n", "Add card Knife");
  write_commit(repo, card_id, "Knife", "First line\nSecond line\n", "Update card Knife");
  write_commit(repo, card_id, "Knife", "First line\nSecond line revised\n", "Update card Knife");
  repo.write_file("unrelated.txt", "ignored");
  repo.stage_path("unrelated.txt");
  repo.commit("Unrelated project change");
  write_commit(repo, card_id, "Knife", "First line\nSecond line revised\nAttachment\n", "Update links for Knife");

  holder::model::Project project;
  project.project_id = "project-history";
  project.name = "History";
  project.root_path = root.string();
  project.privacy_mode = "plain";

  const auto page = holder::history::CardHistoryService().list(project, card_id);
  REQUIRE(page.head_oid.has_value());
  REQUIRE(page.entries.size() == 3);
  CHECK(page.entries[0].kind == "links");
  CHECK(page.entries[1].kind == "updated");
  CHECK(page.entries[1].commit_count == 2);
  REQUIRE(page.entries[1].saves.size() == 2);
  CHECK(page.entries[1].saves.front().oid == page.entries[1].first_oid);
  CHECK(page.entries[1].saves.back().oid == page.entries[1].last_oid);
  CHECK(page.entries[1].saves.front().parent_oids.size() == 1);
  CHECK(page.entries[2].kind == "created");
  CHECK(page.entries[2].summary == "Card created");
}

TEST_CASE("Card history splits editing sessions at author and time boundaries", "[history][git]") {
  const auto root = history_temp_dir();
  const std::string card_id = "abcd-session-boundaries";
  holder::git::GitRepo repo;
  repo.open_or_init(root);

  write_commit_at(repo, root, card_id, "Sessions", "One\n",
                  "Add card Sessions", "Alice", "alice@example.test", 1'000);
  write_commit_at(repo, root, card_id, "Sessions", "Two\n", "Update card Sessions",
                  "Alice", "alice@example.test", 1'100);
  write_commit_at(repo, root, card_id, "Sessions", "Three\n", "Update card Sessions",
                  "Bob", "bob@example.test", 1'200);
  write_commit_at(repo, root, card_id, "Sessions", "Four\n", "Update card Sessions",
                  "Bob", "bob@example.test", 1'300);

  holder::model::Project project;
  project.project_id = "project-history";
  project.root_path = root.string();
  project.privacy_mode = "plain";
  const auto author_page = holder::history::CardHistoryService().list(project, card_id);
  REQUIRE(author_page.entries.size() == 3);
  CHECK(author_page.entries[0].commit_count == 2);
  CHECK(author_page.entries[0].author_name == "Bob");
  CHECK(author_page.entries[1].commit_count == 1);
  CHECK(author_page.entries[1].author_name == "Alice");

  const auto gap_root = history_temp_dir();
  const std::string gap_card_id = "abcd-session-gap";
  holder::git::GitRepo gap_repo;
  gap_repo.open_or_init(gap_root);
  write_commit_at(gap_repo, gap_root, gap_card_id, "Gap", "One\n", "Add card Gap",
                  "Alice", "alice@example.test", 2'000);
  write_commit_at(gap_repo, gap_root, gap_card_id, "Gap", "Two\n", "Update card Gap",
                  "Alice", "alice@example.test", 2'100);
  write_commit_at(gap_repo, gap_root, gap_card_id, "Gap", "Three\n", "Update card Gap",
                  "Alice", "alice@example.test", 2'701);

  project.root_path = gap_root.string();
  const auto gap_page = holder::history::CardHistoryService().list(project, gap_card_id);
  REQUIRE(gap_page.entries.size() == 3);
  CHECK(gap_page.entries[0].commit_count == 1);
  CHECK(gap_page.entries[1].commit_count == 1);

  const auto span_root = history_temp_dir();
  const std::string span_card_id = "abcd-session-span";
  holder::git::GitRepo span_repo;
  span_repo.open_or_init(span_root);
  write_commit_at(span_repo, span_root, span_card_id, "Span", "One\n", "Add card Span",
                  "Alice", "alice@example.test", 3'000);
  write_commit_at(span_repo, span_root, span_card_id, "Span", "Two\n", "Update card Span",
                  "Alice", "alice@example.test", 3'100);
  write_commit_at(span_repo, span_root, span_card_id, "Span", "Three\n", "Update card Span",
                  "Alice", "alice@example.test", 3'700);
  write_commit_at(span_repo, span_root, span_card_id, "Span", "Four\n", "Update card Span",
                  "Alice", "alice@example.test", 4'300);
  write_commit_at(span_repo, span_root, span_card_id, "Span", "Five\n", "Update card Span",
                  "Alice", "alice@example.test", 4'900);
  write_commit_at(span_repo, span_root, span_card_id, "Span", "Six\n", "Update card Span",
                  "Alice", "alice@example.test", 5'500);

  project.root_path = span_root.string();
  const auto span_page = holder::history::CardHistoryService().list(project, span_card_id);
  REQUIRE(span_page.entries.size() == 3);
  CHECK(span_page.entries[0].commit_count == 4);
  CHECK(span_page.entries[1].commit_count == 1);
}

TEST_CASE("Card history preserves parent order when commit clocks move backwards", "[history][git]") {
  const auto root = history_temp_dir();
  const std::string card_id = "abcd-clock-skew";
  holder::git::GitRepo repo;
  repo.open_or_init(root);
  write_commit_at(repo, root, card_id, "Skew", "One\n", "Add card Skew",
                  "Alice", "alice@example.test", 1'000);
  write_commit_at(repo, root, card_id, "Skew", "Two\n", "Update card Skew",
                  "Alice", "alice@example.test", 1'200);
  write_commit_at(repo, root, card_id, "Skew", "Three\n", "Update card Skew",
                  "Alice", "alice@example.test", 900);
  const auto head_oid = repo.head_oid();
  REQUIRE(head_oid.has_value());

  holder::model::Project project;
  project.project_id = "project-history";
  project.root_path = root.string();
  project.privacy_mode = "plain";
  const auto page = holder::history::CardHistoryService().list(project, card_id);

  REQUIRE(page.entries.size() == 3);
  CHECK(page.entries[0].last_oid == *head_oid);
  CHECK(page.entries[0].ended_at == 900);
  CHECK(page.entries[0].parent_oids.size() == 1);
  CHECK(page.entries[0].parent_oids.front() == page.entries[1].last_oid);
  CHECK(page.entries[1].ended_at == 1'200);
  CHECK(page.entries[0].commit_count == 1);
  CHECK(page.entries[1].commit_count == 1);
}

TEST_CASE("Card history retains both merge parents and the resulting card state", "[history][git]") {
  const auto root = history_temp_dir();
  const std::string card_id = "abcd-merge-history";
  holder::git::GitRepo repo;
  repo.open_or_init(root);
  const auto base_oid = write_commit_at(
      repo, root, card_id, "Merge", "Base\n", "Add card Merge",
      "Alice", "alice@example.test", 1'000
  );
  const auto first_parent_oid = write_commit_at(
      repo, root, card_id, "Merge", "Main branch\n", "Update card Merge",
      "Alice", "alice@example.test", 1'100
  );

  const auto path = holder::core::card_rel_path(card_id);
  repo.write_file(path, card_file(card_id, "Merge", "Other branch\n"));
  repo.stage_path(path);
  const auto second_parent_oid = commit_staged_at(
      root,
      "Update card Merge",
      "Bob",
      "bob@example.test",
      1'150,
      {base_oid},
      nullptr
  );

  repo.write_file(path, card_file(card_id, "Merge", "Combined branches\n"));
  repo.stage_path(path);
  const auto merge_oid = commit_staged_at(
      root,
      "Merge branches for Merge",
      "Alice",
      "alice@example.test",
      1'200,
      {first_parent_oid, second_parent_oid},
      "HEAD"
  );

  holder::model::Project project;
  project.project_id = "project-history";
  project.root_path = root.string();
  project.privacy_mode = "plain";
  holder::history::CardHistoryService service;
  const auto page = service.list(project, card_id);

  REQUIRE(page.head_oid.has_value());
  CHECK(*page.head_oid == merge_oid);
  REQUIRE_FALSE(page.entries.empty());
  const auto& merge = page.entries.front();
  CHECK(merge.last_oid == merge_oid);
  CHECK(merge.kind == "merged");
  CHECK(merge.is_merge);
  REQUIRE(merge.parent_oids.size() == 2);
  CHECK(merge.parent_oids[0] == first_parent_oid);
  CHECK(merge.parent_oids[1] == second_parent_oid);
  CHECK(merge.commit_count == 1);

  const auto comparison = service.compare(project, card_id, first_parent_oid, merge_oid);
  CHECK(comparison.from.body == "Main branch\n");
  CHECK(comparison.to.body == "Combined branches\n");
}

TEST_CASE("Card history follows the same UUID through live and Trash paths", "[history][git]") {
  const auto root = history_temp_dir();
  const std::string card_id = "abcd-live-trash";
  holder::git::GitRepo repo;
  repo.open_or_init(root);
  const auto created_oid = write_commit_at(
      repo, root, card_id, "Trash", "Live version\n", "Add card Trash",
      "Alice", "alice@example.test", 1'000
  );

  const auto live_path = holder::core::card_rel_path(card_id);
  const auto trash_path = holder::core::card_trash_rel_path(card_id);
  repo.remove_path(live_path);
  repo.write_file(trash_path, card_file(card_id, "Trash", "Trashed version\n"));
  repo.stage_path(trash_path);
  const auto trashed_parent = repo.head_oid();
  REQUIRE(trashed_parent.has_value());
  const auto trashed_oid = commit_staged_at(
      root,
      "Delete card Trash",
      "Alice",
      "alice@example.test",
      1'100,
      {*trashed_parent},
      "HEAD"
  );

  repo.remove_path(trash_path);
  repo.write_file(live_path, card_file(card_id, "Trash", "Restored version\n"));
  repo.stage_path(live_path);
  const auto restored_parent = repo.head_oid();
  REQUIRE(restored_parent.has_value());
  const auto restored_oid = commit_staged_at(
      root,
      "Restore card Trash",
      "Alice",
      "alice@example.test",
      1'200,
      {*restored_parent},
      "HEAD"
  );

  holder::model::Project project;
  project.project_id = "project-history";
  project.root_path = root.string();
  project.privacy_mode = "plain";
  holder::history::CardHistoryService service;
  const auto page = service.list(project, card_id);

  REQUIRE(page.head_oid.has_value());
  CHECK(*page.head_oid == restored_oid);
  REQUIRE(page.entries.size() == 3);
  CHECK(page.entries[0].kind == "restored");
  CHECK(page.entries[0].last_oid == restored_oid);
  CHECK(page.entries[1].kind == "deleted");
  CHECK(page.entries[1].last_oid == trashed_oid);
  CHECK(page.entries[2].kind == "created");
  CHECK(page.entries[2].last_oid == created_oid);

  const auto trashed_to_restored = service.compare(project, card_id, trashed_oid, restored_oid);
  CHECK(trashed_to_restored.from.exists);
  CHECK(trashed_to_restored.from.body == "Trashed version\n");
  CHECK(trashed_to_restored.to.exists);
  CHECK(trashed_to_restored.to.body == "Restored version\n");
}

TEST_CASE("Card history compares a selected version with current HEAD", "[history][git]") {
  const auto root = history_temp_dir();
  const std::string card_id = "abcd-compare-card";
  holder::git::GitRepo repo;
  repo.open_or_init(root);
  write_commit(repo, card_id, "Knife", "One\nTwo\n", "Add card Knife");
  const auto old_oid = repo.head_oid();
  REQUIRE(old_oid.has_value());
  write_commit(repo, card_id, "Knife", "One\nTwo changed\nThree\n", "Update card Knife");

  holder::model::Project project;
  project.project_id = "project-history";
  project.root_path = root.string();
  project.privacy_mode = "plain";
  const auto comparison =
      holder::history::CardHistoryService().compare(project, card_id, old_oid);

  REQUIRE(comparison.from.exists);
  REQUIRE(comparison.to.exists);
  CHECK(comparison.from.body == "One\nTwo\n");
  CHECK(comparison.to.body == "One\nTwo changed\nThree\n");
  CHECK(std::any_of(comparison.lines.begin(), comparison.lines.end(), [](const auto& line) {
    return line.origin == '+' && line.text == "Three";
  }));
  CHECK(std::any_of(comparison.lines.begin(), comparison.lines.end(), [](const auto& line) {
    return line.origin == '-' && line.text == "Two";
  }));
}

TEST_CASE("Card history comparison includes title-only changes", "[history][git]") {
  const auto root = history_temp_dir();
  const std::string card_id = "abcd-title-card";
  holder::git::GitRepo repo;
  repo.open_or_init(root);
  write_commit(repo, card_id, "Old title", "Same body\n", "Add card Old title");
  const auto old_oid = repo.head_oid();
  REQUIRE(old_oid.has_value());
  write_commit(repo, card_id, "New title", "Same body\n", "Update card New title");

  holder::model::Project project;
  project.project_id = "project-history";
  project.root_path = root.string();
  project.privacy_mode = "plain";
  const auto comparison = holder::history::CardHistoryService().compare(project, card_id, old_oid);
  CHECK(std::any_of(comparison.lines.begin(), comparison.lines.end(), [](const auto& line) {
    return line.origin == '-' && line.text == "# Old title";
  }));
  CHECK(std::any_of(comparison.lines.begin(), comparison.lines.end(), [](const auto& line) {
    return line.origin == '+' && line.text == "# New title";
  }));
}

TEST_CASE("Card history paginates using the last matching commit", "[history][git]") {
  const auto root = history_temp_dir();
  const std::string card_id = "abcd-page-card";
  holder::git::GitRepo repo;
  repo.open_or_init(root);
  write_commit(repo, card_id, "Page", "One", "Add card Page");
  write_commit(repo, card_id, "Page", "Two", "Update links for Page");
  write_commit(repo, card_id, "Page", "Three", "Update milestones for Page");

  holder::model::Project project;
  project.project_id = "project-history";
  project.root_path = root.string();
  project.privacy_mode = "plain";
  holder::history::CardHistoryService service;
  const auto first = service.list(project, card_id, 2);
  REQUIRE(first.entries.size() == 2);
  REQUIRE(first.next_cursor.has_value());
  const auto second = service.list(project, card_id, 2, first.next_cursor);
  REQUIRE(second.entries.size() == 1);
  CHECK(second.entries.front().kind == "created");
  CHECK_FALSE(second.next_cursor.has_value());
}

TEST_CASE("Card history pagination does not split an editing session", "[history][git]") {
  const auto root = history_temp_dir();
  const std::string card_id = "abcd-session-page";
  holder::git::GitRepo repo;
  repo.open_or_init(root);
  write_commit(repo, card_id, "Page", "One", "Add card Page");
  write_commit(repo, card_id, "Page", "Two", "Update card Page");
  write_commit(repo, card_id, "Page", "Three", "Update card Page");

  holder::model::Project project;
  project.project_id = "project-history";
  project.root_path = root.string();
  project.privacy_mode = "plain";
  holder::history::CardHistoryService service;
  const auto first = service.list(project, card_id, 1);
  REQUIRE(first.entries.size() == 1);
  CHECK(first.entries.front().commit_count == 2);
  REQUIRE(first.next_cursor.has_value());
  const auto second = service.list(project, card_id, 1, first.next_cursor);
  REQUIRE(second.entries.size() == 1);
  CHECK(second.entries.front().kind == "created");
  CHECK_FALSE(second.next_cursor.has_value());
}

TEST_CASE("Card history bounds very large comparison output", "[history][git]") {
  const auto root = history_temp_dir();
  const std::string card_id = "abcd-large-diff";
  holder::git::GitRepo repo;
  repo.open_or_init(root);
  write_commit(repo, card_id, "Large", "", "Add card Large");
  const auto old_oid = repo.head_oid();
  REQUIRE(old_oid.has_value());
  std::string body;
  for (int i = 0; i < 6'000; ++i) body += "Line " + std::to_string(i) + "\n";
  write_commit(repo, card_id, "Large", body, "Update card Large");

  holder::model::Project project;
  project.project_id = "project-history";
  project.root_path = root.string();
  project.privacy_mode = "plain";
  const auto comparison = holder::history::CardHistoryService().compare(project, card_id, old_oid);
  CHECK(comparison.truncated);
  CHECK(comparison.lines.size() == 5'000);
}

TEST_CASE("Card history decrypts encrypted project versions", "[history][git][privacy]") {
  const auto root = history_temp_dir();
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (root / "keystore").string());
  const std::string card_id = "abcd-encrypted-history";
  holder::model::Project project;
  project.project_id = "project-history";
  project.root_path = (root / "repo").string();
  project.privacy_mode = "encrypted_git";
  project.created_at = 1;
  project.updated_at = 1;
  auto db = holder::test::open_db_with_schema(root / "holder.db");
  holder::project::ProjectRepo projects(db);
  projects.create(project);
  project.project_key_id = holder::privacy::ensure_project_key_material(
      projects,
      project.project_id,
      std::nullopt,
      2,
      []() { return std::string("history-key"); }
  );

  holder::git::GitRepo repo;
  repo.open_or_init(project.root_path);
  write_encrypted_commit(repo, project, card_id, "Secret", "First secret\n", "Add card Secret");
  const auto old_oid = repo.head_oid();
  REQUIRE(old_oid.has_value());
  write_encrypted_commit(
      repo, project, card_id, "Secret", "Second secret\n", "Update card Secret"
  );

  holder::history::CardHistoryService service;
  const auto page = service.list(project, card_id);
  REQUIRE(page.entries.size() == 2);
  const auto comparison = service.compare(project, card_id, old_oid);
  CHECK(comparison.from.body == "First secret\n");
  CHECK(comparison.to.body == "Second secret\n");
}

TEST_CASE("Card history never initializes a missing repository", "[history][git]") {
  const auto root = history_temp_dir() / "missing-project";
  holder::model::Project project;
  project.project_id = "project-history";
  project.root_path = root.string();
  project.privacy_mode = "plain";

  CHECK_THROWS(holder::history::CardHistoryService().list(project, "abcd-missing-card"));
  CHECK_FALSE(std::filesystem::exists(root));
}
