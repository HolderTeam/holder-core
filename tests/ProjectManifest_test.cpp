#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "core_test_helpers.h"
#include "git/GitOps.h"
#include "model/Project.h"
#include "privacy/ProjectPrivacy.h"
#include "project/ProjectManifest.h"
#include "project/ProjectRepo.h"

#include <filesystem>
#include <fstream>
#include <iterator>

namespace {

std::string read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("plain project manifest round trips stable identity and metadata", "[project][manifest]") {
  const auto dir = holder::test::make_temp_dir();
  holder::model::Project project;
  project.project_id = "plain-project-id";
  project.name = "Recipes";
  project.root_path = (dir / "recipes").string();
  project.git_remote_url = "https://example.com/holder/recipes.git";
  project.git_provider = "github";
  project.privacy_mode = "plain";
  project.created_at = 101;
  project.updated_at = 202;

  holder::git::RealGitOps git;
  holder::project::write_project_manifest(git, project);

  const auto recovered = holder::project::read_project_manifest(project.root_path);
  REQUIRE(recovered.project_id == project.project_id);
  REQUIRE(recovered.name == project.name);
  REQUIRE(recovered.root_path == project.root_path);
  REQUIRE(recovered.git_remote_url == project.git_remote_url);
  REQUIRE(recovered.git_provider == project.git_provider);
  REQUIRE(recovered.privacy_mode == "plain");
  REQUIRE_FALSE(recovered.project_key_id.has_value());
  REQUIRE(recovered.created_at == project.created_at);
  REQUIRE(recovered.updated_at == project.updated_at);
}

TEST_CASE("encrypted project manifest does not expose its name", "[project][manifest][privacy]") {
  const auto dir = holder::test::make_temp_dir();
  const auto keystore_dir = dir / "keystore";
  std::filesystem::create_directories(keystore_dir);
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", keystore_dir.string());

  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::project::ProjectRepo repo(db);
  holder::model::Project project;
  project.project_id = "encrypted-project-id";
  project.name = "Private family records";
  project.root_path = (dir / "private").string();
  project.privacy_mode = "encrypted_git";
  project.created_at = 303;
  project.updated_at = 404;
  repo.create(project);

  holder::git::RealGitOps git;
  holder::privacy::ensure_encrypted_project_ready(
      git,
      repo,
      project.project_id,
      project.root_path,
      project.project_key_id,
      project.updated_at,
      [] { return std::string("manifest-key"); }
  );
  project = *repo.get(project.project_id);
  holder::project::write_project_manifest(git, project);

  const auto raw = read_file(
      std::filesystem::path(project.root_path) / holder::project::kProjectManifestPath
  );
  REQUIRE(raw.rfind("HolderPriv1\n", 0) == 0);
  REQUIRE(raw.find(project.name) == std::string::npos);

  const auto recovered = holder::project::read_project_manifest(project.root_path);
  REQUIRE(recovered.project_id == project.project_id);
  REQUIRE(recovered.name == project.name);
  REQUIRE(recovered.project_key_id == project.project_key_id);
}

TEST_CASE("project manifest rejects bootstrap and payload identity mismatch", "[project][manifest]") {
  const auto dir = holder::test::make_temp_dir();
  holder::model::Project project;
  project.project_id = "project-one";
  project.name = "One";
  project.root_path = (dir / "one").string();
  project.privacy_mode = "plain";
  project.created_at = 1;
  project.updated_at = 1;

  holder::git::RealGitOps git;
  holder::project::write_project_manifest(git, project);
  git.write_file(
      holder::project::kProjectBootstrapPath,
      R"({"version":1,"project_id":"project-two","mode":"plain"})"
  );

  REQUIRE_THROWS(holder::project::read_project_manifest(project.root_path));
}
