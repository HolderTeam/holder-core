#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "model/Project.h"
#include "model/Resource.h"
#include "platform/Db.h"
#include "project/ProjectRepo.h"
#include "resource/ResourceRepo.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sqlite3.h>
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
  auto dir = base / ("holder_resource_test_" + suffix);
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

void create_project(holder::platform::Db& db, const std::string& project_id) {
  holder::project::ProjectRepo repo(db);
  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Project";
  project.root_path = "/tmp/project";
  project.created_at = 1;
  project.updated_at = 1;
  repo.create(project);
}

int sqlite_interrupt_cb(void* data) {
  auto* flag = static_cast<int*>(data);
  return (flag && *flag) ? 1 : 0;
}

} // namespace

TEST_CASE("ResourceRepo add/list/remove", "[resourcerepo]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  create_project(db, "proj-1");

  holder::resource::ResourceRepo repo(db);

  holder::model::Resource resource;
  resource.resource_id = "res-1";
  resource.project_id = "proj-1";
  resource.kind = "url";
  resource.uri = "https://example.com";
  resource.label = "Example";
  resource.desc = "Docs";
  resource.created_at = 10;
  resource.updated_at = 10;

  repo.add(resource);

  auto list = repo.list("proj-1");
  REQUIRE(list.size() == 1);
  REQUIRE(list[0].resource_id == "res-1");
  REQUIRE(list[0].desc.has_value());

  const auto fetched = repo.get("res-1");
  REQUIRE(fetched.has_value());
  REQUIRE(fetched->uri == "https://example.com");

  repo.remove("res-1");
  list = repo.list("proj-1");
  REQUIRE(list.empty());
}

TEST_CASE("ResourceRepo get returns nullopt for missing resource", "[resourcerepo]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  create_project(db, "proj-1");

  holder::resource::ResourceRepo repo(db);
  const auto missing = repo.get("does-not-exist");
  REQUIRE(!missing.has_value());
}

TEST_CASE("ResourceRepo methods throw when DB handle is closed", "[resourcerepo]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  create_project(db, "proj-1");

  holder::resource::ResourceRepo repo(db);
  db.close();

  holder::model::Resource resource;
  resource.resource_id = "res-closed";
  resource.project_id = "proj-1";
  resource.kind = "url";
  resource.uri = "https://example.com";
  resource.label = "Example";
  resource.desc = std::nullopt;
  resource.created_at = 10;
  resource.updated_at = 11;

  REQUIRE_THROWS(repo.add(resource));
  REQUIRE_THROWS(repo.list("proj-1"));
  REQUIRE_THROWS(repo.get("res-closed"));
  REQUIRE_THROWS(repo.update(resource));
  REQUIRE_THROWS(repo.remove("res-closed"));
}

TEST_CASE("ResourceRepo add throws on duplicate resource_id", "[resourcerepo]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  create_project(db, "proj-1");

  holder::resource::ResourceRepo repo(db);

  holder::model::Resource resource;
  resource.resource_id = "res-dup";
  resource.project_id = "proj-1";
  resource.kind = "url";
  resource.uri = "https://example.com";
  resource.label = "Example";
  resource.desc = "Docs";
  resource.created_at = 10;
  resource.updated_at = 10;

  repo.add(resource);
  REQUIRE_THROWS(repo.add(resource));
}

TEST_CASE(
    "ResourceRepo list/get/update/remove throw on interrupted sqlite step",
    "[resourcerepo]"
) {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  create_project(db, "proj-1");

  holder::resource::ResourceRepo repo(db);
  holder::model::Resource resource;
  resource.resource_id = "res-int";
  resource.project_id = "proj-1";
  resource.kind = "url";
  resource.uri = "https://example.com";
  resource.label = "Example";
  resource.desc = std::nullopt;
  resource.created_at = 10;
  resource.updated_at = 10;
  repo.add(resource);

  int interrupt_on = 1;
  sqlite3_progress_handler(db.handle(), 1, sqlite_interrupt_cb, &interrupt_on);
  REQUIRE_THROWS(repo.list("proj-1"));
  REQUIRE_THROWS(repo.get("res-int"));

  resource.updated_at = 11;
  REQUIRE_THROWS(repo.update(resource));
  REQUIRE_THROWS(repo.remove("res-int"));
  sqlite3_progress_handler(db.handle(), 0, nullptr, nullptr);
}

TEST_CASE("ResourceRepo list throws sqlite error branch under heavy interrupt", "[resourcerepo]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  create_project(db, "proj-1");

  holder::resource::ResourceRepo repo(db);

  // Seed enough rows so query iteration reliably hits interrupted sqlite_step.
  for (int i = 0; i < 600; ++i) {
    holder::model::Resource resource;
    resource.resource_id = "res-heavy-" + std::to_string(i);
    resource.project_id = "proj-1";
    resource.kind = "url";
    resource.uri = "https://example.com/" + std::to_string(i);
    resource.label = "Example";
    resource.desc = (i % 2 == 0) ? std::optional<std::string>("Docs") : std::nullopt;
    resource.created_at = i + 1;
    resource.updated_at = i + 1;
    repo.add(resource);
  }

  int interrupt_on = 1;
  sqlite3_progress_handler(db.handle(), 1, sqlite_interrupt_cb, &interrupt_on);
  REQUIRE_THROWS(repo.list("proj-1"));
  sqlite3_progress_handler(db.handle(), 0, nullptr, nullptr);
}

TEST_CASE(
    "ResourceRepo add throws sqlite error branch on interrupted insert step",
    "[resourcerepo]"
) {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  create_project(db, "proj-1");

  holder::resource::ResourceRepo repo(db);

  holder::model::Resource resource;
  resource.resource_id = "res-int-add";
  resource.project_id = "proj-1";
  resource.kind = "url";
  resource.uri = "https://example.com/interrupted";
  resource.label = "Example";
  resource.desc = std::nullopt;
  resource.created_at = 10;
  resource.updated_at = 10;

  int interrupt_on = 1;
  sqlite3_progress_handler(db.handle(), 1, sqlite_interrupt_cb, &interrupt_on);
  REQUIRE_THROWS(repo.add(resource));
  sqlite3_progress_handler(db.handle(), 0, nullptr, nullptr);
}

TEST_CASE(
    "ResourceRepo list throws sqlite error branch when resources table is missing",
    "[resourcerepo]"
) {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  create_project(db, "proj-1");

  holder::resource::ResourceRepo repo(db);
  db.exec("DROP TABLE resources;");

  REQUIRE_THROWS(repo.list("proj-1"));
}
