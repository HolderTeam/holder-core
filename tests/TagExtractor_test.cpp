#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "card/TagExtractor.h"

#include <string>
#include <vector>

using holder::core::extract_tags;
using holder::core::extract_tag_occurrences;

TEST_CASE("extract_tags finds a single bare tag", "[tag_extractor]") {
  REQUIRE(extract_tags("#todo") == std::vector<std::string>{"todo"});
}

TEST_CASE("extract_tags finds a tag mid-paragraph", "[tag_extractor]") {
  REQUIRE(extract_tags("Need to fix #android today.") == std::vector<std::string>{"android"});
}

TEST_CASE("extract_tags finds multiple tags on one line, in order", "[tag_extractor]") {
  REQUIRE(
      extract_tags("This affects #sync and #security.") ==
      std::vector<std::string>{"sync", "security"}
  );
}

TEST_CASE("extract_tag_occurrences returns exact byte ranges and repeats", "[tag_extractor]") {
  const std::string body = "é #Todo then #sync and #todo";
  const auto occurrences = extract_tag_occurrences(body);
  REQUIRE(occurrences.size() == 3);
  REQUIRE(occurrences[0].tag == "todo");
  REQUIRE(body.substr(occurrences[0].byte_start, occurrences[0].byte_end - occurrences[0].byte_start) == "#Todo");
  REQUIRE(occurrences[1].tag == "sync");
  REQUIRE(body.substr(occurrences[1].byte_start, occurrences[1].byte_end - occurrences[1].byte_start) == "#sync");
  REQUIRE(occurrences[2].tag == "todo");
}

TEST_CASE("extract_tag_occurrences omits Markdown code and other hash uses", "[tag_extractor]") {
  const std::string body = "# Heading\n`#nope` #yes\n```\n#also-nope\n```\nfoo#bar https://x/#frag";
  const auto occurrences = extract_tag_occurrences(body);
  REQUIRE(occurrences.size() == 1);
  REQUIRE(occurrences[0].tag == "yes");
  REQUIRE(body.substr(occurrences[0].byte_start, occurrences[0].byte_end - occurrences[0].byte_start) == "#yes");
}

TEST_CASE("extract_tags does not treat an ATX heading marker as a tag", "[tag_extractor]") {
  REQUIRE(extract_tags("# Heading").empty());
  REQUIRE(extract_tags("## Another heading").empty());
  REQUIRE(extract_tags("   # Heading with leading spaces").empty());
}

TEST_CASE("extract_tags finds a tag inside heading text", "[tag_extractor]") {
  REQUIRE(extract_tags("## Fix #android issue") == std::vector<std::string>{"android"});
}

TEST_CASE("extract_tags allows hyphen and underscore", "[tag_extractor]") {
  REQUIRE(
      extract_tags("#android-sync and #android_sync") ==
      std::vector<std::string>{"android-sync", "android_sync"}
  );
}

TEST_CASE("extract_tags allows a slash for nested-style tags", "[tag_extractor]") {
  REQUIRE(extract_tags("#area/sub-topic") == std::vector<std::string>{"area/sub-topic"});
}

TEST_CASE("extract_tags lowercases and de-duplicates by normalized form", "[tag_extractor]") {
  REQUIRE(
      extract_tags("#TODO something, then #todo again, then #ToDo once more.") ==
      std::vector<std::string>{"todo"}
  );
}

TEST_CASE("extract_tags rejects C# and F#", "[tag_extractor]") {
  REQUIRE(extract_tags("C#").empty());
  REQUIRE(extract_tags("I love F# a lot").empty());
}

TEST_CASE("extract_tags rejects a hash with no boundary before it", "[tag_extractor]") {
  REQUIRE(extract_tags("foo#bar").empty());
}

TEST_CASE("extract_tags rejects a purely numeric run (no letter to start)", "[tag_extractor]") {
  REQUIRE(extract_tags("issue #123").empty());
}

TEST_CASE("extract_tags rejects hex-color-length all-hex-digit candidates", "[tag_extractor]") {
  REQUIRE(extract_tags("colour #ff8800").empty());
  REQUIRE(extract_tags("#fff").empty());
  REQUIRE(extract_tags("#FFFF").empty());
  REQUIRE(extract_tags("#ffffff").empty());
  REQUIRE(extract_tags("#FFFFFFFF").empty());
}

TEST_CASE("extract_tags keeps a tag that merely happens to be hex-charset but wrong length", "[tag_extractor]") {
  // 5 hex-looking characters -- not a valid CSS color length, so it's a real tag.
  REQUIRE(extract_tags("#deadb") == std::vector<std::string>{"deadb"});
}

TEST_CASE("extract_tags ignores a tag-looking string inside inline code", "[tag_extractor]") {
  REQUIRE(extract_tags("`#not-a-tag`").empty());
}

TEST_CASE("extract_tags ignores a tag-looking string inside a fenced code block", "[tag_extractor]") {
  REQUIRE(extract_tags("```\n#not-a-tag\n```").empty());
}

TEST_CASE("extract_tags ignores a tag-looking string inside an indented code block", "[tag_extractor]") {
  REQUIRE(extract_tags("    #not-a-tag").empty());
}

TEST_CASE("extract_tags still finds tags in prose around a code block", "[tag_extractor]") {
  const std::string markdown = "Before #alpha.\n\n```\n#not-a-tag\n```\n\nAfter #beta.\n";
  REQUIRE(extract_tags(markdown) == std::vector<std::string>{"alpha", "beta"});
}

TEST_CASE("extract_tags finds a tag at the very start of the document", "[tag_extractor]") {
  REQUIRE(extract_tags("#urgent needs doing") == std::vector<std::string>{"urgent"});
}

TEST_CASE("extract_tags stops a tag at trailing punctuation", "[tag_extractor]") {
  REQUIRE(extract_tags("Check #todo, then ship.") == std::vector<std::string>{"todo"});
}

TEST_CASE("extract_tags returns nothing for an empty body", "[tag_extractor]") {
  REQUIRE(extract_tags("").empty());
}

TEST_CASE("extract_tags returns nothing for a body with no candidate hashes", "[tag_extractor]") {
  REQUIRE(extract_tags("Just some ordinary prose with no hashes at all.").empty());
}
