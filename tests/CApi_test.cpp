#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "card/CardRepo.h"
#include "core_test_helpers.h"
#include "model/Card.h"
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

void seed_project_with_cards(const std::filesystem::path& data_dir) {
  seed_project(data_dir);

  const auto db_path = data_dir / "server" / "holder.db";
  holder::platform::Db db;
  db.open(db_path);

  holder::model::Card root;
  root.card_id = "card-1";
  root.project_id = "project-1";
  root.title = "Welcome";
  root.rel_path = "Welcome.md";
  root.sort_key = 1.0;
  root.created_at = 30;
  root.updated_at = 40;

  holder::model::Card child;
  child.card_id = "card-2";
  child.project_id = "project-1";
  child.title = "Second";
  child.rel_path = "Second.md";
  child.parent_card_id = root.card_id;
  child.sort_key = 2.0;
  child.created_at = 50;
  child.updated_at = 60;

  holder::card::CardRepo repo(db);
  repo.create(root);
  repo.create(child);
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

TEST_CASE("C API lists empty cards as JSON", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(holder_card_list(context, "project-1", &json, &error) == HOLDER_OK);
  REQUIRE(json != nullptr);
  REQUIRE(error == nullptr);
  REQUIRE(nlohmann::json::parse(json).empty());

  holder_string_free(json);
  holder_context_destroy(context);
}

TEST_CASE("C API lists cards as JSON", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  seed_project_with_cards(data_dir);

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  const auto schema = read_schema_sql();
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(holder_card_list(context, "project-1", &json, &error) == HOLDER_OK);
  const auto body = nlohmann::json::parse(json);
  REQUIRE(body.size() == 2);
  REQUIRE(body[0]["card_id"] == "card-1");
  REQUIRE(body[0]["project_id"] == "project-1");
  REQUIRE(body[0]["title"] == "Welcome");
  REQUIRE(body[0]["rel_path"] == "Welcome.md");
  REQUIRE(body[0]["parent_card_id"].is_null());
  REQUIRE(body[0]["sort_key"] == 1.0);
  REQUIRE(body[0]["created_at"] == 30);
  REQUIRE(body[0]["updated_at"] == 40);
  REQUIRE(body[0]["deleted_at"].is_null());

  REQUIRE(body[1]["card_id"] == "card-2");
  REQUIRE(body[1]["parent_card_id"] == "card-1");

  holder_string_free(json);
  holder_context_destroy(context);
}

TEST_CASE("C API reports invalid card list arguments", "[capi]") {
  holder_error* error = nullptr;
  char* json = nullptr;

  REQUIRE(holder_card_list(nullptr, "project-1", &json, &error) == HOLDER_ERROR_INVALID_ARGUMENT);
  REQUIRE(json == nullptr);
  REQUIRE(error != nullptr);
  REQUIRE(std::string(holder_error_message(error)).find("context") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  REQUIRE(holder_card_list(context, "", &json, &error) == HOLDER_ERROR_INVALID_ARGUMENT);
  REQUIRE(error != nullptr);
  REQUIRE(std::string(holder_error_message(error)).find("project_id") != std::string::npos);

  holder_error_destroy(error);
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
