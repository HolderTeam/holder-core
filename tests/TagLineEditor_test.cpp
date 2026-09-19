#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "card/TagLineEditor.h"

#include <string>

using holder::core::remove_from_trailing_tag_line;
using holder::core::RemoveTagLineOutcome;
using holder::core::upsert_trailing_tag_line;

TEST_CASE(
    "upsert_trailing_tag_line creates a line with one blank-line separator",
    "[tag_line_editor]"
) {
  REQUIRE(upsert_trailing_tag_line("Some useful thing", "work") == "Some useful thing\n\n#work");
}

TEST_CASE(
    "upsert_trailing_tag_line appends to an existing trailing line, preserving order",
    "[tag_line_editor]"
) {
  REQUIRE(
      upsert_trailing_tag_line("Some useful thing\n\n#work #holder", "android") ==
      "Some useful thing\n\n#work #holder #android"
  );
}

TEST_CASE(
    "upsert_trailing_tag_line is idempotent for a tag already on the line",
    "[tag_line_editor]"
) {
  const std::string body = "Some useful thing\n\n#work #android";
  REQUIRE(upsert_trailing_tag_line(body, "android") == body);
}

TEST_CASE(
    "upsert_trailing_tag_line preserves trailing blank lines, inserting before them",
    "[tag_line_editor]"
) {
  REQUIRE(upsert_trailing_tag_line("text\n\n\n", "android") == "text\n\n#android\n\n\n");
}

TEST_CASE("upsert_trailing_tag_line preserves a single trailing newline", "[tag_line_editor]") {
  REQUIRE(upsert_trailing_tag_line("hello\n", "android") == "hello\n\n#android\n");
}

TEST_CASE(
    "upsert_trailing_tag_line updating an existing line adds no extra blank line",
    "[tag_line_editor]"
) {
  REQUIRE(upsert_trailing_tag_line("hello\n\n#work\n", "android") == "hello\n\n#work #android\n");
}

TEST_CASE(
    "upsert_trailing_tag_line on a blank/empty body becomes just the tag line",
    "[tag_line_editor]"
) {
  REQUIRE(upsert_trailing_tag_line("", "android") == "#android");
  REQUIRE(upsert_trailing_tag_line("   \n\n  ", "android") == "#android");
}

TEST_CASE(
    "upsert_trailing_tag_line does not treat a prose line ending in a tag as the tag line",
    "[tag_line_editor]"
) {
  REQUIRE(
      upsert_trailing_tag_line("This relates to #work quite a lot.", "android") ==
      "This relates to #work quite a lot.\n\n#android"
  );
}

TEST_CASE(
    "remove_from_trailing_tag_line removes one tag, keeping the others in order",
    "[tag_line_editor]"
) {
  const auto result = remove_from_trailing_tag_line("hello\n\n#work #holder #android", "holder");
  REQUIRE(result.outcome == RemoveTagLineOutcome::Removed);
  REQUIRE(result.new_body == "hello\n\n#work #android");
}

TEST_CASE(
    "remove_from_trailing_tag_line removes the whole line and its separator when it empties",
    "[tag_line_editor]"
) {
  const auto result = remove_from_trailing_tag_line("hello\n\n#android", "android");
  REQUIRE(result.outcome == RemoveTagLineOutcome::Removed);
  REQUIRE(result.new_body == "hello");
}

TEST_CASE(
    "remove_from_trailing_tag_line preserves trailing whitespace after removal",
    "[tag_line_editor]"
) {
  const auto result = remove_from_trailing_tag_line("hello\n\n#android\n\n\n", "android");
  REQUIRE(result.outcome == RemoveTagLineOutcome::Removed);
  REQUIRE(result.new_body == "hello\n\n\n");
}

TEST_CASE(
    "remove_from_trailing_tag_line reports NotOnTrailingLine for a tag only in prose",
    "[tag_line_editor]"
) {
  const auto result = remove_from_trailing_tag_line("This is about #work today.", "work");
  REQUIRE(result.outcome == RemoveTagLineOutcome::NotOnTrailingLine);
  REQUIRE(result.new_body == "This is about #work today.");
}

TEST_CASE(
    "remove_from_trailing_tag_line reports NotOnTrailingLine for a tag not on the line",
    "[tag_line_editor]"
) {
  const auto result = remove_from_trailing_tag_line("hello\n\n#work", "android");
  REQUIRE(result.outcome == RemoveTagLineOutcome::NotOnTrailingLine);
  REQUIRE(result.new_body == "hello\n\n#work");
}

TEST_CASE(
    "remove_from_trailing_tag_line reports NotOnTrailingLine when there's no tag line at all",
    "[tag_line_editor]"
) {
  const auto result = remove_from_trailing_tag_line("just a plain card", "android");
  REQUIRE(result.outcome == RemoveTagLineOutcome::NotOnTrailingLine);
  REQUIRE(result.new_body == "just a plain card");
}

TEST_CASE(
    "remove_from_trailing_tag_line on an empty body reports NotOnTrailingLine",
    "[tag_line_editor]"
) {
  const auto result = remove_from_trailing_tag_line("", "android");
  REQUIRE(result.outcome == RemoveTagLineOutcome::NotOnTrailingLine);
  REQUIRE(result.new_body.empty());
}

TEST_CASE(
    "remove_from_trailing_tag_line finding the tag doubled -- prose and trailing line -- only touches the line",
    "[tag_line_editor]"
) {
  const auto result =
      remove_from_trailing_tag_line("I mentioned #android earlier.\n\n#android", "android");
  REQUIRE(result.outcome == RemoveTagLineOutcome::Removed);
  REQUIRE(result.new_body == "I mentioned #android earlier.");
}

using holder::core::tags_on_trailing_line;

TEST_CASE("tags_on_trailing_line returns the line's tags in order", "[tag_line_editor]") {
  REQUIRE(
      tags_on_trailing_line("hello\n\n#work #holder #android") ==
      std::vector<std::string>{"work", "holder", "android"}
  );
}

TEST_CASE("tags_on_trailing_line ignores a tag that only occurs in prose", "[tag_line_editor]") {
  REQUIRE(tags_on_trailing_line("This is about #work today.").empty());
}

TEST_CASE(
    "tags_on_trailing_line only lists the trailing-line tag when the same tag is doubled in prose",
    "[tag_line_editor]"
) {
  REQUIRE(
      tags_on_trailing_line("I mentioned #android earlier.\n\n#android") ==
      std::vector<std::string>{"android"}
  );
}

TEST_CASE("tags_on_trailing_line returns empty for a body with no tag line", "[tag_line_editor]") {
  REQUIRE(tags_on_trailing_line("just a plain card").empty());
}

TEST_CASE("tags_on_trailing_line returns empty for an empty body", "[tag_line_editor]") {
  REQUIRE(tags_on_trailing_line("").empty());
}

TEST_CASE(
    "tags_on_trailing_line rejects an invalid hash token on the tag line",
    "[tag_line_editor]"
) {
  REQUIRE(tags_on_trailing_line("Body\n\n#valid #123").empty());
}

TEST_CASE(
    "tags_on_trailing_line survives trailing whitespace after the line",
    "[tag_line_editor]"
) {
  REQUIRE(
      tags_on_trailing_line("hello\n\n#work #android\n\n\n") ==
      std::vector<std::string>{"work", "android"}
  );
}
