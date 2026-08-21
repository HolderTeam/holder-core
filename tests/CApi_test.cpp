#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "card/CardPaths.h"
#include "card/CardRepo.h"
#include "core_test_helpers.h"
#include "git/GitRepo.h"
#include "index/FtsIndexer.h"
#include "model/Card.h"
#include "model/Project.h"
#include "platform/Db.h"
#include "privacy/PlatformKeyring.h"
#include "privacy/ProjectPrivacy.h"
#include "project/ProjectRepo.h"
#include "project/Rebuilder.h"

#include <git2.h>
#include <holder/holder.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

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
  // Tests using this helper write plain (unencrypted) markdown directly to
  // the working tree, so this must not be the default "encrypted_git" --
  // that would (correctly) trip assert_encryption_push_safe.
  project.privacy_mode = "plain";
  project.created_at = 10;
  project.updated_at = 20;

  holder::project::ProjectRepo repo(db);
  repo.create(project);
}

// Seeds an encrypted_git project with real key material (generating it via
// ensure_encrypted_project_ready, exactly like a real project creation
// would), so recovery-token tests have something real to export/import.
// Callers must set HOLDER_TEST_KEYSTORE_DIR (e.g. via EnvGuard) so key
// storage doesn't touch a real platform keyring.
std::string seed_encrypted_project(
    const std::filesystem::path& data_dir,
    const std::string& project_id,
    const std::filesystem::path& root_path,
    const std::string& name = "Encrypted Project",
    const std::optional<std::string>& remote_url = std::nullopt
) {
  const auto db_path = data_dir / "server" / "holder.db";
  std::filesystem::create_directories(db_path.parent_path());
  holder::platform::Db db;
  db.open(db_path);
  db.exec(read_schema_sql());

  holder::model::Project project;
  project.project_id = project_id;
  project.name = name;
  project.root_path = root_path.string();
  project.git_remote_url = remote_url;
  project.privacy_mode = "encrypted_git";
  project.created_at = 10;
  project.updated_at = 20;

  holder::project::ProjectRepo repo(db);
  repo.create(project);

  holder::git::RealGitOps git;
  holder::privacy::ensure_encrypted_project_ready(
      git,
      repo,
      project_id,
      project.root_path,
      std::nullopt,
      20,
      [project_id]() { return "key-" + project_id; }
  );

  return repo.get(project_id).value().project_key_id.value();
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

TEST_CASE("C API reports invalid card list_trashed/restore/purge arguments", "[capi]") {
  holder_error* error = nullptr;
  char* json = nullptr;

  REQUIRE(holder_card_list_trashed(nullptr, "project-1", &json, &error) == HOLDER_ERROR_INVALID_ARGUMENT);
  REQUIRE(json == nullptr);
  REQUIRE(error != nullptr);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(holder_card_restore(nullptr, "card-1", &json, &error) == HOLDER_ERROR_INVALID_ARGUMENT);
  REQUIRE(json == nullptr);
  REQUIRE(error != nullptr);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(holder_card_purge(nullptr, "card-1", &error) == HOLDER_ERROR_INVALID_ARGUMENT);
  REQUIRE(error != nullptr);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(holder_card_list_trashed(nullptr, "project-1", nullptr, &error) == HOLDER_ERROR_INVALID_ARGUMENT);
  REQUIRE(error != nullptr);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(holder_card_restore(nullptr, "card-1", nullptr, &error) == HOLDER_ERROR_INVALID_ARGUMENT);
  REQUIRE(error != nullptr);
  holder_error_destroy(error);
  error = nullptr;

  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  REQUIRE(holder_card_list_trashed(context, "", &json, &error) == HOLDER_ERROR_INVALID_ARGUMENT);
  REQUIRE(error != nullptr);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(holder_card_restore(context, "", &json, &error) == HOLDER_ERROR_INVALID_ARGUMENT);
  REQUIRE(error != nullptr);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(holder_card_purge(context, "", &error) == HOLDER_ERROR_INVALID_ARGUMENT);
  REQUIRE(error != nullptr);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(holder_card_restore(context, "missing-card", &json, &error) == HOLDER_ERROR_RUNTIME);
  REQUIRE(error != nullptr);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(holder_card_purge(context, "missing-card", &error) == HOLDER_ERROR_RUNTIME);
  REQUIRE(error != nullptr);
  holder_error_destroy(error);

  holder_context_destroy(context);
}

TEST_CASE("C API card_list_trashed lists only soft-deleted cards", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  seed_git_project(data_dir, "project-1", data_dir / "repo", std::nullopt);

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  const auto schema = read_schema_sql();
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(
      holder_card_create(context, "project-1", "Keep", "keep me", nullptr, &json, &error) == HOLDER_OK
  );
  holder_string_free(json);

  json = nullptr;
  REQUIRE(
      holder_card_create(context, "project-1", "Trash me", "bye", nullptr, &json, &error) == HOLDER_OK
  );
  const std::string trashed_id = nlohmann::json::parse(json)["card_id"].get<std::string>();
  holder_string_free(json);

  REQUIRE(holder_card_delete(context, trashed_id.c_str(), &error) == HOLDER_OK);

  json = nullptr;
  REQUIRE(holder_card_list(context, "project-1", &json, &error) == HOLDER_OK);
  auto active = nlohmann::json::parse(json);
  REQUIRE(active.size() == 1);
  REQUIRE(active[0]["title"] == "Keep");
  holder_string_free(json);

  json = nullptr;
  REQUIRE(holder_card_list_trashed(context, "project-1", &json, &error) == HOLDER_OK);
  auto trashed = nlohmann::json::parse(json);
  REQUIRE(trashed.size() == 1);
  REQUIRE(trashed[0]["card_id"] == trashed_id);
  REQUIRE(trashed[0]["title"] == "Trash me");
  REQUIRE_FALSE(trashed[0]["deleted_at"].is_null());
  holder_string_free(json);

  holder_context_destroy(context);
}

TEST_CASE("C API card_restore brings a trashed card back with its content intact", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  seed_git_project(data_dir, "project-1", data_dir / "repo", std::nullopt);

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  const auto schema = read_schema_sql();
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(
      holder_card_create(context, "project-1", "Undo me", "original content", nullptr, &json, &error) ==
      HOLDER_OK
  );
  const std::string card_id = nlohmann::json::parse(json)["card_id"].get<std::string>();
  holder_string_free(json);

  REQUIRE(holder_card_delete(context, card_id.c_str(), &error) == HOLDER_OK);

  // Restoring a card that isn't trashed is rejected.
  json = nullptr;
  REQUIRE(holder_card_restore(context, card_id.c_str(), &json, &error) == HOLDER_OK);
  auto restored = nlohmann::json::parse(json);
  REQUIRE(restored["card_id"] == card_id);
  REQUIRE(restored["deleted_at"].is_null());
  holder_string_free(json);
  json = nullptr;

  REQUIRE(holder_card_restore(context, card_id.c_str(), &json, &error) == HOLDER_ERROR_RUNTIME);
  REQUIRE(error != nullptr);
  holder_error_destroy(error);
  error = nullptr;

  char* content = nullptr;
  REQUIRE(holder_card_get_content(context, card_id.c_str(), &content, &error) == HOLDER_OK);
  REQUIRE(std::string(content) == "original content");
  holder_string_free(content);

  json = nullptr;
  REQUIRE(holder_card_list(context, "project-1", &json, &error) == HOLDER_OK);
  REQUIRE(nlohmann::json::parse(json).size() == 1);
  holder_string_free(json);

  json = nullptr;
  REQUIRE(holder_card_list_trashed(context, "project-1", &json, &error) == HOLDER_OK);
  REQUIRE(nlohmann::json::parse(json).empty());
  holder_string_free(json);

  holder_context_destroy(context);
}

TEST_CASE("C API card_purge permanently removes a trashed card", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  seed_git_project(data_dir, "project-1", data_dir / "repo", std::nullopt);

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  const auto schema = read_schema_sql();
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(
      holder_card_create(context, "project-1", "Gone for good", "content", nullptr, &json, &error) ==
      HOLDER_OK
  );
  const std::string card_id = nlohmann::json::parse(json)["card_id"].get<std::string>();
  holder_string_free(json);

  // Purging a card that isn't trashed is rejected.
  REQUIRE(holder_card_purge(context, card_id.c_str(), &error) == HOLDER_ERROR_RUNTIME);
  REQUIRE(error != nullptr);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(holder_card_delete(context, card_id.c_str(), &error) == HOLDER_OK);
  REQUIRE(holder_card_purge(context, card_id.c_str(), &error) == HOLDER_OK);

  json = nullptr;
  REQUIRE(holder_card_list_trashed(context, "project-1", &json, &error) == HOLDER_OK);
  REQUIRE(nlohmann::json::parse(json).empty());
  holder_string_free(json);

  char* content = nullptr;
  REQUIRE(holder_card_get_content(context, card_id.c_str(), &content, &error) == HOLDER_ERROR_RUNTIME);
  REQUIRE(error != nullptr);
  holder_error_destroy(error);
  error = nullptr;

  // Gone -- not even restorable any more.
  REQUIRE(holder_card_restore(context, card_id.c_str(), &json, &error) == HOLDER_ERROR_RUNTIME);
  REQUIRE(error != nullptr);
  holder_error_destroy(error);

  holder_context_destroy(context);
}

TEST_CASE("C API card_restore reports missing trash content", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto repo_dir = data_dir / "repo";
  seed_git_project(data_dir, "project-1", repo_dir, std::nullopt);

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  const auto schema = read_schema_sql();
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(
      holder_card_create(context, "project-1", "Doomed", "content", nullptr, &json, &error) == HOLDER_OK
  );
  const std::string card_id = nlohmann::json::parse(json)["card_id"].get<std::string>();
  holder_string_free(json);

  REQUIRE(holder_card_delete(context, card_id.c_str(), &error) == HOLDER_OK);
  REQUIRE(std::filesystem::remove(repo_dir / holder::core::card_trash_rel_path(card_id)));

  json = nullptr;
  REQUIRE(holder_card_restore(context, card_id.c_str(), &json, &error) == HOLDER_ERROR_RUNTIME);
  REQUIRE(json == nullptr);
  REQUIRE(error != nullptr);
  REQUIRE(std::string(holder_error_message(error)).find("content missing") != std::string::npos);
  holder_error_destroy(error);

  holder_context_destroy(context);
}

TEST_CASE("C API reports invalid card link arguments", "[capi]") {
  holder_error* error = nullptr;
  char* json = nullptr;

  REQUIRE(holder_card_list_links(nullptr, "card-1", &json, &error) == HOLDER_ERROR_INVALID_ARGUMENT);
  REQUIRE(json == nullptr);
  REQUIRE(error != nullptr);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_card_link_add(nullptr, "card-1", "card-2", "ref", nullptr, &json, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(json == nullptr);
  REQUIRE(error != nullptr);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_card_link_remove(nullptr, "card-1", "card-2", "ref", &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(error != nullptr);
  holder_error_destroy(error);
  error = nullptr;

  const auto data_dir = holder::test::make_temp_dir();
  seed_git_project(data_dir, "project-1", data_dir / "repo", std::nullopt);
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  REQUIRE(holder_card_list_links(context, "", &json, &error) == HOLDER_ERROR_INVALID_ARGUMENT);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_card_link_add(context, "", "card-2", "ref", nullptr, &json, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  holder_error_destroy(error);
  error = nullptr;
  REQUIRE(
      holder_card_link_add(context, "card-1", "", "ref", nullptr, &json, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  holder_error_destroy(error);
  error = nullptr;
  REQUIRE(
      holder_card_link_add(context, "card-1", "card-2", "", nullptr, &json, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_card_link_remove(context, "", "card-2", "ref", &error) == HOLDER_ERROR_INVALID_ARGUMENT
  );
  holder_error_destroy(error);
  error = nullptr;
  REQUIRE(
      holder_card_link_remove(context, "card-1", "", "ref", &error) == HOLDER_ERROR_INVALID_ARGUMENT
  );
  holder_error_destroy(error);
  error = nullptr;
  REQUIRE(
      holder_card_link_remove(context, "card-1", "card-2", "", &error) == HOLDER_ERROR_INVALID_ARGUMENT
  );
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(holder_card_list_links(context, "missing-card", &json, &error) == HOLDER_ERROR_RUNTIME);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_card_link_add(context, "missing-card", "also-missing", "ref", nullptr, &json, &error) ==
      HOLDER_ERROR_RUNTIME
  );
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_card_link_remove(context, "missing-card", "also-missing", "ref", &error) ==
      HOLDER_ERROR_RUNTIME
  );
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(holder_card_list_links(context, "card-1", nullptr, &error) == HOLDER_ERROR_INVALID_ARGUMENT);
  holder_error_destroy(error);
  error = nullptr;
  REQUIRE(
      holder_card_link_add(context, "card-1", "card-2", "ref", nullptr, nullptr, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  holder_error_destroy(error);
  error = nullptr;

  json = nullptr;
  REQUIRE(
      holder_card_create(context, "project-1", "Real card", "content", nullptr, &json, &error) ==
      HOLDER_OK
  );
  const std::string real_card_id = nlohmann::json::parse(json)["card_id"].get<std::string>();
  holder_string_free(json);

  json = nullptr;
  REQUIRE(
      holder_card_link_add(context, real_card_id.c_str(), "no-such-target", "ref", nullptr, &json, &error) ==
      HOLDER_ERROR_RUNTIME
  );
  REQUIRE(std::string(holder_error_message(error)).find("no-such-target") != std::string::npos);
  holder_error_destroy(error);

  holder_context_destroy(context);
}

TEST_CASE("C API card link functions report the underlying sqlite error when card_links is missing", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  seed_git_project(data_dir, "project-1", data_dir / "repo", std::nullopt);
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(
      holder_card_create(context, "project-1", "From", "content", nullptr, &json, &error) == HOLDER_OK
  );
  const std::string from_id = nlohmann::json::parse(json)["card_id"].get<std::string>();
  holder_string_free(json);

  json = nullptr;
  REQUIRE(holder_card_create(context, "project-1", "To", "content", nullptr, &json, &error) == HOLDER_OK);
  const std::string to_id = nlohmann::json::parse(json)["card_id"].get<std::string>();
  holder_string_free(json);

  {
    holder::platform::Db raw_db;
    raw_db.open(data_dir / "server" / "holder.db");
    raw_db.exec("DROP TABLE card_links;");
  }

  json = nullptr;
  REQUIRE(holder_card_list_links(context, from_id.c_str(), &json, &error) == HOLDER_ERROR_RUNTIME);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_card_link_add(context, from_id.c_str(), to_id.c_str(), "ref", nullptr, &json, &error) ==
      HOLDER_ERROR_RUNTIME
  );
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_card_link_remove(context, from_id.c_str(), to_id.c_str(), "ref", &error) ==
      HOLDER_ERROR_RUNTIME
  );
  holder_error_destroy(error);

  holder_context_destroy(context);
}

TEST_CASE("C API card_link_add/remove round-trips connections with title enrichment", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  seed_git_project(data_dir, "project-1", data_dir / "repo", std::nullopt);
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(
      holder_card_create(context, "project-1", "Blocked task", "content", nullptr, &json, &error) ==
      HOLDER_OK
  );
  const std::string blocked_id = nlohmann::json::parse(json)["card_id"].get<std::string>();
  holder_string_free(json);

  json = nullptr;
  REQUIRE(
      holder_card_create(context, "project-1", "Blocking task", "content", nullptr, &json, &error) ==
      HOLDER_OK
  );
  const std::string blocking_id = nlohmann::json::parse(json)["card_id"].get<std::string>();
  holder_string_free(json);

  // A card with no target's title is still resolvable is exercised separately below; here first
  // confirm a fresh card has empty connections in both directions.
  json = nullptr;
  REQUIRE(holder_card_list_links(context, blocked_id.c_str(), &json, &error) == HOLDER_OK);
  auto empty_links = nlohmann::json::parse(json);
  REQUIRE(empty_links["outgoing"].empty());
  REQUIRE(empty_links["backlinks"].empty());
  holder_string_free(json);

  json = nullptr;
  REQUIRE(
      holder_card_link_add(
          context, blocked_id.c_str(), blocking_id.c_str(), "blocked_by", "waiting on this", &json, &error
      ) == HOLDER_OK
  );
  auto outgoing = nlohmann::json::parse(json);
  REQUIRE(outgoing.size() == 1);
  REQUIRE(outgoing[0]["to_card_id"] == blocking_id);
  REQUIRE(outgoing[0]["to_title"] == "Blocking task");
  REQUIRE(outgoing[0]["kind"] == "blocked_by");
  REQUIRE(outgoing[0]["label"] == "waiting on this");
  holder_string_free(json);

  // Re-adding the same from/to/kind updates the label in place rather than duplicating.
  json = nullptr;
  REQUIRE(
      holder_card_link_add(
          context, blocked_id.c_str(), blocking_id.c_str(), "blocked_by", "still waiting", &json, &error
      ) == HOLDER_OK
  );
  auto updated_outgoing = nlohmann::json::parse(json);
  REQUIRE(updated_outgoing.size() == 1);
  REQUIRE(updated_outgoing[0]["label"] == "still waiting");
  holder_string_free(json);

  json = nullptr;
  REQUIRE(holder_card_list_links(context, blocking_id.c_str(), &json, &error) == HOLDER_OK);
  auto blocking_links = nlohmann::json::parse(json);
  REQUIRE(blocking_links["outgoing"].empty());
  REQUIRE(blocking_links["backlinks"].size() == 1);
  REQUIRE(blocking_links["backlinks"][0]["from_card_id"] == blocked_id);
  REQUIRE(blocking_links["backlinks"][0]["from_title"] == "Blocked task");
  REQUIRE(blocking_links["backlinks"][0]["kind"] == "blocked_by");
  holder_string_free(json);

  // The connection round-trips through the card's own front matter, not just the DB index: a
  // rebuild from disk must still see it.
  {
    holder::platform::Db raw_db;
    raw_db.open(data_dir / "server" / "holder.db");
    holder::index::FtsIndexer raw_fts(raw_db);
    holder::project::ProjectRepo raw_projects(raw_db);
    const auto project = raw_projects.get("project-1").value();
    holder::store::Rebuilder(raw_db, &raw_fts).rebuild_project(project);
  }
  json = nullptr;
  REQUIRE(holder_card_list_links(context, blocked_id.c_str(), &json, &error) == HOLDER_OK);
  auto rebuilt = nlohmann::json::parse(json);
  REQUIRE(rebuilt["outgoing"].size() == 1);
  REQUIRE(rebuilt["outgoing"][0]["to_card_id"] == blocking_id);
  holder_string_free(json);

  REQUIRE(holder_card_link_remove(context, blocked_id.c_str(), blocking_id.c_str(), "blocked_by", &error) == HOLDER_OK);

  json = nullptr;
  REQUIRE(holder_card_list_links(context, blocked_id.c_str(), &json, &error) == HOLDER_OK);
  REQUIRE(nlohmann::json::parse(json)["outgoing"].empty());
  holder_string_free(json);

  json = nullptr;
  REQUIRE(holder_card_list_links(context, blocking_id.c_str(), &json, &error) == HOLDER_OK);
  REQUIRE(nlohmann::json::parse(json)["backlinks"].empty());
  holder_string_free(json);

  // Removing an already-removed connection is a harmless no-op, not an error.
  REQUIRE(holder_card_link_remove(context, blocked_id.c_str(), blocking_id.c_str(), "blocked_by", &error) == HOLDER_OK);

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

TEST_CASE("C API git_push reports up_to_date for a repo with no commits yet", "[capi][git]") {
  // No card is ever created here, so open_project_git's lazy git_repository_init leaves the
  // local repo with an unborn HEAD -- exercising the "nothing to push" branch distinct from
  // remote_unset (which never even opens the repo).
  const auto data_dir = holder::test::make_temp_dir();
  const auto remote_dir = data_dir / "remote";
  init_bare_repo(remote_dir);
  seed_git_project(data_dir, "project-1", data_dir / "repo", remote_dir.string());

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  const auto schema = read_schema_sql();
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(holder_git_push(context, "project-1", nullptr, 1, &json, &error) == HOLDER_OK);
  const auto body = nlohmann::json::parse(json);
  REQUIRE(body["status"] == "up_to_date");
  holder_string_free(json);
  holder_context_destroy(context);
}

TEST_CASE("C API git_push and git_pull round-trip through a local remote", "[capi][git]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto remote_dir = data_dir / "remote";
  init_bare_repo(remote_dir);

  const auto local_dir = data_dir / "repo";
  seed_git_project(data_dir, "project-1", local_dir, remote_dir.string());

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  const auto schema = read_schema_sql();
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(holder_card_create(context, "project-1", "Seed", "v1", nullptr, &json, &error) == HOLDER_OK);
  holder_string_free(json);

  json = nullptr;
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

TEST_CASE("C API git_pull makes a card written by another peer actually visible", "[capi][git]") {
  // holder_git_pull's job is "content written elsewhere becomes visible here" -- fetching and
  // checking out the git working tree isn't enough to prove that; the pulled card must actually
  // show up in holder_card_list, which reads the SQLite index rather than the working tree.
  const auto data_dir = holder::test::make_temp_dir();
  const auto remote_dir = data_dir / "remote";
  init_bare_repo(remote_dir);

  seed_git_project(data_dir, "project-1", data_dir / "repo", remote_dir.string());
  holder_context* writer_context = nullptr;
  holder_error* error = nullptr;
  const auto schema = read_schema_sql();
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &writer_context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(
      holder_card_create(writer_context, "project-1", "From another peer", "body", nullptr, &json, &error) ==
      HOLDER_OK
  );
  holder_string_free(json);
  json = nullptr;
  REQUIRE(holder_git_push(writer_context, "project-1", nullptr, 1, &json, &error) == HOLDER_OK);
  holder_string_free(json);
  holder_context_destroy(writer_context);

  // A second, independent peer: its own data dir, its own empty local working tree, the same
  // remote -- exactly the "desktop wrote it, phone pulls it" scenario.
  const auto reader_data_dir = holder::test::make_temp_dir();
  seed_git_project(reader_data_dir, "project-1", reader_data_dir / "repo", remote_dir.string());
  holder_context* reader_context = nullptr;
  REQUIRE(
      holder_context_open(reader_data_dir.string().c_str(), schema.c_str(), &reader_context, &error) ==
      HOLDER_OK
  );

  json = nullptr;
  REQUIRE(holder_card_list(reader_context, "project-1", &json, &error) == HOLDER_OK);
  REQUIRE(nlohmann::json::parse(json).empty());
  holder_string_free(json);

  json = nullptr;
  REQUIRE(holder_git_pull(reader_context, "project-1", &json, &error) == HOLDER_OK);
  REQUIRE(nlohmann::json::parse(json)["status"] == "succeeded");
  holder_string_free(json);

  json = nullptr;
  REQUIRE(holder_card_list(reader_context, "project-1", &json, &error) == HOLDER_OK);
  const auto cards = nlohmann::json::parse(json);
  REQUIRE(cards.size() == 1);
  REQUIRE(cards[0]["title"] == "From another peer");
  holder_string_free(json);

  holder_context_destroy(reader_context);
}

TEST_CASE(
    "C API git_pull resolves a diverged pull card-level: remote wins the shared card, local "
    "becomes a conflicted copy, unrelated cards on both sides survive",
    "[capi][git]"
) {
  const auto data_dir = holder::test::make_temp_dir();
  const auto remote_dir = data_dir / "remote";
  init_bare_repo(remote_dir);
  const auto schema = read_schema_sql();
  holder_error* error = nullptr;
  char* json = nullptr;

  // Peer A creates the card both sides will later edit, and pushes.
  seed_git_project(data_dir, "project-1", data_dir / "repo", remote_dir.string());
  holder_context* peer_a = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &peer_a, &error) == HOLDER_OK);
  REQUIRE(
      holder_card_create(peer_a, "project-1", "Shared Card", "v0", nullptr, &json, &error) == HOLDER_OK
  );
  const std::string shared_card_id = nlohmann::json::parse(json)["card_id"].get<std::string>();
  holder_string_free(json);
  json = nullptr;
  REQUIRE(holder_git_push(peer_a, "project-1", nullptr, 1, &json, &error) == HOLDER_OK);
  holder_string_free(json);

  // Peer B: a second, independent peer that pulls the shared card before diverging from it, so
  // both sides end up editing the exact same card_id -- the scenario this resolution exists for.
  const auto peer_b_data_dir = holder::test::make_temp_dir();
  seed_git_project(peer_b_data_dir, "project-1", peer_b_data_dir / "repo", remote_dir.string());
  holder_context* peer_b = nullptr;
  REQUIRE(
      holder_context_open(peer_b_data_dir.string().c_str(), schema.c_str(), &peer_b, &error) == HOLDER_OK
  );
  json = nullptr;
  REQUIRE(holder_git_pull(peer_b, "project-1", &json, &error) == HOLDER_OK);
  holder_string_free(json);

  // Now diverge: each peer edits the shared card differently, and each creates a card the other
  // has never seen. Peer A pushes first.
  json = nullptr;
  REQUIRE(
      holder_card_update_content(peer_a, shared_card_id.c_str(), "v1 from A", nullptr, &json, &error) ==
      HOLDER_OK
  );
  holder_string_free(json);
  json = nullptr;
  REQUIRE(holder_card_create(peer_a, "project-1", "A only", "a-only", nullptr, &json, &error) == HOLDER_OK);
  holder_string_free(json);
  json = nullptr;
  REQUIRE(holder_git_push(peer_a, "project-1", nullptr, 1, &json, &error) == HOLDER_OK);
  holder_string_free(json);

  json = nullptr;
  REQUIRE(
      holder_card_update_content(peer_b, shared_card_id.c_str(), "v1 from B", nullptr, &json, &error) ==
      HOLDER_OK
  );
  holder_string_free(json);
  json = nullptr;
  REQUIRE(holder_card_create(peer_b, "project-1", "B only", "b-only", nullptr, &json, &error) == HOLDER_OK);
  holder_string_free(json);

  // Peer B pulls into a now-diverged history: it has an unpushed edit to the shared card and an
  // unpushed new card, while remote has its own independent edit to the same shared card and its
  // own new card.
  json = nullptr;
  REQUIRE(holder_git_pull(peer_b, "project-1", &json, &error) == HOLDER_OK);
  auto pull_body = nlohmann::json::parse(json);
  REQUIRE(pull_body["status"] == "succeeded");
  REQUIRE(pull_body["conflicts_resolved"] == 1);
  holder_string_free(json);

  json = nullptr;
  REQUIRE(holder_card_list(peer_b, "project-1", &json, &error) == HOLDER_OK);
  const auto cards = nlohmann::json::parse(json);
  REQUIRE(cards.size() == 4);

  std::optional<std::string> conflicted_copy_id;
  std::set<std::string> titles;
  for (const auto& card : cards) {
    titles.insert(card["title"].get<std::string>());
    if (card["title"] == "Shared Card (conflicted copy)") {
      conflicted_copy_id = card["card_id"].get<std::string>();
    }
  }
  REQUIRE(titles == std::set<std::string>{"Shared Card", "Shared Card (conflicted copy)", "A only", "B only"});
  REQUIRE(conflicted_copy_id.has_value());

  // Remote (A's edit) wins the original card_id; B's pre-merge edit survives as the duplicate.
  char* content = nullptr;
  REQUIRE(holder_card_get_content(peer_b, shared_card_id.c_str(), &content, &error) == HOLDER_OK);
  REQUIRE(std::string(content) == "v1 from A");
  holder_string_free(content);

  content = nullptr;
  REQUIRE(holder_card_get_content(peer_b, conflicted_copy_id->c_str(), &content, &error) == HOLDER_OK);
  REQUIRE(std::string(content) == "v1 from B");
  holder_string_free(content);

  // The resolution must actually unblock syncing, not just avoid crashing: peer B's merge commit
  // has remote's pre-pull HEAD as a parent, so pushing it back should be a plain fast-forward.
  json = nullptr;
  REQUIRE(holder_git_push(peer_b, "project-1", nullptr, 1, &json, &error) == HOLDER_OK);
  REQUIRE(nlohmann::json::parse(json)["status"] == "pushed");
  holder_string_free(json);

  holder_context_destroy(peer_a);
  holder_context_destroy(peer_b);
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

TEST_CASE("C API git_set_ssh_signer's provider is actually wired into git operations", "[capi][git]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto remote_dir = data_dir / "remote";
  init_bare_repo(remote_dir);
  seed_git_project(data_dir, "project-1", data_dir / "repo", remote_dir.string());

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  const auto schema = read_schema_sql();
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  const unsigned char pubkey[] = {0x01, 0x02, 0x03};
  REQUIRE(
      holder_git_set_ssh_signer(
          context, "git", pubkey, sizeof(pubkey), fake_sign_ok, nullptr, nullptr, &error
      ) == HOLDER_OK
  );

  // A local-path remote never actually calls back into the signer, but this still exercises
  // open_project_git's "context has a credential provider installed" branch.
  char* json = nullptr;
  REQUIRE(holder_git_test_remote(context, "project-1", nullptr, &json, &error) == HOLDER_OK);
  const auto body = nlohmann::json::parse(json);
  REQUIRE(body["status"] == "reachable");
  holder_string_free(json);

  holder_context_destroy(context);
}

TEST_CASE("C API git_sync_if_due is a no-op when no remote is configured", "[capi][git]") {
  const auto data_dir = holder::test::make_temp_dir();
  seed_git_project(data_dir, "project-1", data_dir / "repo", std::nullopt);

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  const auto schema = read_schema_sql();
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(holder_git_sync_if_due(context, "project-1", 0, 0, &json, &error) == HOLDER_OK);
  const auto body = nlohmann::json::parse(json);
  REQUIRE(body["pull_attempted"] == false);
  REQUIRE(body["push_attempted"] == false);

  holder_string_free(json);
  holder_context_destroy(context);
}

TEST_CASE("C API git_sync_if_due pulls and pushes when nothing has synced yet, then skips until due again", "[capi][git]") {
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

  // Long intervals so this first call is due only because there's no prior sync state.
  // The remote is a genuinely empty bare repo (no commits yet), so the pull attempt
  // fails as expected -- there's nothing to pull -- and only the push seeds it.
  char* json = nullptr;
  REQUIRE(holder_git_sync_if_due(context, "project-1", 3600, 3600, &json, &error) == HOLDER_OK);
  auto body = nlohmann::json::parse(json);
  REQUIRE(body["pull_attempted"] == true);
  REQUIRE(body["pull_status"] == "failed");
  REQUIRE(body["push_attempted"] == true);
  REQUIRE(body["push_status"] == "pushed");
  holder_string_free(json);

  // Called again immediately with the same long intervals: now due to both
  // having a recent last_pull_at/last_push_at.
  json = nullptr;
  REQUIRE(holder_git_sync_if_due(context, "project-1", 3600, 3600, &json, &error) == HOLDER_OK);
  body = nlohmann::json::parse(json);
  REQUIRE(body["pull_attempted"] == false);
  REQUIRE(body["push_attempted"] == false);

  holder_string_free(json);
  holder_context_destroy(context);
}

TEST_CASE("C API git_sync_if_due reports up_to_date for a repo with no commits yet", "[capi][git]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto remote_dir = data_dir / "remote";
  init_bare_repo(remote_dir);
  seed_git_project(data_dir, "project-1", data_dir / "repo", remote_dir.string());

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  const auto schema = read_schema_sql();
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(holder_git_sync_if_due(context, "project-1", 3600, 3600, &json, &error) == HOLDER_OK);
  const auto body = nlohmann::json::parse(json);
  REQUIRE(body["push_attempted"] == true);
  REQUIRE(body["push_status"] == "up_to_date");
  holder_string_free(json);
  holder_context_destroy(context);
}

TEST_CASE(
    "C API git_sync_if_due reports unknown_error when an encrypted project's push safety check fails",
    "[capi][git][privacy]"
) {
  // A raw plaintext file sitting where an encrypted card should be trips
  // assert_encryption_push_safe, which only holder_git_sync_if_due (not holder_git_push
  // directly) runs before pushing an encrypted_git project.
  const auto data_dir = holder::test::make_temp_dir();
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (data_dir / "keystore").string());
  const auto remote_dir = data_dir / "remote";
  init_bare_repo(remote_dir);
  const auto repo_dir = data_dir / "repo";
  seed_encrypted_project(data_dir, "project-1", repo_dir, "Notes", remote_dir.string());

  const auto plain_path = repo_dir / "cards" / "ab" / "plain.md";
  std::filesystem::create_directories(plain_path.parent_path());
  std::ofstream(plain_path) << "# title\nhello\n";

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  const auto schema = read_schema_sql();
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(holder_git_sync_if_due(context, "project-1", 3600, 3600, &json, &error) == HOLDER_OK);
  const auto body = nlohmann::json::parse(json);
  REQUIRE(body["push_attempted"] == true);
  REQUIRE(body["push_status"] == "unknown_error");
  REQUIRE_FALSE(body["push_error"].is_null());
  holder_string_free(json);
  holder_context_destroy(context);
}

TEST_CASE("C API git_pull reports a real fetch failure as a failed status", "[capi][git]") {
  // A genuinely empty bare remote (no commits, no refs) fails pull_remote_ff_only with a plain
  // exception rather than NonFastForwardPullError -- there's nothing to diverge from.
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
  REQUIRE(holder_git_pull(context, "project-1", &json, &error) == HOLDER_OK);
  const auto body = nlohmann::json::parse(json);
  REQUIRE(body["status"] == "failed");
  REQUIRE_FALSE(body["error_message"].is_null());
  holder_string_free(json);
  holder_context_destroy(context);
}

TEST_CASE("C API git_sync_if_due reports invalid project arguments", "[capi][git]") {
  const auto data_dir = holder::test::make_temp_dir();
  holder_context* context = nullptr;
  holder_error* error = nullptr;
  const auto schema = read_schema_sql();
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(
      holder_git_sync_if_due(context, "does-not-exist", 0, 0, &json, &error) == HOLDER_ERROR_RUNTIME
  );
  REQUIRE(error != nullptr);
  holder_error_destroy(error);

  holder_context_destroy(context);
}

namespace {

// A fake external keyring provider for the C ABI, backed by plain in-memory
// state passed as user_data -- exactly the shape Android's JNI bridge to
// EncryptedSharedPreferences plays in production.
struct FakeKeyringState {
  std::map<std::string, std::string> secrets;
  std::vector<std::string> observed_project_ids;
};

void record_project_id(FakeKeyringState& state, const char* project_id) {
  state.observed_project_ids.emplace_back(project_id != nullptr ? project_id : "");
}

int fake_keyring_lookup(
    void* user_data,
    int /*kind*/,
    const char* /*service*/,
    const char* account,
    const char* project_id,
    int* out_found,
    char** out_secret,
    char** /*out_error*/
) {
  auto* state = static_cast<FakeKeyringState*>(user_data);
  record_project_id(*state, project_id);
  const auto it = state->secrets.find(account);
  if (it == state->secrets.end()) {
    *out_found = 0;
    return 0;
  }
  *out_found = 1;
  auto* buf = static_cast<char*>(std::malloc(it->second.size() + 1));
  std::memcpy(buf, it->second.c_str(), it->second.size() + 1);
  *out_secret = buf;
  return 0;
}

int fake_keyring_store(
    void* user_data,
    int /*kind*/,
    const char* /*service*/,
    const char* account,
    const char* project_id,
    const char* /*label*/,
    const char* secret,
    char** /*out_error*/
) {
  auto* state = static_cast<FakeKeyringState*>(user_data);
  record_project_id(*state, project_id);
  state->secrets[account] = secret;
  return 0;
}

int fake_keyring_remove(
    void* user_data,
    int /*kind*/,
    const char* /*service*/,
    const char* account,
    const char* project_id,
    char** /*out_error*/
) {
  auto* state = static_cast<FakeKeyringState*>(user_data);
  record_project_id(*state, project_id);
  state->secrets.erase(account);
  return 0;
}

void noop_keyring_destroy(void*) {}

char* malloc_copy(const char* text) {
  const size_t len = std::strlen(text);
  auto* buf = static_cast<char*>(std::malloc(len + 1));
  std::memcpy(buf, text, len + 1);
  return buf;
}

// "_with_message" variants set *out_error; "_silent" variants report failure (rc != 0) without
// setting it, forcing the CApiKeyringProviderHandle wrapper's fallback message.
int failing_keyring_lookup_with_message(void*, int, const char*, const char*, const char*, int*, char**, char** out_error) {
  *out_error = malloc_copy("lookup exploded");
  return 1;
}
int failing_keyring_lookup_silent(void*, int, const char*, const char*, const char*, int*, char**, char**) {
  return 1;
}
int failing_keyring_store_with_message(void*, int, const char*, const char*, const char*, const char*, const char*, char** out_error) {
  *out_error = malloc_copy("store exploded");
  return 1;
}
int failing_keyring_store_silent(void*, int, const char*, const char*, const char*, const char*, const char*, char**) {
  return 1;
}
int failing_keyring_remove_with_message(void*, int, const char*, const char*, const char*, char** out_error) {
  *out_error = malloc_copy("remove exploded");
  return 1;
}
int failing_keyring_remove_silent(void*, int, const char*, const char*, const char*, char**) {
  return 1;
}

} // namespace

TEST_CASE("C API keyring_set_provider validates arguments, still destroying user_data exactly once", "[capi][privacy]") {
  int destroy_count = 0;
  auto destroy = [](void* user_data) {
    *static_cast<int*>(user_data) += 1;
  };
  holder_error* error = nullptr;

  REQUIRE(
      holder_keyring_set_provider(
          nullptr,
          fake_keyring_store,
          fake_keyring_remove,
          &destroy_count,
          destroy,
          &error
      ) == HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(destroy_count == 1);

  holder::privacy::platform_keyring_clear_external_provider();
}

TEST_CASE("C API keyring_set_provider destroys user_data exactly once on replace and on clear", "[capi][privacy]") {
  int first_destroy_count = 0;
  int second_destroy_count = 0;
  auto destroy_first = [](void* user_data) { *static_cast<int*>(user_data) += 1; };
  auto destroy_second = [](void* user_data) { *static_cast<int*>(user_data) += 1; };
  holder_error* error = nullptr;

  // lookup_fn/store_fn/remove_fn are never actually invoked in this test (nothing here
  // calls platform_keyring_lookup/store/remove_secret), so passing fake_keyring_lookup/
  // store/remove here is only about satisfying holder_keyring_set_provider's non-null
  // validation -- user_data is exercised solely by the destroy_* callbacks below.
  REQUIRE(
      holder_keyring_set_provider(
          fake_keyring_lookup,
          fake_keyring_store,
          fake_keyring_remove,
          &first_destroy_count,
          destroy_first,
          &error
      ) == HOLDER_OK
  );
  REQUIRE(first_destroy_count == 0);

  REQUIRE(
      holder_keyring_set_provider(
          fake_keyring_lookup,
          fake_keyring_store,
          fake_keyring_remove,
          &second_destroy_count,
          destroy_second,
          &error
      ) == HOLDER_OK
  );
  REQUIRE(first_destroy_count == 1);
  REQUIRE(second_destroy_count == 0);

  holder::privacy::platform_keyring_clear_external_provider();
  REQUIRE(second_destroy_count == 1);
  REQUIRE(first_destroy_count == 1); // unchanged
}

TEST_CASE("C API keyring_set_provider round-trips lookup/store/remove through the registered callbacks", "[capi][privacy]") {
  FakeKeyringState state;
  holder_error* error = nullptr;
  REQUIRE(
      holder_keyring_set_provider(
          fake_keyring_lookup,
          fake_keyring_store,
          fake_keyring_remove,
          &state,
          noop_keyring_destroy,
          &error
      ) == HOLDER_OK
  );

  const holder::privacy::PlatformKeyringSecretRef ref{
      .kind = holder::privacy::PlatformKeyringSecretKind::GenericSecret,
      .service = "holder.test",
      .account = "acct",
      .project_id = "project-1",
  };

  REQUIRE_FALSE(holder::privacy::platform_keyring_lookup_secret(ref).secret.has_value());

  holder::privacy::platform_keyring_store_secret(ref, "label", "secret-value");
  REQUIRE(state.secrets.at("acct") == "secret-value");
  REQUIRE(holder::privacy::platform_keyring_lookup_secret(ref).secret == "secret-value");

  holder::privacy::platform_keyring_remove_secret(ref);
  REQUIRE(state.secrets.find("acct") == state.secrets.end());
  REQUIRE(state.observed_project_ids == std::vector<std::string>(4, "project-1"));

  holder::privacy::platform_keyring_clear_external_provider();
}

TEST_CASE(
    "C API keyring_set_provider surfaces lookup/store/remove failures, with and without a message",
    "[capi][privacy]"
) {
  const holder::privacy::PlatformKeyringSecretRef ref{
      .kind = holder::privacy::PlatformKeyringSecretKind::GenericSecret,
      .service = "holder.test",
      .account = "acct",
      .project_id = std::nullopt,
  };
  holder_error* error = nullptr;

  SECTION("lookup failure with a message") {
    REQUIRE(
        holder_keyring_set_provider(
            failing_keyring_lookup_with_message,
            fake_keyring_store,
            fake_keyring_remove,
            nullptr,
            noop_keyring_destroy,
            &error
        ) == HOLDER_OK
    );
    const auto result = holder::privacy::platform_keyring_lookup_secret(ref);
    REQUIRE(result.error_message == "lookup exploded");
  }

  SECTION("lookup failure without a message falls back to a default") {
    REQUIRE(
        holder_keyring_set_provider(
            failing_keyring_lookup_silent,
            fake_keyring_store,
            fake_keyring_remove,
            nullptr,
            noop_keyring_destroy,
            &error
        ) == HOLDER_OK
    );
    const auto result = holder::privacy::platform_keyring_lookup_secret(ref);
    REQUIRE(result.error_message == "keyring lookup failed");
  }

  SECTION("store failure with a message throws with that message") {
    REQUIRE(
        holder_keyring_set_provider(
            fake_keyring_lookup,
            failing_keyring_store_with_message,
            fake_keyring_remove,
            nullptr,
            noop_keyring_destroy,
            &error
        ) == HOLDER_OK
    );
    REQUIRE_THROWS_WITH(
        holder::privacy::platform_keyring_store_secret(ref, "label", "secret"),
        Catch::Matchers::Equals("store exploded")
    );
  }

  SECTION("store failure without a message falls back to a default") {
    REQUIRE(
        holder_keyring_set_provider(
            fake_keyring_lookup,
            failing_keyring_store_silent,
            fake_keyring_remove,
            nullptr,
            noop_keyring_destroy,
            &error
        ) == HOLDER_OK
    );
    REQUIRE_THROWS_WITH(
        holder::privacy::platform_keyring_store_secret(ref, "label", "secret"),
        Catch::Matchers::Equals("keyring store failed")
    );
  }

  SECTION("remove failure with a message throws with that message") {
    REQUIRE(
        holder_keyring_set_provider(
            fake_keyring_lookup,
            fake_keyring_store,
            failing_keyring_remove_with_message,
            nullptr,
            noop_keyring_destroy,
            &error
        ) == HOLDER_OK
    );
    REQUIRE_THROWS_WITH(
        holder::privacy::platform_keyring_remove_secret(ref), Catch::Matchers::Equals("remove exploded")
    );
  }

  SECTION("remove failure without a message falls back to a default") {
    REQUIRE(
        holder_keyring_set_provider(
            fake_keyring_lookup,
            fake_keyring_store,
            failing_keyring_remove_silent,
            nullptr,
            noop_keyring_destroy,
            &error
        ) == HOLDER_OK
    );
    REQUIRE_THROWS_WITH(
        holder::privacy::platform_keyring_remove_secret(ref), Catch::Matchers::Equals("keyring remove failed")
    );
  }

  holder::privacy::platform_keyring_clear_external_provider();
}

TEST_CASE("C API encryption_check reports plain projects safe without touching disk", "[capi][privacy]") {
  const auto data_dir = holder::test::make_temp_dir();
  seed_git_project(data_dir, "project-1", data_dir / "repo", std::nullopt);

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  const auto schema = read_schema_sql();
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(holder_encryption_check(context, "project-1", &json, &error) == HOLDER_OK);
  const auto body = nlohmann::json::parse(json);
  REQUIRE(body["privacy_mode"] == "plain");
  REQUIRE(body["check"]["ok"] == true);
  REQUIRE(body["check"]["checked_files"] == 0);

  holder_string_free(json);
  holder_context_destroy(context);
}

TEST_CASE("C API encryption_check finds encrypted cards safe", "[capi][privacy]") {
  const auto data_dir = holder::test::make_temp_dir();
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (data_dir / "keystore").string());
  seed_encrypted_project(data_dir, "project-1", data_dir / "repo");

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  const auto schema = read_schema_sql();
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(holder_card_create(context, "project-1", "Secret", "hidden", nullptr, &json, &error) == HOLDER_OK);
  holder_string_free(json);

  json = nullptr;
  REQUIRE(holder_encryption_check(context, "project-1", &json, &error) == HOLDER_OK);
  const auto body = nlohmann::json::parse(json);
  REQUIRE(body["privacy_mode"] == "encrypted_git");
  REQUIRE(body["check"]["ok"] == true);
  REQUIRE(body["check"]["checked_files"] == 1);
  REQUIRE(body["check"]["unsafe_files"] == 0);

  holder_string_free(json);
  holder_context_destroy(context);
}

TEST_CASE("C API recovery_token_export requires key material and a non-empty pin", "[capi][privacy]") {
  const auto data_dir = holder::test::make_temp_dir();
  seed_git_project(data_dir, "project-1", data_dir / "repo", std::nullopt);

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  const auto schema = read_schema_sql();
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(holder_recovery_token_export(context, "project-1", "", &json, &error) == HOLDER_ERROR_INVALID_ARGUMENT);

  REQUIRE(
      holder_recovery_token_export(context, "project-1", "1234", &json, &error) == HOLDER_ERROR_RUNTIME
  );
  REQUIRE(std::string(holder_error_message(error)).find("no key material") != std::string::npos);
  holder_error_destroy(error);

  holder_context_destroy(context);
}

TEST_CASE("C API recovery_token_export/inspect/import round-trip through a real encrypted project", "[capi][privacy]") {
  const auto data_dir = holder::test::make_temp_dir();
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (data_dir / "keystore").string());
  const auto key_id = seed_encrypted_project(
      data_dir,
      "project-1",
      data_dir / "repo",
      "My Notes",
      "git@example.com:org/repo.git"
  );

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  const auto schema = read_schema_sql();
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(holder_recovery_token_export(context, "project-1", "1234", &json, &error) == HOLDER_OK);
  const auto exported = nlohmann::json::parse(json);
  REQUIRE(exported["project_id"] == "project-1");
  REQUIRE(exported["key_id"] == key_id);
  const std::string token = exported["recovery_token"].get<std::string>();
  REQUIRE_FALSE(token.empty());
  holder_string_free(json);

  SECTION("inspect reports the token's metadata without importing anything") {
    json = nullptr;
    REQUIRE(holder_recovery_token_inspect("1234", token.c_str(), &json, &error) == HOLDER_OK);
    const auto metadata = nlohmann::json::parse(json);
    REQUIRE(metadata["project_id"] == "project-1");
    REQUIRE(metadata["project_key_id"] == key_id);
    REQUIRE(metadata["project_name"] == "My Notes");
    REQUIRE(metadata["git_remote_url"] == "git@example.com:org/repo.git");
    holder_string_free(json);
  }

  SECTION("inspect with the wrong pin fails") {
    json = nullptr;
    REQUIRE(holder_recovery_token_inspect("0000", token.c_str(), &json, &error) == HOLDER_ERROR_RUNTIME);
    REQUIRE(error != nullptr);
    holder_error_destroy(error);
  }

  SECTION("import into the same existing project succeeds") {
    json = nullptr;
    REQUIRE(
        holder_recovery_token_import(context, "project-1", "1234", token.c_str(), &json, &error) ==
        HOLDER_OK
    );
    const auto body = nlohmann::json::parse(json);
    REQUIRE(body["project_id"] == "project-1");
    holder_string_free(json);
  }

  SECTION("import into a nonexistent project fails") {
    json = nullptr;
    REQUIRE(
        holder_recovery_token_import(context, "does-not-exist", "1234", token.c_str(), &json, &error) ==
        HOLDER_ERROR_RUNTIME
    );
    REQUIRE(error != nullptr);
    holder_error_destroy(error);
  }

  holder_context_destroy(context);
}

TEST_CASE("C API recovery_token_import_global creates a project when none exists, with no remote hint", "[capi][privacy]") {
  const auto data_dir = holder::test::make_temp_dir();
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (data_dir / "keystore").string());
  const auto key_id =
      seed_encrypted_project(data_dir, "project-1", data_dir / "repo", "Recovered Notes");

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  const auto schema = read_schema_sql();
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(holder_recovery_token_export(context, "project-1", "1234", &json, &error) == HOLDER_OK);
  const std::string token = nlohmann::json::parse(json)["recovery_token"].get<std::string>();
  holder_string_free(json);

  // A fresh context/data_dir, as if this were a different device that has
  // never heard of project-1.
  const auto other_data_dir = holder::test::make_temp_dir();
  holder_context* other_context = nullptr;
  REQUIRE(
      holder_context_open(other_data_dir.string().c_str(), schema.c_str(), &other_context, &error) ==
      HOLDER_OK
  );

  json = nullptr;
  REQUIRE(
      holder_recovery_token_import_global(other_context, "1234", token.c_str(), &json, &error) ==
      HOLDER_OK
  );
  const auto body = nlohmann::json::parse(json);
  REQUIRE(body["project_id"] == "project-1");
  REQUIRE(body["project_created"] == true);
  REQUIRE(body["remote_hint_present"] == false);
  REQUIRE(body["remote_configured"] == false);
  REQUIRE(body["pull_status"] == "not_attempted");
  holder_string_free(json);

  json = nullptr;
  REQUIRE(holder_project_list(other_context, &json, &error) == HOLDER_OK);
  const auto projects = nlohmann::json::parse(json);
  REQUIRE(projects.size() == 1);
  REQUIRE(projects[0]["project_id"] == "project-1");
  REQUIRE(projects[0]["name"] == "Recovered Notes");
  REQUIRE(projects[0]["privacy_mode"] == "encrypted_git");
  holder_string_free(json);

  // Calling it again with an already-recovered project reports project_created=false.
  json = nullptr;
  REQUIRE(
      holder_recovery_token_import_global(other_context, "1234", token.c_str(), &json, &error) ==
      HOLDER_OK
  );
  REQUIRE(nlohmann::json::parse(json)["project_created"] == false);
  holder_string_free(json);

  holder_context_destroy(other_context);
  holder_context_destroy(context);
}

TEST_CASE("C API recovery_token_import_global configures and pulls a remote hint", "[capi][privacy]") {
  const auto data_dir = holder::test::make_temp_dir();
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (data_dir / "keystore").string());

  const auto remote_dir = data_dir / "remote";
  init_bare_repo(remote_dir);

  const auto key_id = seed_encrypted_project(
      data_dir,
      "project-1",
      data_dir / "repo",
      "Synced Notes",
      remote_dir.string()
  );

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  const auto schema = read_schema_sql();
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  // Seed the remote with the project's (encrypted) content so the recovering
  // device has something to pull.
  char* json = nullptr;
  REQUIRE(holder_card_create(context, "project-1", "Secret", "hidden", nullptr, &json, &error) == HOLDER_OK);
  holder_string_free(json);
  REQUIRE(holder_git_push(context, "project-1", nullptr, 1, &json, &error) == HOLDER_OK);
  holder_string_free(json);

  json = nullptr;
  REQUIRE(holder_recovery_token_export(context, "project-1", "1234", &json, &error) == HOLDER_OK);
  const std::string token = nlohmann::json::parse(json)["recovery_token"].get<std::string>();
  holder_string_free(json);

  const auto other_data_dir = holder::test::make_temp_dir();
  holder_context* other_context = nullptr;
  REQUIRE(
      holder_context_open(other_data_dir.string().c_str(), schema.c_str(), &other_context, &error) ==
      HOLDER_OK
  );

  json = nullptr;
  REQUIRE(
      holder_recovery_token_import_global(other_context, "1234", token.c_str(), &json, &error) ==
      HOLDER_OK
  );
  const auto body = nlohmann::json::parse(json);
  REQUIRE(body["project_created"] == true);
  REQUIRE(body["remote_hint_present"] == true);
  REQUIRE(body["remote_configured"] == true);
  REQUIRE(body["pull_status"] == "succeeded");
  holder_string_free(json);

  // The pulled card must actually be visible, not just present in git's working tree --
  // holder_card_list reads from the SQLite index, which nothing but a rebuild after pull
  // keeps in sync with content that arrived from a remote.
  json = nullptr;
  REQUIRE(holder_card_list(other_context, "project-1", &json, &error) == HOLDER_OK);
  const auto cards = nlohmann::json::parse(json);
  REQUIRE(cards.size() == 1);
  REQUIRE(cards[0]["title"] == "Secret");
  holder_string_free(json);

  holder_context_destroy(other_context);
  holder_context_destroy(context);
}

TEST_CASE(
    "C API recovery_token_import_global reports a failed pull when the remote hint is a real but empty repo",
    "[capi][privacy]"
) {
  // The remote is reachable and gets configured successfully, but has no commits at all -- so
  // the pull itself fails with a plain exception (nothing to diverge from), distinct from both
  // "remote unreachable" and the card-level conflict-resolution path.
  const auto data_dir = holder::test::make_temp_dir();
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (data_dir / "keystore").string());

  const auto remote_dir = data_dir / "remote";
  init_bare_repo(remote_dir);

  seed_encrypted_project(data_dir, "project-1", data_dir / "repo", "Synced Notes", remote_dir.string());

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  const auto schema = read_schema_sql();
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(holder_recovery_token_export(context, "project-1", "1234", &json, &error) == HOLDER_OK);
  const std::string token = nlohmann::json::parse(json)["recovery_token"].get<std::string>();
  holder_string_free(json);

  const auto other_data_dir = holder::test::make_temp_dir();
  holder_context* other_context = nullptr;
  REQUIRE(
      holder_context_open(other_data_dir.string().c_str(), schema.c_str(), &other_context, &error) ==
      HOLDER_OK
  );

  json = nullptr;
  REQUIRE(
      holder_recovery_token_import_global(other_context, "1234", token.c_str(), &json, &error) ==
      HOLDER_OK
  );
  const auto body = nlohmann::json::parse(json);
  REQUIRE(body["remote_configured"] == true);
  REQUIRE(body["pull_status"] == "failed");
  REQUIRE_FALSE(body["pull_error"].is_null());
  holder_string_free(json);

  holder_context_destroy(other_context);
  holder_context_destroy(context);
}

// ---------------------------------------------------------------------------
// Argument validation for functions that had no coverage of it at all. Every
// one of these follows holder.cpp's own convention: check first, mutate
// *out_json to null (if applicable), return HOLDER_ERROR_INVALID_ARGUMENT
// with a message naming the bad field.
// ---------------------------------------------------------------------------

TEST_CASE("C API reports invalid context_open arguments", "[capi]") {
  holder_error* error = nullptr;
  holder_context* context = nullptr;
  const auto schema = read_schema_sql();

  REQUIRE(holder_context_open("", schema.c_str(), &context, &error) == HOLDER_ERROR_INVALID_ARGUMENT);
  REQUIRE(context == nullptr);
  REQUIRE(std::string(holder_error_message(error)).find("data_dir") != std::string::npos);
  holder_error_destroy(error);
}

TEST_CASE("C API context_open reports a malformed schema_sql as a runtime error", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  holder_error* error = nullptr;
  holder_context* context = nullptr;

  REQUIRE(
      holder_context_open(data_dir.string().c_str(), "THIS IS NOT VALID SQL;", &context, &error) ==
      HOLDER_ERROR_RUNTIME
  );
  REQUIRE(context == nullptr);
  REQUIRE(error != nullptr);
  holder_error_destroy(error);
}

TEST_CASE("C API reports invalid project_list arguments", "[capi]") {
  holder_error* error = nullptr;
  char* json = nullptr;

  REQUIRE(holder_project_list(nullptr, &json, &error) == HOLDER_ERROR_INVALID_ARGUMENT);
  REQUIRE(json == nullptr);
  REQUIRE(std::string(holder_error_message(error)).find("context") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(holder_project_list(nullptr, nullptr, &error) == HOLDER_ERROR_INVALID_ARGUMENT);
  REQUIRE(std::string(holder_error_message(error)).find("out_json") != std::string::npos);
  holder_error_destroy(error);
}

TEST_CASE("C API reports remaining invalid card_list argument", "[capi]") {
  holder_error* error = nullptr;
  REQUIRE(holder_card_list(nullptr, "project-1", nullptr, &error) == HOLDER_ERROR_INVALID_ARGUMENT);
  REQUIRE(std::string(holder_error_message(error)).find("out_json") != std::string::npos);
  holder_error_destroy(error);
}

TEST_CASE("C API reports remaining invalid card_get_content argument", "[capi]") {
  holder_error* error = nullptr;
  REQUIRE(holder_card_get_content(nullptr, "card-1", nullptr, &error) == HOLDER_ERROR_INVALID_ARGUMENT);
  REQUIRE(std::string(holder_error_message(error)).find("out_content") != std::string::npos);
  holder_error_destroy(error);
}

TEST_CASE("C API reports remaining invalid project_create argument", "[capi]") {
  holder_error* error = nullptr;
  REQUIRE(
      holder_project_create(nullptr, "Home", nullptr, nullptr, nullptr, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("out_json") != std::string::npos);
  holder_error_destroy(error);
}

TEST_CASE("C API creates a project with an explicit root_path", "[capi]") {
  // holder_project_create's "caller supplied root_path" branch -- every other project-create
  // test leaves this null and lets it default.
  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  const auto explicit_root = (data_dir / "custom-root").string();
  char* json = nullptr;
  REQUIRE(
      holder_project_create(context, "Custom", explicit_root.c_str(), nullptr, &json, &error) == HOLDER_OK
  );
  REQUIRE(nlohmann::json::parse(json)["root_path"] == explicit_root);
  holder_string_free(json);
  holder_context_destroy(context);
}

TEST_CASE("C API reports invalid project_rename arguments", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(
      holder_project_rename(context, "project-1", "New Name", nullptr, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("out_json") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_project_rename(context, "", "New Name", &json, &error) == HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("project_id") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_project_rename(context, "project-1", "", &json, &error) == HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("name") != std::string::npos);
  holder_error_destroy(error);

  holder_context_destroy(context);
}

TEST_CASE("C API reports remaining invalid project_delete argument", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  REQUIRE(holder_project_delete(context, "", &error) == HOLDER_ERROR_INVALID_ARGUMENT);
  REQUIRE(std::string(holder_error_message(error)).find("project_id") != std::string::npos);
  holder_error_destroy(error);
  holder_context_destroy(context);
}

TEST_CASE("C API reports remaining invalid card_create arguments", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  REQUIRE(
      holder_card_create(context, "project-1", "Title", "body", nullptr, nullptr, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("out_json") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  char* json = nullptr;
  REQUIRE(
      holder_card_create(context, "", "Title", "body", nullptr, &json, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("project_id") != std::string::npos);
  holder_error_destroy(error);
  holder_context_destroy(context);
}

TEST_CASE("C API creates a card with an explicit parent_card_id", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  seed_git_project(data_dir, "project-1", data_dir / "repo", std::nullopt);
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* parent_json = nullptr;
  REQUIRE(
      holder_card_create(context, "project-1", "Parent", "body", nullptr, &parent_json, &error) ==
      HOLDER_OK
  );
  const std::string parent_id = nlohmann::json::parse(parent_json)["card_id"].get<std::string>();
  holder_string_free(parent_json);

  char* json = nullptr;
  REQUIRE(
      holder_card_create(context, "project-1", "Child", "body", parent_id.c_str(), &json, &error) ==
      HOLDER_OK
  );
  REQUIRE(nlohmann::json::parse(json)["parent_card_id"] == parent_id);
  holder_string_free(json);
  holder_context_destroy(context);
}

TEST_CASE("C API reports invalid card_update_content arguments", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(
      holder_card_update_content(context, "card-1", "content", nullptr, nullptr, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("out_json") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_card_update_content(context, "", "content", nullptr, &json, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("card_id") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_card_update_content(context, "card-1", nullptr, nullptr, &json, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("content") != std::string::npos);
  holder_error_destroy(error);

  holder_context_destroy(context);
}

TEST_CASE("C API reports remaining invalid card_delete argument", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  REQUIRE(holder_card_delete(context, "", &error) == HOLDER_ERROR_INVALID_ARGUMENT);
  REQUIRE(std::string(holder_error_message(error)).find("card_id") != std::string::npos);
  holder_error_destroy(error);
  holder_context_destroy(context);
}

TEST_CASE("C API reports remaining invalid card_search arguments", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  REQUIRE(
      holder_card_search(context, "project-1", "q", 20, 0, nullptr, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("out_json") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  char* json = nullptr;
  REQUIRE(
      holder_card_search(context, "", "q", 20, 0, &json, &error) == HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("project_id") != std::string::npos);
  holder_error_destroy(error);
  holder_context_destroy(context);
}

TEST_CASE("C API reports remaining invalid ensure_default_project arguments", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  REQUIRE(
      holder_ensure_default_project(context, "Home", "Welcome", "body", nullptr, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("out_json") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  char* json = nullptr;
  REQUIRE(
      holder_ensure_default_project(context, "", "Welcome", "body", &json, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("name") != std::string::npos);
  holder_error_destroy(error);
  holder_context_destroy(context);
}

TEST_CASE("C API reports remaining invalid git_set_ssh_signer arguments", "[capi]") {
  holder_error* error = nullptr;
  REQUIRE(
      holder_git_set_ssh_signer(nullptr, "git", nullptr, 0, nullptr, nullptr, nullptr, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("context") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  const unsigned char blob[] = {0x01};
  REQUIRE(
      holder_git_set_ssh_signer(context, "git", nullptr, 0, nullptr, nullptr, nullptr, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("public_key_blob") != std::string::npos);
  holder_error_destroy(error);
  (void)blob;
  holder_context_destroy(context);
}

TEST_CASE("C API reports invalid project_update_git_remote arguments", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  REQUIRE(
      holder_project_update_git_remote(context, "project-1", "ssh://x", nullptr, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("out_json") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  char* json = nullptr;
  REQUIRE(
      holder_project_update_git_remote(nullptr, "project-1", "ssh://x", &json, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("context") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_project_update_git_remote(context, "", "ssh://x", &json, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("project_id") != std::string::npos);
  holder_error_destroy(error);
  holder_context_destroy(context);
}

TEST_CASE("C API reports remaining invalid git_test_remote arguments", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  REQUIRE(
      holder_git_test_remote(context, "project-1", nullptr, nullptr, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("out_json") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  char* json = nullptr;
  REQUIRE(
      holder_git_test_remote(nullptr, "project-1", nullptr, &json, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("context") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_git_test_remote(context, "", nullptr, &json, &error) == HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("project_id") != std::string::npos);
  holder_error_destroy(error);
  holder_context_destroy(context);
}

TEST_CASE("C API reports invalid git_push arguments", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  REQUIRE(
      holder_git_push(context, "project-1", nullptr, 0, nullptr, &error) == HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("out_json") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  char* json = nullptr;
  REQUIRE(
      holder_git_push(nullptr, "project-1", nullptr, 0, &json, &error) == HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("context") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_git_push(context, "", nullptr, 0, &json, &error) == HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("project_id") != std::string::npos);
  holder_error_destroy(error);
  holder_context_destroy(context);
}

TEST_CASE("C API reports invalid git_pull arguments", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  REQUIRE(holder_git_pull(context, "project-1", nullptr, &error) == HOLDER_ERROR_INVALID_ARGUMENT);
  REQUIRE(std::string(holder_error_message(error)).find("out_json") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  char* json = nullptr;
  REQUIRE(holder_git_pull(nullptr, "project-1", &json, &error) == HOLDER_ERROR_INVALID_ARGUMENT);
  REQUIRE(std::string(holder_error_message(error)).find("context") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(holder_git_pull(context, "", &json, &error) == HOLDER_ERROR_INVALID_ARGUMENT);
  REQUIRE(std::string(holder_error_message(error)).find("project_id") != std::string::npos);
  holder_error_destroy(error);
  holder_context_destroy(context);
}

TEST_CASE("C API reports invalid git_sync_status arguments", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  REQUIRE(
      holder_git_sync_status(context, "project-1", nullptr, &error) == HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("out_json") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  char* json = nullptr;
  REQUIRE(
      holder_git_sync_status(nullptr, "project-1", &json, &error) == HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("context") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(holder_git_sync_status(context, "", &json, &error) == HOLDER_ERROR_INVALID_ARGUMENT);
  REQUIRE(std::string(holder_error_message(error)).find("project_id") != std::string::npos);
  holder_error_destroy(error);
  holder_context_destroy(context);
}

TEST_CASE("C API reports invalid git_sync_if_due arguments", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  REQUIRE(
      holder_git_sync_if_due(context, "project-1", 0, 0, nullptr, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("out_json") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  char* json = nullptr;
  REQUIRE(
      holder_git_sync_if_due(nullptr, "project-1", 0, 0, &json, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("context") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_git_sync_if_due(context, "", 0, 0, &json, &error) == HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("project_id") != std::string::npos);
  holder_error_destroy(error);
  holder_context_destroy(context);
}

TEST_CASE("C API reports invalid encryption_check arguments", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  REQUIRE(
      holder_encryption_check(context, "project-1", nullptr, &error) == HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("out_json") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  char* json = nullptr;
  REQUIRE(
      holder_encryption_check(nullptr, "project-1", &json, &error) == HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("context") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(holder_encryption_check(context, "", &json, &error) == HOLDER_ERROR_INVALID_ARGUMENT);
  REQUIRE(std::string(holder_error_message(error)).find("project_id") != std::string::npos);
  holder_error_destroy(error);
  holder_context_destroy(context);
}

TEST_CASE("C API reports remaining invalid recovery_token_export arguments", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  REQUIRE(
      holder_recovery_token_export(context, "project-1", "1234", nullptr, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("out_json") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  char* json = nullptr;
  REQUIRE(
      holder_recovery_token_export(nullptr, "project-1", "1234", &json, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("context") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_recovery_token_export(context, "", "1234", &json, &error) == HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("project_id") != std::string::npos);
  holder_error_destroy(error);
  holder_context_destroy(context);
}

TEST_CASE("C API reports invalid recovery_token_import arguments", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  REQUIRE(
      holder_recovery_token_import(context, "project-1", "1234", "token", nullptr, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("out_json") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  char* json = nullptr;
  REQUIRE(
      holder_recovery_token_import(nullptr, "project-1", "1234", "token", &json, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("context") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_recovery_token_import(context, "", "1234", "token", &json, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("project_id") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_recovery_token_import(context, "project-1", "", "token", &json, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("pin") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_recovery_token_import(context, "project-1", "1234", "", &json, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("recovery_token") != std::string::npos);
  holder_error_destroy(error);
  holder_context_destroy(context);
}

TEST_CASE("C API reports invalid recovery_token_inspect arguments", "[capi]") {
  holder_error* error = nullptr;
  REQUIRE(
      holder_recovery_token_inspect("1234", "token", nullptr, &error) == HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("out_json") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  char* json = nullptr;
  REQUIRE(
      holder_recovery_token_inspect("", "token", &json, &error) == HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("pin") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_recovery_token_inspect("1234", "", &json, &error) == HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("recovery_token") != std::string::npos);
  holder_error_destroy(error);
}

TEST_CASE("C API reports invalid recovery_token_import_global arguments", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  REQUIRE(
      holder_recovery_token_import_global(context, "1234", "token", nullptr, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("out_json") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  char* json = nullptr;
  REQUIRE(
      holder_recovery_token_import_global(nullptr, "1234", "token", &json, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("context") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_recovery_token_import_global(context, "", "token", &json, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("pin") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_recovery_token_import_global(context, "1234", "", &json, &error) ==
      HOLDER_ERROR_INVALID_ARGUMENT
  );
  REQUIRE(std::string(holder_error_message(error)).find("recovery_token") != std::string::npos);
  holder_error_destroy(error);
  holder_context_destroy(context);
}

TEST_CASE("C API reports project not found for functions that had no coverage of it", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(
      holder_project_update_git_remote(context, "missing", "ssh://x", &json, &error) ==
      HOLDER_ERROR_RUNTIME
  );
  REQUIRE(std::string(holder_error_message(error)).find("not found") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_git_test_remote(context, "missing", nullptr, &json, &error) == HOLDER_ERROR_RUNTIME
  );
  REQUIRE(std::string(holder_error_message(error)).find("not found") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(holder_git_push(context, "missing", nullptr, 0, &json, &error) == HOLDER_ERROR_RUNTIME);
  REQUIRE(std::string(holder_error_message(error)).find("not found") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(holder_git_pull(context, "missing", &json, &error) == HOLDER_ERROR_RUNTIME);
  REQUIRE(std::string(holder_error_message(error)).find("not found") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(holder_git_sync_status(context, "missing", &json, &error) == HOLDER_ERROR_RUNTIME);
  REQUIRE(std::string(holder_error_message(error)).find("not found") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(holder_encryption_check(context, "missing", &json, &error) == HOLDER_ERROR_RUNTIME);
  REQUIRE(std::string(holder_error_message(error)).find("not found") != std::string::npos);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_recovery_token_export(context, "missing", "1234", &json, &error) == HOLDER_ERROR_RUNTIME
  );
  REQUIRE(std::string(holder_error_message(error)).find("not found") != std::string::npos);
  holder_error_destroy(error);

  holder_context_destroy(context);
}

TEST_CASE("C API git_pull reports remote not configured", "[capi][git]") {
  const auto data_dir = holder::test::make_temp_dir();
  seed_git_project(data_dir, "project-1", data_dir / "repo", std::nullopt);
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(holder_git_pull(context, "project-1", &json, &error) == HOLDER_OK);
  const auto body = nlohmann::json::parse(json);
  REQUIRE(body["status"] == "failed");
  REQUIRE(body["error_message"] == "Remote URL is not configured.");
  holder_string_free(json);
  holder_context_destroy(context);
}

TEST_CASE("C API card_get_content reports missing content file", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  seed_git_project(data_dir, "project-1", data_dir / "repo", std::nullopt);
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* created_json = nullptr;
  REQUIRE(
      holder_card_create(context, "project-1", "Title", "body", nullptr, &created_json, &error) ==
      HOLDER_OK
  );
  const auto created = nlohmann::json::parse(created_json);
  const std::string card_id = created["card_id"].get<std::string>();
  const std::string rel_path = created["rel_path"].get<std::string>();
  holder_string_free(created_json);

  // Simulate the card file having vanished from disk without the DB row knowing.
  REQUIRE(std::filesystem::remove(data_dir / "repo" / rel_path));

  char* content = nullptr;
  REQUIRE(holder_card_get_content(context, card_id.c_str(), &content, &error) == HOLDER_ERROR_RUNTIME);
  REQUIRE(std::string(holder_error_message(error)).find("content missing") != std::string::npos);
  holder_error_destroy(error);
  holder_context_destroy(context);
}

TEST_CASE("C API card_update_content reports card not found", "[capi]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(
      holder_card_update_content(context, "missing-card", "content", nullptr, &json, &error) ==
      HOLDER_ERROR_RUNTIME
  );
  REQUIRE(std::string(holder_error_message(error)).find("not found") != std::string::npos);
  holder_error_destroy(error);
  holder_context_destroy(context);
}

TEST_CASE("C API git_sync_if_due succeeds on a real pull and rebuilds the index", "[capi][git]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto remote_dir = data_dir / "remote";
  init_bare_repo(remote_dir);

  seed_git_project(data_dir, "project-1", data_dir / "repo", remote_dir.string());
  holder_context* writer_context = nullptr;
  holder_error* error = nullptr;
  const auto schema = read_schema_sql();
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &writer_context, &error) == HOLDER_OK);
  char* json = nullptr;
  REQUIRE(
      holder_card_create(writer_context, "project-1", "Seed", "body", nullptr, &json, &error) == HOLDER_OK
  );
  holder_string_free(json);
  json = nullptr;
  REQUIRE(holder_git_push(writer_context, "project-1", nullptr, 1, &json, &error) == HOLDER_OK);
  holder_string_free(json);
  holder_context_destroy(writer_context);

  const auto reader_data_dir = holder::test::make_temp_dir();
  seed_git_project(reader_data_dir, "project-1", reader_data_dir / "repo", remote_dir.string());
  holder_context* reader_context = nullptr;
  REQUIRE(
      holder_context_open(reader_data_dir.string().c_str(), schema.c_str(), &reader_context, &error) ==
      HOLDER_OK
  );

  json = nullptr;
  REQUIRE(holder_git_sync_if_due(reader_context, "project-1", 3600, 3600, &json, &error) == HOLDER_OK);
  const auto body = nlohmann::json::parse(json);
  REQUIRE(body["pull_status"] == "succeeded");
  REQUIRE(body["pull_conflicts_resolved"] == 0);
  holder_string_free(json);

  json = nullptr;
  REQUIRE(holder_card_list(reader_context, "project-1", &json, &error) == HOLDER_OK);
  REQUIRE(nlohmann::json::parse(json).size() == 1);
  holder_string_free(json);

  holder_context_destroy(reader_context);
}

TEST_CASE("C API git_sync_if_due resolves a diverged pull card-level", "[capi][git]") {
  const auto data_dir = holder::test::make_temp_dir();
  const auto remote_dir = data_dir / "remote";
  init_bare_repo(remote_dir);
  const auto schema = read_schema_sql();
  holder_error* error = nullptr;
  char* json = nullptr;

  seed_git_project(data_dir, "project-1", data_dir / "repo", remote_dir.string());
  holder_context* peer_a = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &peer_a, &error) == HOLDER_OK);
  REQUIRE(
      holder_card_create(peer_a, "project-1", "Shared Card", "v0", nullptr, &json, &error) == HOLDER_OK
  );
  const std::string shared_card_id = nlohmann::json::parse(json)["card_id"].get<std::string>();
  holder_string_free(json);
  json = nullptr;
  REQUIRE(holder_git_push(peer_a, "project-1", nullptr, 1, &json, &error) == HOLDER_OK);
  holder_string_free(json);

  // Clone peer_b's local working tree directly, bypassing the C ABI -- calling
  // holder_git_pull here would record a pull result, and the sync_if_due call below (with a
  // long interval, relying on "no prior state" to be due) needs there to be none yet.
  const auto peer_b_data_dir = holder::test::make_temp_dir();
  const auto peer_b_repo_dir = peer_b_data_dir / "repo";
  holder::git::GitRepo peer_b_git;
  peer_b_git.open_or_init(peer_b_repo_dir);
  peer_b_git.set_remote("origin", remote_dir.string());
  peer_b_git.pull_remote_ff_only("origin");

  seed_git_project(peer_b_data_dir, "project-1", peer_b_repo_dir, remote_dir.string());
  holder_context* peer_b = nullptr;
  REQUIRE(
      holder_context_open(peer_b_data_dir.string().c_str(), schema.c_str(), &peer_b, &error) == HOLDER_OK
  );
  {
    holder::platform::Db peer_b_db;
    peer_b_db.open(peer_b_data_dir / "server" / "holder.db");
    holder::index::FtsIndexer peer_b_fts(peer_b_db);
    holder::project::ProjectRepo peer_b_projects(peer_b_db);
    const auto peer_b_project = peer_b_projects.get("project-1").value();
    holder::store::Rebuilder(peer_b_db, &peer_b_fts).rebuild_project(peer_b_project);
  }

  json = nullptr;
  REQUIRE(
      holder_card_update_content(peer_a, shared_card_id.c_str(), "v1 from A", nullptr, &json, &error) ==
      HOLDER_OK
  );
  holder_string_free(json);
  json = nullptr;
  REQUIRE(holder_git_push(peer_a, "project-1", nullptr, 1, &json, &error) == HOLDER_OK);
  holder_string_free(json);

  json = nullptr;
  REQUIRE(
      holder_card_update_content(peer_b, shared_card_id.c_str(), "v1 from B", nullptr, &json, &error) ==
      HOLDER_OK
  );
  holder_string_free(json);

  json = nullptr;
  REQUIRE(holder_git_sync_if_due(peer_b, "project-1", 3600, 3600, &json, &error) == HOLDER_OK);
  const auto body = nlohmann::json::parse(json);
  REQUIRE(body["pull_status"] == "succeeded");
  REQUIRE(body["pull_conflicts_resolved"] == 1);
  holder_string_free(json);

  json = nullptr;
  REQUIRE(holder_card_list(peer_b, "project-1", &json, &error) == HOLDER_OK);
  REQUIRE(nlohmann::json::parse(json).size() == 2);
  holder_string_free(json);

  holder_context_destroy(peer_a);
  holder_context_destroy(peer_b);
}

TEST_CASE("C API recovery_token_import_global resolves a diverged pull card-level", "[capi][privacy]") {
  const auto data_dir = holder::test::make_temp_dir();
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (data_dir / "keystore").string());
  const auto remote_dir = data_dir / "remote";
  init_bare_repo(remote_dir);

  const auto key_id = seed_encrypted_project(
      data_dir,
      "project-1",
      data_dir / "repo",
      "Synced Notes",
      remote_dir.string()
  );

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  const auto schema = read_schema_sql();
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(
      holder_card_create(context, "project-1", "Shared", "v0", nullptr, &json, &error) == HOLDER_OK
  );
  const std::string shared_card_id = nlohmann::json::parse(json)["card_id"].get<std::string>();
  holder_string_free(json);
  json = nullptr;
  REQUIRE(holder_git_push(context, "project-1", nullptr, 1, &json, &error) == HOLDER_OK);
  holder_string_free(json);

  json = nullptr;
  REQUIRE(holder_recovery_token_export(context, "project-1", "1234", &json, &error) == HOLDER_OK);
  const std::string token = nlohmann::json::parse(json)["recovery_token"].get<std::string>();
  holder_string_free(json);

  // Recover onto a second device, then diverge from what's already on the remote.
  const auto other_data_dir = holder::test::make_temp_dir();
  holder_context* other_context = nullptr;
  REQUIRE(
      holder_context_open(other_data_dir.string().c_str(), schema.c_str(), &other_context, &error) ==
      HOLDER_OK
  );
  json = nullptr;
  REQUIRE(
      holder_recovery_token_import_global(other_context, "1234", token.c_str(), &json, &error) ==
      HOLDER_OK
  );
  holder_string_free(json);

  json = nullptr;
  REQUIRE(
      holder_card_update_content(context, shared_card_id.c_str(), "v1 from original", nullptr, &json, &error) ==
      HOLDER_OK
  );
  holder_string_free(json);
  json = nullptr;
  REQUIRE(holder_git_push(context, "project-1", nullptr, 1, &json, &error) == HOLDER_OK);
  holder_string_free(json);

  json = nullptr;
  REQUIRE(
      holder_card_update_content(other_context, shared_card_id.c_str(), "v1 from recovered device", nullptr, &json, &error) ==
      HOLDER_OK
  );
  holder_string_free(json);

  // Re-importing the same token pulls again -- this time into a diverged history.
  json = nullptr;
  REQUIRE(
      holder_recovery_token_import_global(other_context, "1234", token.c_str(), &json, &error) ==
      HOLDER_OK
  );
  const auto body = nlohmann::json::parse(json);
  REQUIRE(body["project_created"] == false);
  REQUIRE(body["pull_status"] == "succeeded");
  holder_string_free(json);

  json = nullptr;
  REQUIRE(holder_card_list(other_context, "project-1", &json, &error) == HOLDER_OK);
  const auto cards = nlohmann::json::parse(json);
  REQUIRE(cards.size() == 2);
  holder_string_free(json);

  holder_context_destroy(context);
  holder_context_destroy(other_context);
}

TEST_CASE(
    "C API reports the underlying sqlite error when the projects table is missing",
    "[capi]"
) {
  // Every function below starts its try block by touching the "projects" table (directly or via
  // ProjectRepo::get), so dropping it out from under an already-open context is the cheapest way
  // to exercise each one's generic catch(const std::exception&) branch for real, rather than via
  // the "project not found" early-return path.
  const auto data_dir = holder::test::make_temp_dir();
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  {
    holder::platform::Db raw_db;
    raw_db.open(data_dir / "server" / "holder.db");
    raw_db.exec("DROP TABLE projects;");
  }

  char* json = nullptr;
  REQUIRE(holder_project_list(context, &json, &error) == HOLDER_ERROR_RUNTIME);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_project_create(context, "Name", nullptr, nullptr, &json, &error) == HOLDER_ERROR_RUNTIME
  );
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_project_rename(context, "project-1", "New name", &json, &error) == HOLDER_ERROR_RUNTIME
  );
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(holder_project_delete(context, "project-1", &error) == HOLDER_ERROR_RUNTIME);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_ensure_default_project(context, "Home", "Welcome", nullptr, &json, &error) ==
      HOLDER_ERROR_RUNTIME
  );
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_project_update_git_remote(context, "project-1", "ssh://x", &json, &error) ==
      HOLDER_ERROR_RUNTIME
  );
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_git_test_remote(context, "project-1", nullptr, &json, &error) == HOLDER_ERROR_RUNTIME
  );
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(holder_git_push(context, "project-1", nullptr, 0, &json, &error) == HOLDER_ERROR_RUNTIME);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(holder_git_pull(context, "project-1", &json, &error) == HOLDER_ERROR_RUNTIME);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(holder_git_sync_status(context, "project-1", &json, &error) == HOLDER_ERROR_RUNTIME);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_git_sync_if_due(context, "project-1", 0, 0, &json, &error) == HOLDER_ERROR_RUNTIME
  );
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(holder_encryption_check(context, "project-1", &json, &error) == HOLDER_ERROR_RUNTIME);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_recovery_token_export(context, "project-1", "1234", &json, &error) ==
      HOLDER_ERROR_RUNTIME
  );
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_recovery_token_import(context, "project-1", "1234", "not-a-real-token", &json, &error) ==
      HOLDER_ERROR_RUNTIME
  );
  holder_error_destroy(error);

  holder_context_destroy(context);
}

TEST_CASE(
    "C API reports the underlying sqlite error when the cards table is missing",
    "[capi]"
) {
  const auto data_dir = holder::test::make_temp_dir();
  seed_git_project(data_dir, "project-1", data_dir / "repo", std::nullopt);
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(
      holder_card_create(context, "project-1", "Title", "content", nullptr, &json, &error) == HOLDER_OK
  );
  const std::string card_id = nlohmann::json::parse(json)["card_id"].get<std::string>();
  holder_string_free(json);

  {
    holder::platform::Db raw_db;
    raw_db.open(data_dir / "server" / "holder.db");
    raw_db.exec("DROP TABLE cards;");
  }

  json = nullptr;
  REQUIRE(holder_card_list(context, "project-1", &json, &error) == HOLDER_ERROR_RUNTIME);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(holder_card_get_content(context, card_id.c_str(), &json, &error) == HOLDER_ERROR_RUNTIME);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_card_create(context, "project-1", "Another", "content", nullptr, &json, &error) ==
      HOLDER_ERROR_RUNTIME
  );
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(
      holder_card_update_content(context, card_id.c_str(), "new content", nullptr, &json, &error) ==
      HOLDER_ERROR_RUNTIME
  );
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(holder_card_delete(context, card_id.c_str(), &error) == HOLDER_ERROR_RUNTIME);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(holder_card_list_trashed(context, "project-1", &json, &error) == HOLDER_ERROR_RUNTIME);
  holder_error_destroy(error);
  error = nullptr;

  REQUIRE(holder_reindex(context, &error) == HOLDER_ERROR_RUNTIME);
  holder_error_destroy(error);

  holder_context_destroy(context);
}

TEST_CASE(
    "C API card_search reports the underlying sqlite error when cards_fts is missing",
    "[capi]"
) {
  const auto data_dir = holder::test::make_temp_dir();
  seed_git_project(data_dir, "project-1", data_dir / "repo", std::nullopt);
  const auto schema = read_schema_sql();
  holder_context* context = nullptr;
  holder_error* error = nullptr;
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  {
    holder::platform::Db raw_db;
    raw_db.open(data_dir / "server" / "holder.db");
    raw_db.exec("DROP TABLE cards_fts;");
  }

  char* json = nullptr;
  REQUIRE(
      holder_card_search(context, "project-1", "query", 10, 0, &json, &error) == HOLDER_ERROR_RUNTIME
  );
  holder_error_destroy(error);

  holder_context_destroy(context);
}

TEST_CASE(
    "C API recovery_token_inspect and recovery_token_import_global report a wrong pin as a runtime error",
    "[capi][privacy]"
) {
  const auto data_dir = holder::test::make_temp_dir();
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (data_dir / "keystore").string());
  seed_encrypted_project(data_dir, "project-1", data_dir / "repo", "My Notes");

  holder_context* context = nullptr;
  holder_error* error = nullptr;
  const auto schema = read_schema_sql();
  REQUIRE(holder_context_open(data_dir.string().c_str(), schema.c_str(), &context, &error) == HOLDER_OK);

  char* json = nullptr;
  REQUIRE(holder_recovery_token_export(context, "project-1", "1234", &json, &error) == HOLDER_OK);
  const std::string token = nlohmann::json::parse(json)["recovery_token"].get<std::string>();
  holder_string_free(json);

  // The wrong pin fails to decrypt the token's envelope -- a genuine std::exception, not a
  // validation early-return, since inspect_recovery_token/import_recovery_token don't know the
  // pin is wrong until decryption itself fails.
  json = nullptr;
  REQUIRE(holder_recovery_token_inspect("0000", token.c_str(), &json, &error) == HOLDER_ERROR_RUNTIME);
  REQUIRE(json == nullptr);
  holder_error_destroy(error);
  error = nullptr;

  json = nullptr;
  REQUIRE(
      holder_recovery_token_import_global(context, "0000", token.c_str(), &json, &error) ==
      HOLDER_ERROR_RUNTIME
  );
  REQUIRE(json == nullptr);
  holder_error_destroy(error);

  holder_context_destroy(context);
}

TEST_CASE("C API error_message returns an empty string for a null error", "[capi]") {
  REQUIRE(std::string(holder_error_message(nullptr)) == "");
}
