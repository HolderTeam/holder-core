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

  const auto front_matter = holder::core::render_card_front_matter(card, {link});
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

  const auto front_matter = holder::core::render_card_front_matter(card, {link});
  REQUIRE(front_matter.find("deleted_at: 99") != std::string::npos);
  REQUIRE(
      (front_matter.find("label: ~") != std::string::npos ||
       front_matter.find("label: null") != std::string::npos)
  );
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
