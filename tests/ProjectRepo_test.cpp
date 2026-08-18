#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "model/Project.h"
#include "platform/Db.h"
#include "project/ProjectRepo.h"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace {

std::filesystem::path find_schema_sql() {
#ifdef SCHEMA_SQL_PATH
  std::filesystem::path p = SCHEMA_SQL_PATH;
  if (std::filesystem::exists(p)) return p;
#endif
  namespace fs = std::filesystem;
  fs::path p1 = fs::current_path() / "schema" / "schema.sql";
  if (fs::exists(p1)) return p1;
  fs::path p2 = fs::current_path().parent_path() / "schema" / "schema.sql";
  if (fs::exists(p2)) return p2;
  throw std::runtime_error("schema.sql not found for tests");
}

std::filesystem::path make_temp_dir() {
  const auto base = std::filesystem::temp_directory_path();
  const auto suffix = std::to_string(
      static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())
  );
  auto dir = base / ("holder_project_test_" + suffix);
  std::filesystem::create_directories(dir);
  return dir;
}

void apply_schema(holder::platform::Db& db) {
  const auto schema_path = find_schema_sql();
  std::ifstream in(schema_path);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);
}

holder::model::Project make_project(const std::string& id, long long ts = 10) {
  holder::model::Project project;
  project.project_id = id;
  project.name = "Name-" + id;
  project.root_path = "/tmp/" + id;
  project.created_at = ts;
  project.updated_at = ts;
  return project;
}

int deny_project_mutations(void*, int action, const char* detail1, const char*, const char*, const char*) {
  if ((action == SQLITE_INSERT || action == SQLITE_UPDATE || action == SQLITE_DELETE) &&
      detail1 != nullptr && std::string(detail1) == "projects") {
    return SQLITE_DENY;
  }
  return SQLITE_OK;
}

int always_interrupt(void*) { return 1; }

} // namespace

TEST_CASE("ProjectRepo CRUD", "[projectrepo]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);

  holder::project::ProjectRepo repo(db);

  holder::model::Project project;
  project.project_id = "proj-1";
  project.name = "Alpha";
  project.root_path = "/tmp/alpha";
  project.created_at = 10;
  project.updated_at = 10;

  repo.create(project);

  const auto fetched = repo.get("proj-1");
  REQUIRE(fetched.has_value());
  REQUIRE(fetched->name == "Alpha");
  REQUIRE(fetched->root_path == "/tmp/alpha");
  REQUIRE(fetched->privacy_mode == "encrypted_git");
  REQUIRE_FALSE(fetched->project_key_id.has_value());

  auto list = repo.list();
  REQUIRE(list.size() == 1);
  REQUIRE(list[0].project_id == "proj-1");

  repo.update_name("proj-1", "Beta", 20);
  const auto updated_name = repo.get("proj-1");
  REQUIRE(updated_name.has_value());
  REQUIRE(updated_name->name == "Beta");
  REQUIRE(updated_name->updated_at == 20);

  repo.update_root_path("proj-1", "/tmp/beta", 30);
  const auto updated_root = repo.get("proj-1");
  REQUIRE(updated_root.has_value());
  REQUIRE(updated_root->root_path == "/tmp/beta");
  REQUIRE(updated_root->updated_at == 30);

  repo.update_git_remote("proj-1", std::optional<std::string>("git@github.com:me/repo.git"), 35);
  repo.update_git_provider("proj-1", std::optional<std::string>("github"), 36);
  const auto updated_git = repo.get("proj-1");
  REQUIRE(updated_git.has_value());
  REQUIRE(updated_git->git_remote_url.has_value());
  REQUIRE(updated_git->git_remote_url.value() == "git@github.com:me/repo.git");
  REQUIRE(updated_git->git_provider.has_value());
  REQUIRE(updated_git->git_provider.value() == "github");

  repo.update_git_remote("proj-1", std::nullopt, 37);
  repo.update_git_provider("proj-1", std::nullopt, 38);
  const auto cleared_git = repo.get("proj-1");
  REQUIRE(cleared_git.has_value());
  REQUIRE_FALSE(cleared_git->git_remote_url.has_value());
  REQUIRE_FALSE(cleared_git->git_provider.has_value());

  repo.update_privacy_mode("proj-1", "plain", 39);
  repo.update_project_key_id("proj-1", std::optional<std::string>("proj-key-1"), 40);
  const auto updated_privacy = repo.get("proj-1");
  REQUIRE(updated_privacy.has_value());
  REQUIRE(updated_privacy->privacy_mode == "plain");
  REQUIRE(updated_privacy->project_key_id.has_value());
  REQUIRE(updated_privacy->project_key_id.value() == "proj-key-1");

  repo.update_project_key_id("proj-1", std::nullopt, 41);
  const auto cleared_key = repo.get("proj-1");
  REQUIRE(cleared_key.has_value());
  REQUIRE_FALSE(cleared_key->project_key_id.has_value());

  repo.touch_updated("proj-1", 42);
  const auto touched = repo.get("proj-1");
  REQUIRE(touched.has_value());
  REQUIRE(touched->updated_at == 42);

  repo.remove("proj-1");
  REQUIRE_FALSE(repo.get("proj-1").has_value());
  list = repo.list();
  REQUIRE(list.empty());
}

TEST_CASE("ProjectRepo throws prepare failures when DB handle is missing", "[projectrepo]") {
  holder::platform::Db db;
  holder::project::ProjectRepo repo(db);
  const auto project = make_project("missing-db");

  REQUIRE_THROWS_WITH(
      repo.create(project),
      Catch::Matchers::ContainsSubstring("prepare insert project failed")
  );
  REQUIRE_THROWS_WITH(
      repo.get("p1"),
      Catch::Matchers::ContainsSubstring("prepare get project failed")
  );
  REQUIRE_THROWS_WITH(
      repo.list(),
      Catch::Matchers::ContainsSubstring("prepare list projects failed")
  );
  REQUIRE_THROWS_WITH(
      repo.update_name("p1", "n", 1),
      Catch::Matchers::ContainsSubstring("prepare update project name failed")
  );
  REQUIRE_THROWS_WITH(
      repo.update_root_path("p1", "/tmp/x", 1),
      Catch::Matchers::ContainsSubstring("prepare update project root failed")
  );
  REQUIRE_THROWS_WITH(
      repo.update_git_remote("p1", std::optional<std::string>("x"), 1),
      Catch::Matchers::ContainsSubstring("prepare update git remote failed")
  );
  REQUIRE_THROWS_WITH(
      repo.update_git_provider("p1", std::optional<std::string>("x"), 1),
      Catch::Matchers::ContainsSubstring("prepare update git provider failed")
  );
  REQUIRE_THROWS_WITH(
      repo.update_privacy_mode("p1", "plain", 1),
      Catch::Matchers::ContainsSubstring("prepare update privacy mode failed")
  );
  REQUIRE_THROWS_WITH(
      repo.update_project_key_id("p1", std::optional<std::string>("k"), 1),
      Catch::Matchers::ContainsSubstring("prepare update project key id failed")
  );
  REQUIRE_THROWS_WITH(
      repo.touch_updated("p1", 1),
      Catch::Matchers::ContainsSubstring("prepare touch project failed")
  );
  REQUIRE_THROWS_WITH(
      repo.remove("p1"),
      Catch::Matchers::ContainsSubstring("prepare delete project failed")
  );
}

TEST_CASE("ProjectRepo throws when sqlite step fails", "[projectrepo]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  holder::project::ProjectRepo repo(db);

  repo.create(make_project("p1", 1));

  sqlite3* raw = db.handle();
  REQUIRE(raw != nullptr);

  REQUIRE(sqlite3_set_authorizer(raw, deny_project_mutations, nullptr) == SQLITE_OK);
  REQUIRE_THROWS_WITH(
      repo.create(make_project("p2", 2)),
      Catch::Matchers::ContainsSubstring("insert project failed")
  );
  REQUIRE_THROWS_WITH(
      repo.update_name("p1", "Blocked", 3),
      Catch::Matchers::ContainsSubstring("update project name failed")
  );
  REQUIRE_THROWS_WITH(
      repo.update_root_path("p1", "/tmp/blocked", 3),
      Catch::Matchers::ContainsSubstring("update project root failed")
  );
  REQUIRE_THROWS_WITH(
      repo.update_git_remote("p1", std::optional<std::string>("git@x:y/z"), 3),
      Catch::Matchers::ContainsSubstring("update git remote failed")
  );
  REQUIRE_THROWS_WITH(
      repo.update_git_provider("p1", std::optional<std::string>("github"), 3),
      Catch::Matchers::ContainsSubstring("update git provider failed")
  );
  REQUIRE_THROWS_WITH(
      repo.update_privacy_mode("p1", "plain", 3),
      Catch::Matchers::ContainsSubstring("update privacy mode failed")
  );
  REQUIRE_THROWS_WITH(
      repo.update_project_key_id("p1", std::optional<std::string>("kid"), 3),
      Catch::Matchers::ContainsSubstring("update project key id failed")
  );
  REQUIRE_THROWS_WITH(
      repo.touch_updated("p1", 3),
      Catch::Matchers::ContainsSubstring("touch project failed")
  );
  REQUIRE_THROWS_WITH(
      repo.remove("p1"),
      Catch::Matchers::ContainsSubstring("delete project failed")
  );
  REQUIRE(sqlite3_set_authorizer(raw, nullptr, nullptr) == SQLITE_OK);

  sqlite3_progress_handler(raw, 1, always_interrupt, nullptr);
  REQUIRE_THROWS_WITH(repo.get("p1"), Catch::Matchers::ContainsSubstring("get project failed"));
  REQUIRE_THROWS_WITH(repo.list(), Catch::Matchers::ContainsSubstring("list projects failed"));
  sqlite3_progress_handler(raw, 0, nullptr, nullptr);
}
