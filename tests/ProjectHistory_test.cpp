#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "history/ProjectHistory.h"
#include "card/CardFrontMatter.h"
#include "card/CardPaths.h"
#include "git/GitRepo.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

using holder::history::ProjectHistoryObjectKind;

namespace {

std::filesystem::path project_history_temp_dir() {
  const auto suffix = std::to_string(static_cast<unsigned long long>(
      std::chrono::steady_clock::now().time_since_epoch().count()
  ));
  const auto path = std::filesystem::temp_directory_path() / ("holder_project_history_test_" + suffix);
  std::filesystem::create_directories(path);
  return path;
}

std::string project_history_card_file(const std::string& card_id, const std::string& title) {
  holder::model::Card card;
  card.card_id = card_id;
  card.project_id = "project-history";
  card.title = title;
  card.rel_path = holder::core::card_rel_path(card_id);
  card.created_at = 1;
  card.updated_at = 1;
  return holder::core::render_card_front_matter(card, {}, {}) + "Card body\n";
}

} // namespace

TEST_CASE("Project history classifies durable Holder paths", "[history]") {
  const auto classify = [](std::string_view path) {
    return holder::history::classify_project_history_path(path);
  };

  CHECK(classify("cards/ab/cd/abcd-card.md") == ProjectHistoryObjectKind::Card);
  CHECK(classify("trash/cards/ab/cd/abcd-card.md") == ProjectHistoryObjectKind::Card);
  CHECK(classify("resources/ab/cd/abcd-resource.json") == ProjectHistoryObjectKind::Resource);
  CHECK(classify("locations/ab/cd/abcd-location.json") == ProjectHistoryObjectKind::Location);
  CHECK(classify("ai_messages/ab/cd/abcd-message.md") == ProjectHistoryObjectKind::AiData);
  CHECK(classify("trash/ai_messages/ab/cd/abcd-message.md") == ProjectHistoryObjectKind::AiData);
  CHECK(classify("ai_threads/ab/cd/abcd-thread.json") == ProjectHistoryObjectKind::AiData);
  CHECK(classify(".holder/project.json") == ProjectHistoryObjectKind::ProjectSettings);
  CHECK(classify(".holder/privacy.json") == ProjectHistoryObjectKind::ProjectSettings);
}

TEST_CASE("Project history keeps unrecognised paths unknown", "[history]") {
  CHECK(
      holder::history::classify_project_history_path("notes/from-another-tool.txt") ==
      ProjectHistoryObjectKind::Unknown
  );
  CHECK(
      holder::history::classify_project_history_path(".holder/experimental.json") ==
      ProjectHistoryObjectKind::Unknown
  );
  CHECK(std::string(holder::history::project_history_object_kind_name(
      ProjectHistoryObjectKind::Unknown
  )) == "unknown");
}

TEST_CASE("Project history groups a commit's affected objects and filters activities", "[history][git]") {
  const auto root = project_history_temp_dir();
  holder::git::GitRepo repo;
  repo.open_or_init(root);
  repo.write_file(
      "cards/ab/cd/abcd-card.md", project_history_card_file("abcd-card", "Project card")
  );
  repo.write_file("resources/ef/gh/efgh-resource.json", "resource");
  repo.stage_paths({"cards/ab/cd/abcd-card.md", "resources/ef/gh/efgh-resource.json"});
  repo.commit("Attach example");
  repo.write_file("notes/from-another-tool.txt", "external");
  repo.stage_path("notes/from-another-tool.txt");
  repo.commit("External note");

  holder::model::Project project;
  project.project_id = "project-history";
  project.root_path = root.string();
  project.privacy_mode = "plain";
  const holder::history::ProjectHistoryService service;

  const auto page = service.list(project);
  REQUIRE(page.activities.size() == 2);
  CHECK(page.activities[0].message == "External note");
  REQUIRE(page.activities[0].affected_objects.size() == 1);
  CHECK(page.activities[0].affected_objects[0].kind == ProjectHistoryObjectKind::Unknown);
  CHECK(page.activities[1].message == "Attach example");
  REQUIRE(page.activities[1].affected_objects.size() == 2);
  CHECK(page.activities[1].affected_objects[0].kind == ProjectHistoryObjectKind::Card);
  REQUIRE(page.activities[1].affected_objects[0].items.size() == 1);
  REQUIRE(page.activities[1].affected_objects[0].items[0].title.has_value());
  CHECK(*page.activities[1].affected_objects[0].items[0].title == "Project card");
  CHECK(page.activities[1].affected_objects[1].kind == ProjectHistoryObjectKind::Resource);

  const auto resource_page = service.list(project, 50, std::nullopt, ProjectHistoryObjectKind::Resource);
  REQUIRE(resource_page.activities.size() == 1);
  CHECK(resource_page.activities[0].message == "Attach example");
}

TEST_CASE("Project history retains a card path when its historical title is unavailable", "[history][git]") {
  const auto root = project_history_temp_dir();
  holder::git::GitRepo repo;
  repo.open_or_init(root);
  repo.write_file("cards/ab/cd/abcd-unreadable-card.md", "not a Holder card");
  repo.stage_path("cards/ab/cd/abcd-unreadable-card.md");
  repo.commit("Imported external card file");

  holder::model::Project project;
  project.project_id = "project-history-unavailable-title";
  project.root_path = root.string();
  project.privacy_mode = "plain";

  const holder::history::ProjectHistoryService service;
  const auto page = service.list(project);
  REQUIRE(page.activities.size() == 1);
  REQUIRE(page.activities[0].affected_objects.size() == 1);
  const auto& item = page.activities[0].affected_objects[0].items[0];
  CHECK(item.path == "cards/ab/cd/abcd-unreadable-card.md");
  CHECK_FALSE(item.title.has_value());
}

TEST_CASE("Project history paginates incrementally and exposes bounded scan continuations", "[history][git]") {
  const auto root = project_history_temp_dir();
  holder::git::GitRepo repo;
  repo.open_or_init(root);

  for (int revision = 0; revision < 70; ++revision) {
    repo.write_file("cards/ab/cd/abcd-card.md", "revision " + std::to_string(revision));
    repo.stage_path("cards/ab/cd/abcd-card.md");
    repo.commit("Activity " + std::to_string(revision));
  }

  holder::model::Project project;
  project.project_id = "project-history-pagination";
  project.root_path = root.string();
  project.privacy_mode = "plain";

  const holder::history::ProjectHistoryService paged_service;
  const auto first_page = paged_service.list(project, 2);
  REQUIRE(first_page.activities.size() == 2);
  REQUIRE(first_page.next_cursor.has_value());
  CHECK_FALSE(first_page.scan_limited);

  const auto second_page = paged_service.list(project, 2, first_page.next_cursor);
  REQUIRE(second_page.activities.size() == 2);
  CHECK(second_page.activities[0].oid != first_page.activities[0].oid);
  CHECK(second_page.activities[0].oid != first_page.activities[1].oid);

  const holder::history::ProjectHistoryService bounded_service(3);
  const auto bounded_page = bounded_service.list(project, 50);
  REQUIRE(bounded_page.activities.size() == 3);
  CHECK(bounded_page.scan_limited);
  REQUIRE(bounded_page.next_cursor.has_value());

  const auto continued_page = bounded_service.list(project, 50, bounded_page.next_cursor);
  REQUIRE(continued_page.activities.size() == 3);
  CHECK(continued_page.scan_limited);
  REQUIRE(continued_page.next_cursor.has_value());
  CHECK(continued_page.activities[0].oid != bounded_page.activities[0].oid);
}
