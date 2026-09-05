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
  CHECK(page.entries[2].kind == "created");
  CHECK(page.entries[2].summary == "Card created");
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
