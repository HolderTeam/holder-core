#include "model/IdScheme.h"
#include "model/Project.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("ID schemes use stable canonical strings", "[identity][id_scheme]") {
  using holder::model::IdScheme;

  REQUIRE(holder::model::to_string(IdScheme::Uuid4) == "uuid4");
  REQUIRE(holder::model::to_string(IdScheme::Uuid7) == "uuid7");
  REQUIRE(holder::model::id_scheme_from_string("uuid4") == IdScheme::Uuid4);
  REQUIRE(holder::model::id_scheme_from_string("uuid7") == IdScheme::Uuid7);
  REQUIRE_FALSE(holder::model::id_scheme_from_string("UUID7").has_value());
  REQUIRE_FALSE(holder::model::id_scheme_from_string("unknown").has_value());
}

TEST_CASE("Project model defaults to UUID4 for old data", "[identity][id_scheme]") {
  REQUIRE(holder::model::Project{}.id_scheme == holder::model::IdScheme::Uuid4);
}
