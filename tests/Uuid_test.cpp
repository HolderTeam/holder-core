#include "identity/Uuid.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <set>
#include <string>
#include <thread>

namespace {

bool is_lower_hex(char value) {
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
}

bool has_canonical_uuid_format(const std::string& value) {
  if (value.size() != 36) return false;

  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (value[index] != '-') return false;
    } else if (!is_lower_hex(value[index])) {
      return false;
    }
  }

  return true;
}

bool has_rfc_variant(const std::string& value) {
  return value[19] == '8' || value[19] == '9' || value[19] == 'a' || value[19] == 'b';
}

} // namespace

TEST_CASE("UUID generator returns canonical version 4 UUIDs", "[identity][uuid]") {
  std::set<std::string> generated;

  for (int i = 0; i < 64; ++i) {
    const auto uuid = holder::identity::uuid_v4();
    REQUIRE(has_canonical_uuid_format(uuid));
    REQUIRE(uuid[14] == '4');
    REQUIRE(has_rfc_variant(uuid));
    REQUIRE(generated.insert(uuid).second);
  }
}

TEST_CASE("UUIDv7 generator returns canonical, unique version 7 UUIDs", "[identity][uuid]") {
  std::set<std::string> generated;

  for (int i = 0; i < 64; ++i) {
    const auto uuid = holder::identity::uuid_v7();
    REQUIRE(has_canonical_uuid_format(uuid));
    REQUIRE(uuid[14] == '7');
    REQUIRE(has_rfc_variant(uuid));
    REQUIRE(generated.insert(uuid).second);
  }
}

TEST_CASE("UUIDv7 values sort in timestamp order", "[identity][uuid]") {
  const auto earlier = holder::identity::uuid_v7();
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  const auto later = holder::identity::uuid_v7();

  REQUIRE(earlier.substr(0, 13) < later.substr(0, 13));
  REQUIRE(earlier < later);
}

TEST_CASE("generate_id dispatches UUIDv4 schemes", "[identity][uuid]") {
  const auto uuid = holder::identity::generate_id(holder::model::IdScheme::Uuid4);

  REQUIRE(has_canonical_uuid_format(uuid));
  REQUIRE(uuid[14] == '4');
  REQUIRE(has_rfc_variant(uuid));
}

TEST_CASE("generate_id dispatches UUIDv7 schemes", "[identity][uuid]") {
  const auto uuid = holder::identity::generate_id(holder::model::IdScheme::Uuid7);

  REQUIRE(has_canonical_uuid_format(uuid));
  REQUIRE(uuid[14] == '7');
  REQUIRE(has_rfc_variant(uuid));
}

TEST_CASE("UUID validation accepts Holder UUIDv4 and UUIDv7 identities", "[identity][uuid]") {
  REQUIRE(holder::identity::is_valid_uuid("550e8400-e29b-41d4-a716-446655440000"));
  REQUIRE(holder::identity::is_valid_uuid("01890f3e-7b5a-7cc8-98c4-dc0c0c07398f"));
}

TEST_CASE("UUID validation rejects malformed and unsupported identities", "[identity][uuid]") {
  SECTION("malformed structure") {
    REQUIRE_FALSE(holder::identity::is_valid_uuid("not-a-uuid"));
    REQUIRE_FALSE(holder::identity::is_valid_uuid("550e8400e29b-41d4-a716-446655440000"));
    REQUIRE_FALSE(holder::identity::is_valid_uuid("550e8400-e29b-41d4-a716-44665544000z"));
  }

  SECTION("unsupported version") {
    REQUIRE_FALSE(holder::identity::is_valid_uuid("550e8400-e29b-51d4-a716-446655440000"));
  }

  SECTION("invalid RFC variant") {
    REQUIRE_FALSE(holder::identity::is_valid_uuid("550e8400-e29b-41d4-7716-446655440000"));
  }
}
