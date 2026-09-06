#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "history/ProjectHistory.h"
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
  repo.write_file("cards/ab/cd/abcd-card.md", "card");
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
  CHECK(page.activities[1].affected_objects[1].kind == ProjectHistoryObjectKind::Resource);

  const auto resource_page = service.list(project, 50, std::nullopt, ProjectHistoryObjectKind::Resource);
  REQUIRE(resource_page.activities.size() == 1);
  CHECK(resource_page.activities[0].message == "Attach example");
}
