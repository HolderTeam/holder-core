#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "ai/AiMessageFrontMatter.h"

TEST_CASE("parse_ai_message_file reads front matter and body", "[ai_message_front_matter]") {
  const std::string raw = "---\n"
                          "message_id: msg-1\n"
                          "project_id: proj-1\n"
                          "thread_id: thread-1\n"
                          "role: user\n"
                          "source: manual\n"
                          "provider: Ollama\n"
                          "model: llama\n"
                          "created_at: 10\n"
                          "prompt_hash: hash\n"
                          "meta_json: \"{\\\"tokens\\\": 2}\"\n"
                          "links:\n"
                          "  - to: card-1\n"
                          "    to_type: card\n"
                          "    kind: ref\n"
                          "    created_at: 12\n"
                          "  - to: res-1\n"
                          "    to_type: resource\n"
                          "    kind: source\n"
                          "    created_at: 13\n"
                          "---\n"
                          "Hello\n";

  const auto parsed = holder::core::parse_ai_message_file(raw);
  REQUIRE(parsed.has_front_matter);
  REQUIRE(parsed.project_id == "proj-1");
  REQUIRE(parsed.message.message_id == "msg-1");
  REQUIRE(parsed.message.thread_id == "thread-1");
  REQUIRE(parsed.message.role == "user");
  REQUIRE(parsed.message.source == "manual");
  REQUIRE(parsed.message.provider.has_value());
  REQUIRE(parsed.message.model.has_value());
  REQUIRE(parsed.message.prompt_hash.has_value());
  REQUIRE(parsed.message.meta_json.has_value());
  REQUIRE(parsed.links.size() == 2);
  REQUIRE(parsed.links[0].to_card_id == "card-1");
  REQUIRE(parsed.links[0].to_type == "card");
  REQUIRE(parsed.links[1].to_card_id == "res-1");
  REQUIRE(parsed.links[1].to_type == "resource");
  REQUIRE(parsed.body == "Hello\n");
}

TEST_CASE("parse_ai_message_file falls back when no front matter", "[ai_message_front_matter]") {
  const std::string raw = "Just a body\n";
  const auto parsed = holder::core::parse_ai_message_file(raw);
  REQUIRE_FALSE(parsed.has_front_matter);
  REQUIRE(parsed.body == raw);
}

TEST_CASE(
    "render_ai_message_front_matter includes optional link label",
    "[ai_message_front_matter]"
) {
  holder::model::AiMessage message;
  message.message_id = "msg-1";
  message.thread_id = "thread-1";
  message.role = "assistant";
  message.source = "manual";
  message.created_at = 42;

  holder::model::CardLink link;
  link.to_card_id = "card-1";
  link.to_type = "card";
  link.kind = "ref";
  link.created_at = 43;
  link.label = "See also";
  std::vector<holder::model::CardLink> links{link};

  const auto yaml = holder::core::render_ai_message_front_matter(message, "proj-1", links);
  REQUIRE(yaml.find("label: See also") != std::string::npos);
}

TEST_CASE(
    "parse_ai_message_file falls back when front matter is unterminated",
    "[ai_message_front_matter]"
) {
  const std::string raw = "---\n"
                          "message_id: msg-1\n"
                          "project_id: proj-1\n";
  const auto parsed = holder::core::parse_ai_message_file(raw);
  REQUIRE_FALSE(parsed.has_front_matter);
  REQUIRE(parsed.body == raw);
}

TEST_CASE(
    "parse_ai_message_file falls back when yaml root is not a map",
    "[ai_message_front_matter]"
) {
  const std::string raw = "---\n"
                          "- item\n"
                          "---\n"
                          "Body\n";
  const auto parsed = holder::core::parse_ai_message_file(raw);
  REQUIRE_FALSE(parsed.has_front_matter);
  REQUIRE(parsed.body == raw);
}

TEST_CASE("parse_ai_message_file parses optional link label", "[ai_message_front_matter]") {
  const std::string raw = "---\n"
                          "message_id: msg-1\n"
                          "project_id: proj-1\n"
                          "thread_id: thread-1\n"
                          "role: user\n"
                          "source: manual\n"
                          "created_at: 10\n"
                          "links:\n"
                          "  - to: card-1\n"
                          "    to_type: card\n"
                          "    kind: ref\n"
                          "    label: Related\n"
                          "    created_at: 12\n"
                          "---\n"
                          "Hello\n";

  const auto parsed = holder::core::parse_ai_message_file(raw);
  REQUIRE(parsed.has_front_matter);
  REQUIRE(parsed.links.size() == 1);
  REQUIRE(parsed.links[0].label.has_value());
  REQUIRE(parsed.links[0].label.value() == "Related");
}

TEST_CASE("parse_ai_message_file falls back on yaml parse exception", "[ai_message_front_matter]") {
  const std::string raw = "---\n"
                          "message_id: [\n"
                          "---\n"
                          "Body\n";
  const auto parsed = holder::core::parse_ai_message_file(raw);
  REQUIRE_FALSE(parsed.has_front_matter);
  REQUIRE(parsed.body == raw);
}
