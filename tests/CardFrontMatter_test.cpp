#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "card/CardFrontMatter.h"

#include <string>

TEST_CASE("parse_card_file reads front matter and body", "[card_front_matter]") {
  const std::string raw = "---\n"
                          "card_id: abcd1234\n"
                          "project_id: proj-1\n"
                          "title: Hello\n"
                          "created_at: 10\n"
                          "updated_at: 20\n"
                          "parent_card_id: null\n"
                          "sort_key: 1.5\n"
                          "rel_path: cards/ab/cd/abcd1234.md\n"
                          "deleted_at: null\n"
                          "links:\n"
                          "  - to: efgh5678\n"
                          "    to_type: card\n"
                          "    kind: ref\n"
                          "    created_at: 15\n"
                          "    label: \"See also\"\n"
                          "  - to: wxyz9999\n"
                          "    to_type: ai_message\n"
                          "    kind: parent\n"
                          "    created_at: 16\n"
                          "---\n"
                          "Body text\n";

  const auto parsed = holder::core::parse_card_file(raw);
  REQUIRE(parsed.has_front_matter);
  REQUIRE(parsed.body == "Body text\n");
  REQUIRE(parsed.card.card_id == "abcd1234");
  REQUIRE(parsed.card.project_id == "proj-1");
  REQUIRE(parsed.card.title == "Hello");
  REQUIRE(parsed.card.created_at == 10);
  REQUIRE(parsed.card.updated_at == 20);
  REQUIRE_FALSE(parsed.card.parent_card_id.has_value());
  REQUIRE(parsed.card.sort_key == 1.5);
  REQUIRE(parsed.card.rel_path == "cards/ab/cd/abcd1234.md");
  REQUIRE_FALSE(parsed.card.deleted_at.has_value());
  REQUIRE(parsed.links.size() == 2);
  REQUIRE(parsed.links[0].from_card_id == "abcd1234");
  REQUIRE(parsed.links[0].to_card_id == "efgh5678");
  REQUIRE(parsed.links[0].to_type == "card");
  REQUIRE(parsed.links[0].kind == "ref");
  REQUIRE(parsed.links[0].label.has_value());
  REQUIRE(parsed.links[0].label.value() == "See also");
  REQUIRE(parsed.links[1].to_card_id == "wxyz9999");
  REQUIRE(parsed.links[1].to_type == "ai_message");
  REQUIRE(parsed.links[1].kind == "parent");
}

TEST_CASE("parse_card_file falls back when no front matter", "[card_front_matter]") {
  const std::string raw = "Just a body\n";
  const auto parsed = holder::core::parse_card_file(raw);
  REQUIRE_FALSE(parsed.has_front_matter);
  REQUIRE(parsed.body == raw);
}

TEST_CASE("parse_card_file accepts CRLF front matter", "[card_front_matter]") {
  const std::string raw = "---\r\n"
                          "card_id: abcd1234\r\n"
                          "project_id: proj-1\r\n"
                          "title: Windows checkout\r\n"
                          "created_at: 10\r\n"
                          "updated_at: 20\r\n"
                          "sort_key: 1.5\r\n"
                          "rel_path: cards/ab/cd/abcd1234.md\r\n"
                          "---\r\n"
                          "Body text\r\n";

  const auto parsed = holder::core::parse_card_file(raw);
  REQUIRE(parsed.has_front_matter);
  REQUIRE(parsed.card.card_id == "abcd1234");
  REQUIRE(parsed.card.title == "Windows checkout");
  REQUIRE(parsed.body == "Body text\r\n");
}

TEST_CASE("render_card_front_matter includes links", "[card_front_matter]") {
  holder::model::Card card;
  card.card_id = "abcd1234";
  card.project_id = "proj-1";
  card.title = "Hello";
  card.created_at = 10;
  card.updated_at = 20;
  card.sort_key = 1.5;
  card.rel_path = "cards/ab/cd/abcd1234.md";

  holder::model::CardLink link;
  link.project_id = "proj-1";
  link.from_card_id = "abcd1234";
  link.to_card_id = "efgh5678";
  link.to_type = "card";
  link.kind = "ref";
  link.created_at = 15;
  link.label = "See also";

  const auto front_matter = holder::core::render_card_front_matter(card, {link}, {});
  REQUIRE(front_matter.find("links:") != std::string::npos);
  REQUIRE(front_matter.find("to: efgh5678") != std::string::npos);
  REQUIRE(front_matter.find("to_type: card") != std::string::npos);
  REQUIRE(front_matter.find("kind: ref") != std::string::npos);
  REQUIRE(front_matter.find("label: See also") != std::string::npos);
}

TEST_CASE("render_card_front_matter writes deleted_at and null link label", "[card_front_matter]") {
  holder::model::Card card;
  card.card_id = "abcd1234";
  card.project_id = "proj-1";
  card.title = "Hello";
  card.created_at = 10;
  card.updated_at = 20;
  card.sort_key = 1.5;
  card.rel_path = "cards/ab/cd/abcd1234.md";
  card.deleted_at = 99;

  holder::model::CardLink link;
  link.project_id = "proj-1";
  link.from_card_id = "abcd1234";
  link.to_card_id = "efgh5678";
  link.to_type = "card";
  link.kind = "ref";
  link.created_at = 15;

  const auto front_matter = holder::core::render_card_front_matter(card, {link}, {});
  REQUIRE(front_matter.find("deleted_at: 99") != std::string::npos);
  REQUIRE(
      (front_matter.find("label: ~") != std::string::npos ||
       front_matter.find("label: null") != std::string::npos)
  );
}

TEST_CASE("parse_card_file reads milestones", "[card_front_matter]") {
  const std::string raw = "---\n"
                          "card_id: abcd1234\n"
                          "project_id: proj-1\n"
                          "title: My Car\n"
                          "created_at: 10\n"
                          "updated_at: 20\n"
                          "sort_key: 1.5\n"
                          "rel_path: cards/ab/cd/abcd1234.md\n"
                          "milestones:\n"
                          "  - milestone_id: mile-1\n"
                          "    start_at: 100\n"
                          "    end_at: null\n"
                          "    all_day: true\n"
                          "    kind: Renewal\n"
                          "    description: Car insurance renewal\n"
                          "    created_at: 30\n"
                          "    updated_at: 30\n"
                          "  - milestone_id: mile-2\n"
                          "    start_at: 200\n"
                          "    end_at: 250\n"
                          "    all_day: false\n"
                          "    kind: null\n"
                          "    description: null\n"
                          "    created_at: 40\n"
                          "    updated_at: 40\n"
                          "---\n"
                          "Body text\n";

  const auto parsed = holder::core::parse_card_file(raw);
  REQUIRE(parsed.has_front_matter);
  REQUIRE(parsed.milestones.size() == 2);

  const auto& first = parsed.milestones[0];
  REQUIRE(first.milestone_id == "mile-1");
  REQUIRE(first.project_id == "proj-1");
  REQUIRE(first.card_id == "abcd1234");
  REQUIRE(first.start_at == 100);
  REQUIRE_FALSE(first.end_at.has_value());
  REQUIRE(first.all_day);
  REQUIRE(first.kind.has_value());
  REQUIRE(first.kind.value() == "Renewal");
  REQUIRE(first.description.has_value());
  REQUIRE(first.description.value() == "Car insurance renewal");
  REQUIRE(first.created_at == 30);
  REQUIRE(first.updated_at == 30);

  const auto& second = parsed.milestones[1];
  REQUIRE(second.milestone_id == "mile-2");
  REQUIRE(second.start_at == 200);
  REQUIRE(second.end_at.has_value());
  REQUIRE(second.end_at.value() == 250);
  REQUIRE_FALSE(second.all_day);
  REQUIRE_FALSE(second.kind.has_value());
  REQUIRE_FALSE(second.description.has_value());
}

TEST_CASE("parse_card_file skips a milestone entry with no milestone_id", "[card_front_matter]") {
  const std::string raw = "---\n"
                          "card_id: abcd1234\n"
                          "project_id: proj-1\n"
                          "title: My Car\n"
                          "created_at: 10\n"
                          "updated_at: 20\n"
                          "sort_key: 1.5\n"
                          "rel_path: cards/ab/cd/abcd1234.md\n"
                          "milestones:\n"
                          "  - start_at: 100\n"
                          "    all_day: true\n"
                          "---\n"
                          "Body text\n";

  const auto parsed = holder::core::parse_card_file(raw);
  REQUIRE(parsed.milestones.empty());
}

TEST_CASE("render_card_front_matter includes milestones", "[card_front_matter]") {
  holder::model::Card card;
  card.card_id = "abcd1234";
  card.project_id = "proj-1";
  card.title = "My Car";
  card.created_at = 10;
  card.updated_at = 20;
  card.sort_key = 1.5;
  card.rel_path = "cards/ab/cd/abcd1234.md";

  holder::model::Milestone milestone;
  milestone.milestone_id = "mile-1";
  milestone.project_id = "proj-1";
  milestone.card_id = "abcd1234";
  milestone.start_at = 100;
  milestone.end_at = 150;
  milestone.all_day = false;
  milestone.kind = "Service";
  milestone.description = "Annual service at garage";
  milestone.created_at = 30;
  milestone.updated_at = 30;

  const auto front_matter = holder::core::render_card_front_matter(card, {}, {milestone});
  REQUIRE(front_matter.find("milestones:") != std::string::npos);
  REQUIRE(front_matter.find("milestone_id: mile-1") != std::string::npos);
  REQUIRE(front_matter.find("start_at: 100") != std::string::npos);
  REQUIRE(front_matter.find("end_at: 150") != std::string::npos);
  REQUIRE(front_matter.find("kind: Service") != std::string::npos);
  REQUIRE(front_matter.find("description: Annual service at garage") != std::string::npos);
}

TEST_CASE("render_card_front_matter writes null end_at, kind, and description", "[card_front_matter]") {
  holder::model::Card card;
  card.card_id = "abcd1234";
  card.project_id = "proj-1";
  card.title = "My Car";
  card.created_at = 10;
  card.updated_at = 20;
  card.sort_key = 1.5;
  card.rel_path = "cards/ab/cd/abcd1234.md";

  holder::model::Milestone milestone;
  milestone.milestone_id = "mile-1";
  milestone.project_id = "proj-1";
  milestone.card_id = "abcd1234";
  milestone.start_at = 100;
  milestone.all_day = true;
  milestone.created_at = 30;
  milestone.updated_at = 30;

  const auto front_matter = holder::core::render_card_front_matter(card, {}, {milestone});
  const auto has_null = [&](const std::string& key) {
    return front_matter.find(key + ": ~") != std::string::npos ||
           front_matter.find(key + ": null") != std::string::npos;
  };
  REQUIRE(has_null("end_at"));
  REQUIRE(has_null("kind"));
  REQUIRE(has_null("description"));
}

TEST_CASE("milestones round-trip through render and parse", "[card_front_matter]") {
  holder::model::Card card;
  card.card_id = "abcd1234";
  card.project_id = "proj-1";
  card.title = "My Car";
  card.created_at = 10;
  card.updated_at = 20;
  card.sort_key = 1.5;
  card.rel_path = "cards/ab/cd/abcd1234.md";

  holder::model::Milestone milestone;
  milestone.milestone_id = "mile-1";
  milestone.project_id = "proj-1";
  milestone.card_id = "abcd1234";
  milestone.start_at = 100;
  milestone.end_at = 150;
  milestone.all_day = false;
  milestone.kind = "Service";
  milestone.description = "Annual service at garage";
  milestone.created_at = 30;
  milestone.updated_at = 30;

  const auto front_matter = holder::core::render_card_front_matter(card, {}, {milestone});
  const auto parsed = holder::core::parse_card_file(front_matter + "Body\n");

  REQUIRE(parsed.milestones.size() == 1);
  const auto& round_tripped = parsed.milestones[0];
  REQUIRE(round_tripped.milestone_id == milestone.milestone_id);
  REQUIRE(round_tripped.start_at == milestone.start_at);
  REQUIRE(round_tripped.end_at == milestone.end_at);
  REQUIRE(round_tripped.all_day == milestone.all_day);
  REQUIRE(round_tripped.kind == milestone.kind);
  REQUIRE(round_tripped.description == milestone.description);
  REQUIRE(round_tripped.created_at == milestone.created_at);
  REQUIRE(round_tripped.updated_at == milestone.updated_at);
}

TEST_CASE("parse_card_file falls back when front matter is unterminated", "[card_front_matter]") {
  const std::string raw = "---\n"
                          "card_id: abcd1234\n"
                          "project_id: proj-1\n";
  const auto parsed = holder::core::parse_card_file(raw);
  REQUIRE_FALSE(parsed.has_front_matter);
  REQUIRE(parsed.body == raw);
}

TEST_CASE("parse_card_file falls back when yaml root is not a map", "[card_front_matter]") {
  const std::string raw = "---\n"
                          "- list-item\n"
                          "---\n"
                          "Body\n";
  const auto parsed = holder::core::parse_card_file(raw);
  REQUIRE_FALSE(parsed.has_front_matter);
  REQUIRE(parsed.body == raw);
}

TEST_CASE("parse_card_file reads deleted_at when present", "[card_front_matter]") {
  const std::string raw = "---\n"
                          "card_id: abcd1234\n"
                          "project_id: proj-1\n"
                          "title: Hello\n"
                          "created_at: 10\n"
                          "updated_at: 20\n"
                          "parent_card_id: null\n"
                          "sort_key: 1.5\n"
                          "rel_path: cards/ab/cd/abcd1234.md\n"
                          "deleted_at: 123\n"
                          "---\n"
                          "Body text\n";

  const auto parsed = holder::core::parse_card_file(raw);
  REQUIRE(parsed.has_front_matter);
  REQUIRE(parsed.card.deleted_at.has_value());
  REQUIRE(parsed.card.deleted_at.value() == 123);
}

TEST_CASE("parse_card_file falls back on yaml parse exception", "[card_front_matter]") {
  const std::string raw = "---\n"
                          "card_id: [\n"
                          "---\n"
                          "Body text\n";
  const auto parsed = holder::core::parse_card_file(raw);
  REQUIRE_FALSE(parsed.has_front_matter);
  REQUIRE(parsed.body == raw);
}
