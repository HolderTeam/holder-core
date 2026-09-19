#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "ai/AiMessageFrontMatter.h"
#include "ai/AiMessagePaths.h"
#include "ai/AiThreadManifest.h"
#include "card/CardFrontMatter.h"
#include "card/CardPaths.h"
#include "core_test_helpers.h"
#include "git/GitRepo.h"
#include "history/ProjectHistory.h"
#include "project/ProjectManifest.h"
#include "resource/ResourceManifest.h"
#include "resource/ResourcePaths.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

using holder::history::ProjectHistoryObjectKind;

namespace {

std::filesystem::path project_history_temp_dir() {
  const auto suffix = std::to_string(
      static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())
  );
  const auto path = std::filesystem::temp_directory_path() /
                    ("holder_project_history_test_" + suffix);
  std::filesystem::create_directories(path);
  return path;
}

std::string project_history_card_file(
    const std::string& card_id,
    const std::string& title,
    const std::vector<holder::model::Milestone>& milestones = {}
) {
  holder::model::Card card;
  card.card_id = card_id;
  card.project_id = "project-history";
  card.title = title;
  card.rel_path = holder::core::card_rel_path(card_id);
  card.created_at = 1;
  card.updated_at = 1;
  return holder::core::render_card_front_matter(card, {}, milestones) + "Card body\n";
}

std::string project_history_resource_manifest(
    const std::string& resource_id,
    const std::string& label,
    int asset_count = 1
) {
  holder::model::ResourceBundle bundle;
  bundle.resource.resource_id = resource_id;
  bundle.resource.project_id = "project-history";
  bundle.resource.type = "file";
  bundle.resource.label = label;
  bundle.resource.created_at = 1;
  bundle.resource.updated_at = 1;
  for (int index = 0; index < asset_count; ++index) {
    holder::model::Asset asset;
    asset.asset_id = "asset-" + std::to_string(index) + "-" + resource_id;
    asset.resource_id = resource_id;
    asset.original_filename = asset_count == 1 ? "project-notes.pdf"
                                               : "attachment-" + std::to_string(index) + ".pdf";
    asset.media_type = "application/pdf";
    asset.byte_size = 42;
    asset.plaintext_sha256 = std::string(64, 'a');
    bundle.assets.push_back(asset);
  }
  return holder::resource::render_resource_manifest(bundle);
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
  CHECK(
      std::string(holder::history::project_history_object_kind_name(ProjectHistoryObjectKind::Card)
      ) == "card"
  );
  CHECK(
      std::string(
          holder::history::project_history_object_kind_name(ProjectHistoryObjectKind::Resource)
      ) == "resource"
  );
  CHECK(
      std::string(
          holder::history::project_history_object_kind_name(ProjectHistoryObjectKind::Location)
      ) == "location"
  );
  CHECK(
      std::string(holder::history::project_history_object_kind_name(ProjectHistoryObjectKind::AiData
      )) == "ai_data"
  );
  CHECK(
      std::string(holder::history::project_history_object_kind_name(
          ProjectHistoryObjectKind::ProjectSettings
      )) == "project_settings"
  );
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
  CHECK(
      std::string(
          holder::history::project_history_object_kind_name(ProjectHistoryObjectKind::Unknown)
      ) == "unknown"
  );
  CHECK(
      std::string(holder::history::project_history_object_kind_name(
          static_cast<ProjectHistoryObjectKind>(999)
      )) == "unknown"
  );
}

TEST_CASE("Project history marks multi-parent activities as merges", "[history]") {
  const auto activity = holder::history::group_project_history_activity(
      "merge-oid",
      {"main-parent", "side-parent"},
      "Ezra",
      "ezra@example.test",
      1,
      2,
      "Combine project changes",
      {"cards/ab/cd/abcd-card.md"}
  );
  CHECK(activity.is_merge);
  REQUIRE(activity.parent_oids.size() == 2);
  CHECK(activity.parent_oids[0] == "main-parent");
  CHECK(activity.parent_oids[1] == "side-parent");
}

TEST_CASE(
    "Project history groups a commit's affected objects and filters activities",
    "[history][git]"
) {
  const auto root = project_history_temp_dir();
  holder::git::GitRepo repo;
  repo.open_or_init(root);
  holder::model::Milestone milestone;
  milestone.milestone_id = "project-history-milestone";
  milestone.project_id = "project-history";
  milestone.card_id = "abcd-card";
  milestone.start_at = 1;
  milestone.kind = "Review";
  milestone.description = "Project review";
  repo.write_file(
      "cards/ab/cd/abcd-card.md",
      project_history_card_file("abcd-card", "Project card", {milestone})
  );
  repo.write_file(
      "resources/ef/gh/efgh-resource.json",
      project_history_resource_manifest("efgh-resource", "Project notes")
  );
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
  REQUIRE(page.activities[1].affected_objects[0].items[0].detail.has_value());
  CHECK(
      *page.activities[1].affected_objects[0].items[0].detail ==
      "Milestone: Review — Project review"
  );
  CHECK(page.activities[1].affected_objects[1].kind == ProjectHistoryObjectKind::Resource);
  REQUIRE(page.activities[1].affected_objects[1].items[0].title.has_value());
  CHECK(*page.activities[1].affected_objects[1].items[0].title == "Project notes");
  REQUIRE(page.activities[1].affected_objects[1].items[0].detail.has_value());
  CHECK(*page.activities[1].affected_objects[1].items[0].detail == "Attachment: project-notes.pdf");

  const auto resource_page =
      service.list(project, 50, std::nullopt, ProjectHistoryObjectKind::Resource);
  REQUIRE(resource_page.activities.size() == 1);
  CHECK(resource_page.activities[0].message == "Attach example");
}

TEST_CASE(
    "Project history retains a card path when its historical title is unavailable",
    "[history][git]"
) {
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

TEST_CASE(
    "Project history keeps paths when encrypted card enrichment has no "
    "usable key",
    "[history][git]"
) {
  const auto root = project_history_temp_dir();
  holder::git::GitRepo repo;
  repo.open_or_init(root);
  repo.write_file(
      "cards/ab/cd/abcd-encrypted-card.md",
      project_history_card_file("abcd-encrypted-card", "Unavailable title")
  );
  repo.stage_path("cards/ab/cd/abcd-encrypted-card.md");
  repo.commit("Add encrypted card path");

  holder::model::Project project;
  project.project_id = "project-history";
  project.root_path = root.string();
  project.privacy_mode = "encrypted_git";

  SECTION("key id is absent") {
    const auto page = holder::history::ProjectHistoryService().list(project);
    REQUIRE(page.activities.size() == 1);
    CHECK_FALSE(page.activities[0].affected_objects[0].items[0].title.has_value());
  }

  SECTION("key material is absent") {
    holder::test::EnvGuard keystore_env(
        "HOLDER_TEST_KEYSTORE_DIR",
        (root / "empty-keystore").string()
    );
    project.project_key_id = "missing-history-key";
    const auto page = holder::history::ProjectHistoryService().list(project);
    REQUIRE(page.activities.size() == 1);
    CHECK_FALSE(page.activities[0].affected_objects[0].items[0].title.has_value());
  }
}

TEST_CASE("Project history abbreviates long milestone and attachment summaries", "[history][git]") {
  const auto root = project_history_temp_dir();
  holder::git::GitRepo repo;
  repo.open_or_init(root);

  std::vector<holder::model::Milestone> milestones;
  for (int index = 0; index < 4; ++index) {
    holder::model::Milestone milestone;
    milestone.milestone_id = "milestone-" + std::to_string(index);
    milestone.project_id = "project-history";
    milestone.card_id = "abcd-many-items";
    milestone.start_at = index + 1;
    milestone.kind = "Review " + std::to_string(index);
    milestones.push_back(milestone);
  }
  repo.write_file(
      holder::core::card_rel_path("abcd-many-items"),
      project_history_card_file("abcd-many-items", "Many milestones", milestones)
  );
  repo.write_file(
      holder::resource::resource_rel_path("resource-many"),
      project_history_resource_manifest("resource-many", "Many attachments", 4)
  );
  repo.stage_paths({
      holder::core::card_rel_path("abcd-many-items"),
      holder::resource::resource_rel_path("resource-many"),
  });
  repo.commit("Add many project items");

  holder::model::Project project;
  project.project_id = "project-history";
  project.root_path = root.string();
  project.privacy_mode = "plain";
  const auto page = holder::history::ProjectHistoryService().list(project);
  REQUIRE(page.activities.size() == 1);
  REQUIRE(page.activities[0].affected_objects.size() == 2);
  CHECK(
      page.activities[0].affected_objects[0].items[0].detail->find("+1 more") != std::string::npos
  );
  CHECK(
      page.activities[0].affected_objects[1].items[0].detail->find("+1 more") != std::string::npos
  );
}

TEST_CASE("Project history describes historical AI threads and messages", "[history][git]") {
  const auto root = project_history_temp_dir();
  holder::git::GitRepo repo;
  repo.open_or_init(root);

  holder::model::Project project;
  project.project_id = "project-history";
  project.root_path = root.string();
  project.privacy_mode = "plain";

  holder::model::AiThread thread;
  thread.thread_id = "thread-history";
  thread.project_id = project.project_id;
  thread.title = "Release review";
  thread.created_at = 1;
  thread.updated_at = 1;
  const auto thread_path = holder::ai::ai_thread_manifest_rel_path(thread.thread_id);
  repo.write_file(thread_path, holder::ai::render_ai_thread_manifest(project, thread));
  repo.stage_path(thread_path);
  repo.commit("Create AI thread");

  holder::model::AiMessage message;
  message.message_id = "message-history";
  message.thread_id = thread.thread_id;
  message.role = "user";
  message.source = "holder";
  message.created_at = 2;
  const auto message_path = holder::core::ai_message_rel_path(message.message_id);
  repo.write_file(
      message_path,
      holder::core::render_ai_message_front_matter(message, project.project_id, {}) +
          "Can you review the release notes?\n"
  );
  repo.stage_path(message_path);
  repo.commit("Add AI message");

  auto blank_message = message;
  blank_message.message_id = "message-blank-history";
  blank_message.created_at = 3;
  const auto blank_message_path = holder::core::ai_message_rel_path(blank_message.message_id);
  repo.write_file(
      blank_message_path,
      holder::core::render_ai_message_front_matter(blank_message, project.project_id, {}) +
          "  \n\t\n"
  );
  repo.stage_path(blank_message_path);
  repo.commit("Add blank AI message");

  const holder::history::ProjectHistoryService service;
  const auto page = service.list(project);
  REQUIRE(page.activities.size() == 3);
  const auto& blank_item = page.activities[0].affected_objects[0].items[0];
  CHECK(blank_item.path == blank_message_path);
  CHECK_FALSE(blank_item.detail.has_value());
  const auto& message_item = page.activities[1].affected_objects[0].items[0];
  CHECK(message_item.path == message_path);
  REQUIRE(message_item.title.has_value());
  CHECK(*message_item.title == "Release review");
  REQUIRE(message_item.detail.has_value());
  CHECK(*message_item.detail == "user: Can you review the release notes?");
  const auto& thread_item = page.activities[2].affected_objects[0].items[0];
  CHECK(thread_item.path == thread_path);
  REQUIRE(thread_item.title.has_value());
  CHECK(*thread_item.title == "Release review");
  CHECK_FALSE(thread_item.detail.has_value());
}

TEST_CASE(
    "Project history describes historical project settings without secrets",
    "[history][git]"
) {
  const auto root = project_history_temp_dir();
  holder::git::GitRepo repo;
  repo.open_or_init(root);

  holder::model::Project project;
  project.project_id = "project-history-settings";
  project.name = "History project";
  project.root_path = root.string();
  project.privacy_mode = "plain";
  project.git_provider = "github";
  project.git_remote_url = "https://example.test/secret.git";
  project.created_at = 1;
  project.updated_at = 2;
  repo.write_file(
      holder::project::kProjectBootstrapPath,
      holder::project::render_project_bootstrap(project)
  );
  repo.write_file(
      holder::project::kProjectManifestPath,
      holder::project::render_project_manifest(project)
  );
  repo.stage_paths({holder::project::kProjectBootstrapPath, holder::project::kProjectManifestPath});
  repo.commit("Update project settings");

  const holder::history::ProjectHistoryService service;
  const auto page = service.list(project);
  REQUIRE(page.activities.size() == 1);
  REQUIRE(page.activities[0].affected_objects.size() == 1);
  const auto& items = page.activities[0].affected_objects[0].items;
  REQUIRE(items.size() == 2);
  bool found_privacy = false;
  bool found_project = false;
  for (const auto& item : items) {
    if (item.path == holder::project::kProjectBootstrapPath) {
      found_privacy = true;
      CHECK(item.title == "Privacy settings");
      CHECK(item.detail == "Mode: plain Git");
    }
    if (item.path == holder::project::kProjectManifestPath) {
      found_project = true;
      CHECK(item.title == "Project settings");
      CHECK(item.detail == "Project name: History project · Git provider: github");
      CHECK(item.detail->find("secret") == std::string::npos);
    }
  }
  CHECK(found_privacy);
  CHECK(found_project);
}

TEST_CASE("Project history ignores settings manifests owned by another project", "[history][git]") {
  const auto root = project_history_temp_dir();
  holder::git::GitRepo repo;
  repo.open_or_init(root);

  holder::model::Project project;
  project.project_id = "project-history-settings";
  project.name = "History project";
  project.root_path = root.string();
  project.privacy_mode = "plain";
  project.created_at = 1;
  project.updated_at = 2;

  auto foreign = project;
  foreign.project_id = "another-project";
  repo.write_file(
      holder::project::kProjectBootstrapPath,
      holder::project::render_project_bootstrap(foreign)
  );
  repo.stage_path(holder::project::kProjectBootstrapPath);
  repo.commit("Foreign bootstrap");
  repo.write_file(
      holder::project::kProjectManifestPath,
      holder::project::render_project_manifest(foreign)
  );
  repo.stage_path(holder::project::kProjectManifestPath);
  repo.commit("Foreign manifest");

  const auto page = holder::history::ProjectHistoryService().list(project);
  REQUIRE(page.activities.size() == 2);
  for (const auto& activity : page.activities) {
    REQUIRE(activity.affected_objects.size() == 1);
    REQUIRE(activity.affected_objects[0].items.size() == 1);
    CHECK_FALSE(activity.affected_objects[0].items[0].title.has_value());
    CHECK_FALSE(activity.affected_objects[0].items[0].detail.has_value());
  }
}

TEST_CASE("Project history returns no activity for an unborn repository", "[history][git]") {
  const auto root = project_history_temp_dir();
  holder::git::GitRepo repo;
  repo.open_or_init(root);
  holder::model::Project project;
  project.project_id = "project-history-empty";
  project.root_path = root.string();
  project.privacy_mode = "plain";
  const auto page = holder::history::ProjectHistoryService().list(project);
  CHECK(page.activities.empty());
  CHECK_FALSE(page.scan_limited);
  CHECK_FALSE(page.next_cursor.has_value());
}

TEST_CASE(
    "Project history paginates incrementally and exposes bounded scan continuations",
    "[history][git]"
) {
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

  const auto no_resources = holder::history::ProjectHistoryService().list(
      project,
      50,
      std::nullopt,
      ProjectHistoryObjectKind::Resource
  );
  CHECK(no_resources.activities.empty());
  REQUIRE_THROWS_WITH(
      holder::history::ProjectHistoryService().list(project, 0),
      Catch::Matchers::ContainsSubstring("history limit must be between 1 and 200")
  );
}
