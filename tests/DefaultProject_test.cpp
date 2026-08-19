#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "card/CardRepo.h"
#include "core_test_helpers.h"
#include "model/Project.h"
#include "project/DefaultProject.h"
#include "project/ProjectRepo.h"

#include <functional>
#include <string>

namespace {

std::function<std::string()> counting_uuid_v4(const std::string& prefix) {
  return [prefix, count = 0]() mutable {
    return prefix + "-" + std::to_string(++count);
  };
}

} // namespace

TEST_CASE("ensure_default_project creates a project and welcome card when empty", "[default_project]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  const auto created = holder::project::ensure_default_project(
      db,
      "Home",
      "plain",
      "Welcome",
      "# Welcome to Holder\n",
      counting_uuid_v4("id"),
      dir / "projects"
  );

  REQUIRE(created.has_value());
  REQUIRE(created->name == "Home");
  REQUIRE(created->privacy_mode == "plain");

  holder::card::CardRepo card_repo(db);
  const auto cards = card_repo.list_all(created->project_id);
  REQUIRE(cards.size() == 1);
  REQUIRE(cards[0].title == "Welcome");
}

TEST_CASE("ensure_default_project does nothing when a project already exists", "[default_project]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  holder::project::ProjectRepo repo(db);
  holder::model::Project existing;
  existing.project_id = "existing";
  existing.name = "Existing";
  existing.root_path = (dir / "existing").string();
  existing.created_at = 1;
  existing.updated_at = 1;
  repo.create(existing);

  const auto created = holder::project::ensure_default_project(
      db,
      "Home",
      "plain",
      "Welcome",
      "# Welcome to Holder\n",
      counting_uuid_v4("id"),
      dir / "projects"
  );

  REQUIRE_FALSE(created.has_value());
  REQUIRE(repo.list().size() == 1);
}
