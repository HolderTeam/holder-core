#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "card/CardRepo.h"
#include "core_test_helpers.h"
#include "git/GitRepo.h"
#include "model/Card.h"
#include "model/Project.h"
#include "platform/Db.h"
#include "project/ProjectRepo.h"

#include <git2.h>
#include <holder/holder.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
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

// Seeds a project whose root_path is a real, writable directory (unlike
// seed_project's placeholder /tmp/home), for tests that need to actually
// perform git operations against it.
void seed_git_project(
    const std::filesystem::path& data_dir,
    const std::string& project_id,
    const std::filesystem::path& root_path,
    const std::optional<std::string>& remote_url
) {
  const auto db_path = data_dir / "server" / "holder.db";
  std::filesystem::create_directories(db_path.parent_path());
  holder::platform::Db db;
  db.open(db_path);
  db.exec(read_schema_sql());

  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Git Project";
  project.root_path = root_path.string();
  project.git_remote_url = remote_url;
  project.created_at = 10;
  project.updated_at = 20;

  holder::project::ProjectRepo repo(db);
  repo.create(project);
}

// A bare repo, unlike GitRepo::open_or_init, accepts a non-force push to its
// currently checked-out branch -- git refuses that against a working-tree repo.
void init_bare_repo(const std::filesystem::path& repo_path) {
  git_libgit2_init();
  std::filesystem::create_directories(repo_path.parent_path());
  git_repository* repo = nullptr;
  git_repository_init_options opts{};
  REQUIRE(git_repository_init_options_init(&opts, GIT_REPOSITORY_INIT_OPTIONS_VERSION) == 0);
  opts.flags = GIT_REPOSITORY_INIT_BARE | GIT_REPOSITORY_INIT_MKPATH;
  REQUIRE(git_repository_init_ext(&repo, repo_path.string().c_str(), &opts) == 0);
  git_repository_free(repo);
  git_libgit2_shutdown();
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

TEST_CASE("C API creates a plain project defaulting root_path and privacy_mode", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(holder_project_create(context, "Home", nullptr, nullptr, &json, &error) == HOLDER_OK);
  REQUIRE(json != nullptr);
  REQUIRE(error == nullptr);

  const auto body = nlohmann::json::parse(json);
  REQUIRE(body["name"] == "Home");
  REQUIRE(body["privacy_mode"] == "plain");
  REQUIRE_FALSE(body["project_id"].get<std::string>().empty());
  const std::string project_id = body["project_id"].get<std::string>();
  const std::string root_path = body["root_path"].get<std::string>();
  REQUIRE(root_path.find("projects/home") != std::string::npos);
  // A plain project with no remote is not git-initialized until its first
  // write (e.g. a card create), to avoid creating empty repos up front.
  REQUIRE_FALSE(std::filesystem::exists(root_path));

  holder_string_free(json);

  char* card_json = nullptr;
  REQUIRE(
      holder_card_create(context, project_id.c_str(), "Welcome", nullptr, nullptr, &card_json, &error) ==
      HOLDER_OK
  );
  holder_string_free(card_json);
  REQUIRE(std::filesystem::is_directory(std::filesystem::path(root_path) / ".git"));

  char* list_json = nullptr;
  REQUIRE(holder_project_list(context, &list_json, &error) == HOLDER_OK);
  REQUIRE(nlohmann::json::parse(list_json).size() == 1);
  holder_string_free(list_json);

  holder_context_destroy(context);
}

TEST_CASE("C API reports invalid project create arguments", "[capi]") {
  holder_error* error = nullptr;
  char* json = nullptr;

  REQUIRE(holder_project_create(nullptr, "Home", nullptr, nullptr, &json, &error) == HOLDER_ERROR_INVALID_ARGUMENT);
  REQUIRE(json == nullptr);
  REQUIRE(error != nullptr);
  holder_error_destroy(error);
  error = nullptr;

  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  REQUIRE(holder_project_create(context, "", nullptr, nullptr, &json, &error) == HOLDER_ERROR_INVALID_ARGUMENT);
  REQUIRE(error != nullptr);
  REQUIRE(std::string(holder_error_message(error)).find("name") != std::string::npos);

  holder_error_destroy(error);
  holder_context_destroy(context);
}

TEST_CASE("C API renames a project", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* project_json = nullptr;
  REQUIRE(holder_project_create(context, "Home", nullptr, nullptr, &project_json, &error) == HOLDER_OK);
  const std::string project_id = nlohmann::json::parse(project_json)["project_id"].get<std::string>();
  holder_string_free(project_json);

  char* renamed_json = nullptr;
  REQUIRE(holder_project_rename(context, project_id.c_str(), "Personal", &renamed_json, &error) == HOLDER_OK);
  REQUIRE(nlohmann::json::parse(renamed_json)["name"] == "Personal");
  holder_string_free(renamed_json);

  char* list_json = nullptr;
  REQUIRE(holder_project_list(context, &list_json, &error) == HOLDER_OK);
  REQUIRE(nlohmann::json::parse(list_json)[0]["name"] == "Personal");
  holder_string_free(list_json);

  holder_context_destroy(context);
}

TEST_CASE("C API reports invalid project rename arguments", "[capi]") {
  holder_error* error = nullptr;
  char* json = nullptr;

  REQUIRE(
      holder_project_rename(nullptr, "project-1", "New Name", &json, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(json == nullptr);
  REQUIRE(error != nullptr);
  holder_error_destroy(error);
  error = nullptr;

  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  REQUIRE(
      holder_project_rename(context, "missing-project", "New Name", &json, &error) ==
      HOLDER_ERROR_RUNTIME
  );
  REQUIRE(error != nullptr);
  REQUIRE(std::string(holder_error_message(error)).find("not found") != std::string::npos);

  holder_error_destroy(error);
  holder_context_destroy(context);
}

TEST_CASE("C API deletes a project and cascades its cards", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* project_json = nullptr;
  REQUIRE(holder_project_create(context, "Home", nullptr, nullptr, &project_json, &error) == HOLDER_OK);
  const std::string project_id = nlohmann::json::parse(project_json)["project_id"].get<std::string>();
  holder_string_free(project_json);

  char* card_json = nullptr;
  REQUIRE(
      holder_card_create(context, project_id.c_str(), "Welcome", nullptr, nullptr, &card_json, &error) ==
      HOLDER_OK
  );
  holder_string_free(card_json);

  REQUIRE(holder_project_delete(context, project_id.c_str(), &error) == HOLDER_OK);

  char* list_json = nullptr;
  REQUIRE(holder_project_list(context, &list_json, &error) == HOLDER_OK);
  REQUIRE(nlohmann::json::parse(list_json).empty());
  holder_string_free(list_json);

  holder_context_destroy(context);
}

TEST_CASE("C API reports invalid project delete arguments", "[capi]") {
  holder_error* error = nullptr;

  REQUIRE(holder_project_delete(nullptr, "project-1", &error) == HOLDER_ERROR_INVALID_ARGUMENT);
  REQUIRE(error != nullptr);
  holder_error_destroy(error);
  error = nullptr;

  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  REQUIRE(holder_project_delete(context, "missing-project", &error) == HOLDER_ERROR_RUNTIME);
  REQUIRE(error != nullptr);
  REQUIRE(std::string(holder_error_message(error)).find("not found") != std::string::npos);

  holder_error_destroy(error);
  holder_context_destroy(context);
}

TEST_CASE("C API creates a card with generated id and rel_path", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* project_json = nullptr;
  REQUIRE(holder_project_create(context, "Home", nullptr, nullptr, &project_json, &error) == HOLDER_OK);
  const std::string project_id = nlohmann::json::parse(project_json)["project_id"].get<std::string>();
  holder_string_free(project_json);

  char* card_json = nullptr;
  REQUIRE(
      holder_card_create(
          context,
          project_id.c_str(),
          "Welcome",
          "# Welcome to Holder\n",
          nullptr,
          &card_json,
          &error
      ) == HOLDER_OK
  );
  REQUIRE(card_json != nullptr);
  REQUIRE(error == nullptr);

  const auto body = nlohmann::json::parse(card_json);
  REQUIRE(body["title"] == "Welcome");
  REQUIRE(body["project_id"] == project_id);
  REQUIRE(body["parent_card_id"].is_null());
  REQUIRE_FALSE(body["card_id"].get<std::string>().empty());
  REQUIRE_FALSE(body["rel_path"].get<std::string>().empty());

  holder_string_free(card_json);

  char* list_json = nullptr;
  REQUIRE(holder_card_list(context, project_id.c_str(), &list_json, &error) == HOLDER_OK);
  REQUIRE(nlohmann::json::parse(list_json).size() == 1);
  holder_string_free(list_json);

  holder_context_destroy(context);
}

TEST_CASE("C API gets a card's content", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* project_json = nullptr;
  REQUIRE(holder_project_create(context, "Home", nullptr, nullptr, &project_json, &error) == HOLDER_OK);
  const std::string project_id = nlohmann::json::parse(project_json)["project_id"].get<std::string>();
  holder_string_free(project_json);

  char* card_json = nullptr;
  REQUIRE(
      holder_card_create(
          context,
          project_id.c_str(),
          "Welcome",
          "# Welcome to Holder\n\nBody text.\n",
          nullptr,
          &card_json,
          &error
      ) == HOLDER_OK
  );
  const std::string card_id = nlohmann::json::parse(card_json)["card_id"].get<std::string>();
  holder_string_free(card_json);

  char* content = nullptr;
  REQUIRE(holder_card_get_content(context, card_id.c_str(), &content, &error) == HOLDER_OK);
  REQUIRE(content != nullptr);
  REQUIRE(error == nullptr);
  REQUIRE(std::string(content) == "# Welcome to Holder\n\nBody text.\n");

  holder_string_free(content);
  holder_context_destroy(context);
}

TEST_CASE("C API reports card not found for get_content", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* content = nullptr;
  REQUIRE(holder_card_get_content(context, "missing-card", &content, &error) == HOLDER_ERROR_RUNTIME);
  REQUIRE(content == nullptr);
  REQUIRE(error != nullptr);
  REQUIRE(std::string(holder_error_message(error)).find("not found") != std::string::npos);

  holder_error_destroy(error);
  holder_context_destroy(context);
}

TEST_CASE("C API reports invalid card get_content arguments", "[capi]") {
  holder_error* error = nullptr;
  char* content = nullptr;

  REQUIRE(
      holder_card_get_content(nullptr, "card-1", &content, &error) == HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(content == nullptr);
  REQUIRE(error != nullptr);
  holder_error_destroy(error);
  error = nullptr;

  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  REQUIRE(
      holder_card_get_content(context, "", &content, &error) == HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(error != nullptr);
  REQUIRE(std::string(holder_error_message(error)).find("card_id") != std::string::npos);

  holder_error_destroy(error);
  holder_context_destroy(context);
}

TEST_CASE("C API reports invalid card create arguments", "[capi]") {
  holder_error* error = nullptr;
  char* json = nullptr;

  REQUIRE(
      holder_card_create(nullptr, "project-1", "Welcome", nullptr, nullptr, &json, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(json == nullptr);
  REQUIRE(error != nullptr);
  holder_error_destroy(error);
  error = nullptr;

  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  REQUIRE(
      holder_card_create(context, "project-1", "", nullptr, nullptr, &json, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(error != nullptr);
  REQUIRE(std::string(holder_error_message(error)).find("title") != std::string::npos);

  holder_error_destroy(error);
  holder_context_destroy(context);
}

TEST_CASE("C API updates a card's content and title", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* project_json = nullptr;
  REQUIRE(holder_project_create(context, "Home", nullptr, nullptr, &project_json, &error) == HOLDER_OK);
  const std::string project_id = nlohmann::json::parse(project_json)["project_id"].get<std::string>();
  holder_string_free(project_json);

  char* card_json = nullptr;
  REQUIRE(
      holder_card_create(context, project_id.c_str(), "Welcome", "Original body", nullptr, &card_json, &error) ==
      HOLDER_OK
  );
  const std::string card_id = nlohmann::json::parse(card_json)["card_id"].get<std::string>();
  holder_string_free(card_json);

  char* updated_json = nullptr;
  REQUIRE(
      holder_card_update_content(context, card_id.c_str(), "New body", "Renamed", &updated_json, &error) ==
      HOLDER_OK
  );
  REQUIRE(nlohmann::json::parse(updated_json)["title"] == "Renamed");
  holder_string_free(updated_json);

  char* content = nullptr;
  REQUIRE(holder_card_get_content(context, card_id.c_str(), &content, &error) == HOLDER_OK);
  REQUIRE(std::string(content) == "New body");
  holder_string_free(content);

  holder_context_destroy(context);
}

TEST_CASE("C API reports invalid card update_content arguments", "[capi]") {
  holder_error* error = nullptr;
  char* json = nullptr;

  REQUIRE(
      holder_card_update_content(nullptr, "card-1", "content", nullptr, &json, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(json == nullptr);
  REQUIRE(error != nullptr);
  holder_error_destroy(error);
  error = nullptr;

  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  REQUIRE(
      holder_card_update_content(context, "missing-card", "content", nullptr, &json, &error) ==
      HOLDER_ERROR_RUNTIME
  );
  REQUIRE(error != nullptr);

  holder_error_destroy(error);
  holder_context_destroy(context);
}

TEST_CASE("C API deletes (trashes) a card so it stops appearing in the list", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* project_json = nullptr;
  REQUIRE(holder_project_create(context, "Home", nullptr, nullptr, &project_json, &error) == HOLDER_OK);
  const std::string project_id = nlohmann::json::parse(project_json)["project_id"].get<std::string>();
  holder_string_free(project_json);

  char* card_json = nullptr;
  REQUIRE(
      holder_card_create(context, project_id.c_str(), "Welcome", nullptr, nullptr, &card_json, &error) ==
      HOLDER_OK
  );
  const std::string card_id = nlohmann::json::parse(card_json)["card_id"].get<std::string>();
  holder_string_free(card_json);

  REQUIRE(holder_card_delete(context, card_id.c_str(), &error) == HOLDER_OK);

  char* list_json = nullptr;
  REQUIRE(holder_card_list(context, project_id.c_str(), &list_json, &error) == HOLDER_OK);
  REQUIRE(nlohmann::json::parse(list_json).empty());
  holder_string_free(list_json);

  holder_context_destroy(context);
}

TEST_CASE("C API reports invalid card delete arguments", "[capi]") {
  holder_error* error = nullptr;

  REQUIRE(holder_card_delete(nullptr, "card-1", &error) == HOLDER_ERROR_INVALID_ARGUMENT);
  REQUIRE(error != nullptr);
  holder_error_destroy(error);
  error = nullptr;

  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  REQUIRE(holder_card_delete(context, "missing-card", &error) == HOLDER_ERROR_RUNTIME);
  REQUIRE(error != nullptr);

  holder_error_destroy(error);
  holder_context_destroy(context);
}

TEST_CASE("C API indexes cards for search on create, update, and delete", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* project_json = nullptr;
  REQUIRE(holder_project_create(context, "Home", nullptr, nullptr, &project_json, &error) == HOLDER_OK);
  const std::string project_id = nlohmann::json::parse(project_json)["project_id"].get<std::string>();
  holder_string_free(project_json);

  char* card_json = nullptr;
  REQUIRE(
      holder_card_create(
          context,
          project_id.c_str(),
          "Recipe",
          "A recipe for sourdough bread",
          nullptr,
          &card_json,
          &error
      ) == HOLDER_OK
  );
  const std::string card_id = nlohmann::json::parse(card_json)["card_id"].get<std::string>();
  holder_string_free(card_json);

  char* search_json = nullptr;
  REQUIRE(
      holder_card_search(context, project_id.c_str(), "sourdough", 20, 0, &search_json, &error) == HOLDER_OK
  );
  auto results = nlohmann::json::parse(search_json);
  REQUIRE(results.size() == 1);
  REQUIRE(results[0]["card_id"] == card_id);
  REQUIRE(results[0]["title"] == "Recipe");
  holder_string_free(search_json);

  char* updated_json = nullptr;
  REQUIRE(
      holder_card_update_content(context, card_id.c_str(), "A recipe for banana bread", nullptr, &updated_json, &error) ==
      HOLDER_OK
  );
  holder_string_free(updated_json);

  REQUIRE(holder_card_search(context, project_id.c_str(), "sourdough", 20, 0, &search_json, &error) == HOLDER_OK);
  REQUIRE(nlohmann::json::parse(search_json).empty());
  holder_string_free(search_json);

  REQUIRE(holder_card_search(context, project_id.c_str(), "banana", 20, 0, &search_json, &error) == HOLDER_OK);
  REQUIRE(nlohmann::json::parse(search_json).size() == 1);
  holder_string_free(search_json);

  REQUIRE(holder_card_delete(context, card_id.c_str(), &error) == HOLDER_OK);
  REQUIRE(holder_card_search(context, project_id.c_str(), "banana", 20, 0, &search_json, &error) == HOLDER_OK);
  REQUIRE(nlohmann::json::parse(search_json).empty());
  holder_string_free(search_json);

  holder_context_destroy(context);
}

TEST_CASE("C API reports invalid card search arguments", "[capi]") {
  holder_error* error = nullptr;
  char* json = nullptr;

  REQUIRE(
      holder_card_search(nullptr, "project-1", "q", 20, 0, &json, &error) == HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(json == nullptr);
  REQUIRE(error != nullptr);
  holder_error_destroy(error);
  error = nullptr;

  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  REQUIRE(
      holder_card_search(context, "project-1", "", 20, 0, &json, &error) == HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(error != nullptr);
  REQUIRE(std::string(holder_error_message(error)).find("query") != std::string::npos);

  holder_error_destroy(error);
  holder_context_destroy(context);
}

TEST_CASE("C API reindex backfills cards that weren't indexed", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  seed_project_with_cards(data_dir);

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  const auto schema = read_schema_sql();
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* search_json = nullptr;
  REQUIRE(holder_card_search(context, "project-1", "Welcome", 20, 0, &search_json, &error) == HOLDER_OK);
  REQUIRE(nlohmann::json::parse(search_json).empty());
  holder_string_free(search_json);

  REQUIRE(holder_reindex(context, &error) == HOLDER_OK);

  REQUIRE(holder_card_search(context, "project-1", "Welcome", 20, 0, &search_json, &error) == HOLDER_OK);
  REQUIRE(nlohmann::json::parse(search_json).size() == 1);
  holder_string_free(search_json);

  holder_context_destroy(context);
}

TEST_CASE("C API reports invalid reindex arguments", "[capi]") {
  holder_error* error = nullptr;
  REQUIRE(holder_reindex(nullptr, &error) == HOLDER_ERROR_INVALID_ARGUMENT);
  REQUIRE(error != nullptr);
  holder_error_destroy(error);
}

TEST_CASE("C API ensure_default_project bootstraps once and then no-ops", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(
      holder_ensure_default_project(
          context,
          "Home",
          "Welcome",
          "# Welcome to Holder\n",
          &json,
          &error
      ) == HOLDER_OK
  );
  REQUIRE(json != nullptr);
  REQUIRE(error == nullptr);

  const auto body = nlohmann::json::parse(json);
  REQUIRE(body["name"] == "Home");
  REQUIRE(body["privacy_mode"] == "plain");
  const std::string project_id = body["project_id"].get<std::string>();
  holder_string_free(json);

  char* card_list_json = nullptr;
  REQUIRE(holder_card_list(context, project_id.c_str(), &card_list_json, &error) == HOLDER_OK);
  const auto cards = nlohmann::json::parse(card_list_json);
  REQUIRE(cards.size() == 1);
  REQUIRE(cards[0]["title"] == "Welcome");
  holder_string_free(card_list_json);

  char* second_json = nullptr;
  REQUIRE(
      holder_ensure_default_project(
          context,
          "Home",
          "Welcome",
          "# Welcome to Holder\n",
          &second_json,
          &error
      ) == HOLDER_OK
  );
  REQUIRE(second_json != nullptr);
  REQUIRE(std::string(second_json) == "null");
  holder_string_free(second_json);

  char* project_list_json = nullptr;
  REQUIRE(holder_project_list(context, &project_list_json, &error) == HOLDER_OK);
  REQUIRE(nlohmann::json::parse(project_list_json).size() == 1);
  holder_string_free(project_list_json);

  holder_context_destroy(context);
}

TEST_CASE("C API reports invalid ensure_default_project arguments", "[capi]") {
  holder_error* error = nullptr;
  char* json = nullptr;

  REQUIRE(
      holder_ensure_default_project(nullptr, "Home", "Welcome", nullptr, &json, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(json == nullptr);
  REQUIRE(error != nullptr);
  holder_error_destroy(error);
  error = nullptr;

  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  REQUIRE(
      holder_ensure_default_project(context, "Home", "", nullptr, &json, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(error != nullptr);
  REQUIRE(std::string(holder_error_message(error)).find("welcome_title") != std::string::npos);

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

TEST_CASE("C API updates a project's git remote URL, including clearing it", "[capi][git]") {
  const auto data_dir = holder::test::make_temp_dir();
  seed_git_project(data_dir, "project-1", data_dir / "repo", std::nullopt);

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  const auto schema = read_schema_sql();
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(
      holder_project_update_git_remote(context, "project-1", "git@example.com:a/b.git", &json, &error) ==
      HOLDER_OK
  );
  REQUIRE(nlohmann::json::parse(json)["git_remote_url"] == "git@example.com:a/b.git");
  holder_string_free(json);

  json = nullptr;
  REQUIRE(holder_project_update_git_remote(context, "project-1", nullptr, &json, &error) == HOLDER_OK);
  REQUIRE(nlohmann::json::parse(json)["git_remote_url"].is_null());
  holder_string_free(json);

  holder_context_destroy(context);
}

TEST_CASE("C API git_test_remote reports remote_unset when unconfigured", "[capi][git]") {
  const auto data_dir = holder::test::make_temp_dir();
  seed_git_project(data_dir, "project-1", data_dir / "repo", std::nullopt);

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  const auto schema = read_schema_sql();
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(holder_git_test_remote(context, "project-1", nullptr, &json, &error) == HOLDER_OK);
  const auto body = nlohmann::json::parse(json);
  REQUIRE(body["status"] == "remote_unset");
  REQUIRE(body["remote_has_head"] == false);
  REQUIRE_FALSE(body["error_message"].is_null());

  holder_string_free(json);
  holder_context_destroy(context);
}

TEST_CASE("C API git_test_remote reports reachable for a local remote with commits", "[capi][git]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto remote_dir = data_dir / "remote";

  holder::git::GitRepo remote_repo;
  remote_repo.open_or_init(remote_dir);
  remote_repo.write_file("cards/a.md", "seed");
  remote_repo.stage_path("cards/a.md");
  remote_repo.commit("seed");

  seed_git_project(data_dir, "project-1", data_dir / "repo", remote_dir.string());

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  const auto schema = read_schema_sql();
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(holder_git_test_remote(context, "project-1", nullptr, &json, &error) == HOLDER_OK);
  const auto body = nlohmann::json::parse(json);
  REQUIRE(body["status"] == "reachable");
  REQUIRE(body["remote_has_head"] == true);
  REQUIRE(body["error_message"].is_null());

  holder_string_free(json);
  holder_context_destroy(context);
}

TEST_CASE("C API git_push reports remote_unset and records it in sync status", "[capi][git]") {
  const auto data_dir = holder::test::make_temp_dir();
  seed_git_project(data_dir, "project-1", data_dir / "repo", std::nullopt);

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  const auto schema = read_schema_sql();
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(holder_git_push(context, "project-1", nullptr, 1, &json, &error) == HOLDER_OK);
  auto body = nlohmann::json::parse(json);
  REQUIRE(body["status"] == "remote_unset");
  holder_string_free(json);

  json = nullptr;
  REQUIRE(holder_git_sync_status(context, "project-1", &json, &error) == HOLDER_OK);
  body = nlohmann::json::parse(json);
  REQUIRE(body["sync"]["last_push_status"] == "remote_unset");

  holder_string_free(json);
  holder_context_destroy(context);
}

TEST_CASE("C API git_push and git_pull round-trip through a local remote", "[capi][git]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto remote_dir = data_dir / "remote";
  init_bare_repo(remote_dir);

  const auto local_dir = data_dir / "repo";
  holder::git::GitRepo local_repo;
  local_repo.open_or_init(local_dir);
  local_repo.write_file("cards/a.md", "v1");
  local_repo.stage_path("cards/a.md");
  local_repo.commit("seed");

  seed_git_project(data_dir, "project-1", local_dir, remote_dir.string());

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  const auto schema = read_schema_sql();
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(holder_git_push(context, "project-1", nullptr, 1, &json, &error) == HOLDER_OK);
  auto body = nlohmann::json::parse(json);
  REQUIRE(body["status"] == "pushed");
  holder_string_free(json);

  json = nullptr;
  REQUIRE(holder_git_pull(context, "project-1", &json, &error) == HOLDER_OK);
  body = nlohmann::json::parse(json);
  REQUIRE(body["status"] == "succeeded");
  holder_string_free(json);

  json = nullptr;
  REQUIRE(holder_git_sync_status(context, "project-1", &json, &error) == HOLDER_OK);
  body = nlohmann::json::parse(json);
  REQUIRE(body["sync"]["last_push_status"] == "pushed");
  REQUIRE(body["sync"]["last_pull_status"] == "succeeded");
  REQUIRE(body["sync"]["unpushed_commits_count"] == 0);

  holder_string_free(json);
  holder_context_destroy(context);
}

TEST_CASE("C API git_sync_status reports a null sync object before any activity", "[capi][git]") {
  const auto data_dir = holder::test::make_temp_dir();
  seed_git_project(data_dir, "project-1", data_dir / "repo", std::nullopt);

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  const auto schema = read_schema_sql();
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(holder_git_sync_status(context, "project-1", &json, &error) == HOLDER_OK);
  const auto body = nlohmann::json::parse(json);
  REQUIRE(body["sync"]["last_push_status"].is_null());
  REQUIRE(body["sync"]["uncommitted_changes_count"] == 0);

  holder_string_free(json);
  holder_context_destroy(context);
}

TEST_CASE("C API git_set_homedir validates its argument and applies process-wide", "[capi][git]") {
  holder_error* error = nullptr;
  REQUIRE(holder_git_set_homedir("", &error) == HOLDER_ERROR_INVALID_ARGUMENT);
  REQUIRE(error != nullptr);
  holder_error_destroy(error);

  error = nullptr;
  const auto dir = holder::test::make_temp_dir();
  REQUIRE(holder_git_set_homedir(dir.string().c_str(), &error) == HOLDER_OK);
  REQUIRE(error == nullptr);
}

namespace {

int fake_sign_ok(void*, const unsigned char*, size_t, unsigned char** out_der_sig, size_t* out_der_sig_len) {
  auto* buf = static_cast<unsigned char*>(std::malloc(1));
  buf[0] = 0x00;
  *out_der_sig = buf;
  *out_der_sig_len = 1;
  return 0;
}

} // namespace

TEST_CASE("C API git_set_ssh_signer validates arguments and destroys user_data exactly once on replace/destroy", "[capi][git]") {
  const auto data_dir = holder::test::make_temp_dir();
  seed_git_project(data_dir, "project-1", data_dir / "repo", std::nullopt);

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  const auto schema = read_schema_sql();
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  const unsigned char pubkey[] = {0x01, 0x02, 0x03};

  SECTION("rejects a null sign_fn, still destroying user_data exactly once") {
    int destroy_count = 0;
    auto destroy = [](void* user_data) {
      *static_cast<int*>(user_data) += 1;
    };
    REQUIRE(
        holder_git_set_ssh_signer(
            context,
            "git",
            pubkey,
            sizeof(pubkey),
            nullptr,
            &destroy_count,
            destroy,
            &error
        ) == HOLDER_ERROR_INVALID_ARGUMENT
    );
    REQUIRE(destroy_count == 1);
  }

  SECTION("rejects an empty username, still destroying user_data exactly once") {
    int destroy_count = 0;
    auto destroy = [](void* user_data) {
      *static_cast<int*>(user_data) += 1;
    };
    REQUIRE(
        holder_git_set_ssh_signer(
            context,
            "",
            pubkey,
            sizeof(pubkey),
            fake_sign_ok,
            &destroy_count,
            destroy,
            &error
        ) == HOLDER_ERROR_INVALID_ARGUMENT
    );
    REQUIRE(destroy_count == 1);
  }

  SECTION("destroy_user_data fires once when replaced, and once more for the replacement at context destroy") {
    int first_destroy_count = 0;
    int second_destroy_count = 0;

    auto destroy_first = [](void* user_data) {
      *static_cast<int*>(user_data) += 1;
    };
    auto destroy_second = [](void* user_data) {
      *static_cast<int*>(user_data) += 1;
    };

    REQUIRE(
        holder_git_set_ssh_signer(
            context,
            "git",
            pubkey,
            sizeof(pubkey),
            fake_sign_ok,
            &first_destroy_count,
            destroy_first,
            &error
        ) == HOLDER_OK
    );
    REQUIRE(first_destroy_count == 0);

    // Installing a second signer drops the last reference to the first,
    // running its destroy_user_data exactly once.
    REQUIRE(
        holder_git_set_ssh_signer(
            context,
            "git",
            pubkey,
            sizeof(pubkey),
            fake_sign_ok,
            &second_destroy_count,
            destroy_second,
            &error
        ) == HOLDER_OK
    );
    REQUIRE(first_destroy_count == 1);
    REQUIRE(second_destroy_count == 0);

    holder_context_destroy(context);
    context = nullptr;
    REQUIRE(second_destroy_count == 1);
    REQUIRE(first_destroy_count == 1); // unchanged
  }

  if (context != nullptr) {
    holder_context_destroy(context);
  }
}
