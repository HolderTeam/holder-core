#include "identity/Uuid.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <set>
#include <string>

namespace {

bool is_lower_hex(char value) {
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
}

bool is_canonical_uuid_v4(const std::string& value) {
  if (value.size() != 36) return false;

  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (value[index] != '-') return false;
    } else if (!is_lower_hex(value[index])) {
      return false;
    }
  }

  return value[14] == '4' &&
         (value[19] == '8' || value[19] == '9' || value[19] == 'a' || value[19] == 'b');
}

} // namespace

TEST_CASE("UUID generator returns canonical version 4 UUIDs", "[identity][uuid]") {
  std::set<std::string> generated;

  for (int i = 0; i < 64; ++i) {
    const auto uuid = holder::identity::uuid_v4();
    REQUIRE(is_canonical_uuid_v4(uuid));
    REQUIRE(generated.insert(uuid).second);
  }
}
