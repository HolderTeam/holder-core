#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "core_test_helpers.h"
#include "model/Project.h"
#include "project/ProjectRepo.h"
#include "project/ProjectStore.h"

#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>

namespace {

std::function<std::string()> counting_uuid_v4(const std::string& prefix) {
  return [prefix, count = 0]() mutable {
    return prefix + "-" + std::to_string(++count);
  };
}

} // namespace

TEST_CASE("ProjectStore create defaults id, timestamps, and root_path", "[project_store]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  holder::project::ProjectStore store(db);
  holder::model::Project input;
  input.name = "Home";
  input.privacy_mode = "plain";

  const auto created = store.create(input, counting_uuid_v4("id"), dir / "projects");

  REQUIRE(created.project_id == "id-1");
  REQUIRE(created.created_at > 0);
  REQUIRE(created.updated_at == created.created_at);
  REQUIRE(created.root_path == (dir / "projects" / "home").string());

  holder::project::ProjectRepo repo(db);
  REQUIRE(repo.list().size() == 1);
}

TEST_CASE("ProjectStore create honors an explicit root_path", "[project_store]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  holder::project::ProjectStore store(db);
  holder::model::Project input;
  input.name = "Home";
  input.privacy_mode = "plain";
  input.root_path = (dir / "custom-root").string();

  const auto created = store.create(input, counting_uuid_v4("id"), std::nullopt);

  REQUIRE(created.root_path == (dir / "custom-root").string());
}

TEST_CASE(
    "ProjectStore create throws when root_path and projects_root are both missing",
    "[project_store]"
) {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  holder::project::ProjectStore store(db);
  holder::model::Project input;
  input.name = "Home";

  bool threw = false;
  try {
    (void)store.create(input, counting_uuid_v4("id"), std::nullopt);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  REQUIRE(threw);

  holder::project::ProjectRepo repo(db);
  REQUIRE(repo.list().empty());
}

TEST_CASE("ProjectStore create sets up an encrypted project", "[project_store]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  holder::project::ProjectStore store(db);
  holder::model::Project input;
  input.name = "Home";
  input.privacy_mode = "encrypted_git";

  const auto created = store.create(input, counting_uuid_v4("id"), dir / "projects");

  REQUIRE(created.project_key_id.has_value());
  REQUIRE(std::filesystem::exists(std::filesystem::path(created.root_path) / ".holder" / "privacy.json"));
}
