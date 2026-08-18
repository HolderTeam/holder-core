#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "core_test_helpers.h"
#include "model/Project.h"
#include "platform/Db.h"
#include "project/ProjectRepo.h"

#include <holder/holder.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::string read_schema_sql() {
  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void seed_project(const std::filesystem::path& data_dir) {
  const auto db_path = data_dir / "server" / "holder.db";
  std::filesystem::create_directories(db_path.parent_path());
  holder::platform::Db db;
  db.open(db_path);
  db.exec(read_schema_sql());

  holder::model::Project project;
  project.project_id = "project-1";
  project.name = "Home";
  project.root_path = "/tmp/home";
  project.git_remote_url = "git@example.com:holder/home.git";
  project.git_provider = "github";
  project.created_at = 10;
  project.updated_at = 20;

  holder::project::ProjectRepo repo(db);
  repo.create(project);
}

} // namespace

TEST_CASE("C API exposes core version", "[capi]") {
  REQUIRE(std::string(holder_version_string()) == EXPECTED_HOLDER_VERSION_STRING);
  REQUIRE(holder_version_major() == EXPECTED_HOLDER_VERSION_MAJOR);
  REQUIRE(holder_version_minor() == EXPECTED_HOLDER_VERSION_MINOR);
  REQUIRE(holder_version_patch() == EXPECTED_HOLDER_VERSION_PATCH);
}

TEST_CASE("C API reports invalid context open arguments", "[capi]") {
  holder_error* error = nullptr;
  const int rc = holder_context_open(nullptr, nullptr, nullptr, &error);
  REQUIRE(rc == HOLDER_ERROR_INVALID_ARGUMENT);
  REQUIRE(error != nullptr);
  REQUIRE(std::string(holder_error_message(error)).find("out_context") != std::string::npos);
  holder_error_destroy(error);
}

TEST_CASE("C API opens context and lists empty projects", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);
  REQUIRE(context != nullptr);
  REQUIRE(error == nullptr);

  char* json = nullptr;
  REQUIRE(holder_project_list(context, &json, &error) == HOLDER_OK);
  REQUIRE(json != nullptr);
  REQUIRE(error == nullptr);
  REQUIRE(nlohmann::json::parse(json).empty());

  holder_string_free(json);
  holder_context_destroy(context);
}

TEST_CASE("C API lists projects as JSON", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  seed_project(data_dir);

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  const auto schema = read_schema_sql();
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(holder_project_list(context, &json, &error) == HOLDER_OK);
  const auto body = nlohmann::json::parse(json);
  REQUIRE(body.size() == 1);
  REQUIRE(body[0]["project_id"] == "project-1");
  REQUIRE(body[0]["name"] == "Home");
  REQUIRE(body[0]["root_path"] == "/tmp/home");
  REQUIRE(body[0]["git_remote_url"] == "git@example.com:holder/home.git");
  REQUIRE(body[0]["git_provider"] == "github");
  REQUIRE(body[0]["privacy_mode"] == "encrypted_git");
  REQUIRE(body[0]["project_key_id"].is_null());
  REQUIRE(body[0]["created_at"] == 10);
  REQUIRE(body[0]["updated_at"] == 20);

  holder_string_free(json);
  holder_context_destroy(context);
}
