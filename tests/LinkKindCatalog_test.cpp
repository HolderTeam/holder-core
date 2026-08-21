#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "card/LinkKindCatalog.h"

#include <algorithm>
#include <set>

using holder::core::link_kind_catalog;

TEST_CASE("link_kind_catalog has exactly 100 entries", "[link_kind_catalog]") {
  REQUIRE(link_kind_catalog().size() == 100);
}

TEST_CASE("link_kind_catalog ids are unique", "[link_kind_catalog]") {
  std::set<std::string> ids;
  for (const auto& entry : link_kind_catalog()) {
    ids.insert(entry.id);
  }
  REQUIRE(ids.size() == link_kind_catalog().size());
}

TEST_CASE("link_kind_catalog entries have non-empty labels", "[link_kind_catalog]") {
  for (const auto& entry : link_kind_catalog()) {
    REQUIRE_FALSE(entry.id.empty());
    REQUIRE_FALSE(entry.forward_label.empty());
    REQUIRE_FALSE(entry.reverse_label.empty());
  }
}

namespace {

const holder::core::LinkKindInfo& find(const std::string& id) {
  const auto& catalog = link_kind_catalog();
  auto it = std::find_if(
      catalog.begin(), catalog.end(), [&id](const auto& entry) { return entry.id == id; }
  );
  REQUIRE(it != catalog.end());
  return *it;
}

}  // namespace

TEST_CASE("link_kind_catalog spot-checks known kinds", "[link_kind_catalog]") {
  const auto& depends_on = find("depends_on");
  REQUIRE(depends_on.forward_label == "Depends on");
  REQUIRE(depends_on.reverse_label == "Required by");

  const auto& blocks = find("blocks");
  REQUIRE(blocks.forward_label == "Blocks");
  REQUIRE(blocks.reverse_label == "Blocked by");

  const auto& related_to = find("related_to");
  REQUIRE(related_to.forward_label == "Related to");
  REQUIRE(related_to.reverse_label == "Related to");
}
