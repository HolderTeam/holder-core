#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "history/ProjectHistory.h"

#include <string>
#include <string_view>

using holder::history::ProjectHistoryObjectKind;

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
