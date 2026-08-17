#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "ai/AiMessagePaths.h"

#include <stdexcept>
#include <string>

TEST_CASE("ai_message_rel_path shards by message_id", "[ai_message_paths]") {
  REQUIRE(holder::core::ai_message_rel_path("abcd1234") == "ai_messages/ab/cd/abcd1234.md");
}

TEST_CASE("ai_message_trash_rel_path uses trash prefix", "[ai_message_paths]") {
  REQUIRE(
      holder::core::ai_message_trash_rel_path("abcd1234") == "trash/ai_messages/ab/cd/abcd1234.md"
  );
}

TEST_CASE("ai_message_rel_path rejects short ids", "[ai_message_paths]") {
  bool threw = false;
  try {
    (void)holder::core::ai_message_rel_path("abc");
  } catch (const std::runtime_error& e) {
    threw = true;
    REQUIRE(std::string(e.what()) == "message_id too short for path sharding");
  }
  REQUIRE(threw);
}
