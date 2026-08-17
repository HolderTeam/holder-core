#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "card/CardPaths.h"

TEST_CASE("card_rel_path shards by first 4 characters", "[cardpaths]") {
  const std::string path = holder::core::card_rel_path("abcd1234");
  REQUIRE(path == "cards/ab/cd/abcd1234.md");
}

TEST_CASE("card_trash_rel_path uses trash prefix", "[cardpaths]") {
  const std::string path = holder::core::card_trash_rel_path("abcd1234");
  REQUIRE(path == "trash/cards/ab/cd/abcd1234.md");
}

TEST_CASE("card_rel_path rejects short ids", "[cardpaths]") {
  REQUIRE_THROWS(holder::core::card_rel_path("abc"));
}
