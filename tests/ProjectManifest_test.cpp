#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "core_test_helpers.h"
#include "git/GitOps.h"
#include "model/Project.h"
#include "privacy/ProjectPrivacy.h"
#include "project/ProjectManifest.h"
#include "project/ProjectRepo.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>

namespace {

std::string read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE(
    "plain project manifest round trips stable identity and metadata",
    "[project][manifest]"
) {
  const auto dir = holder::test::make_temp_dir();
  holder::model::Project project;
  project.project_id = "plain-project-id";
  project.name = "Recipes";
  project.root_path = (dir / "recipes").string();
  project.git_remote_url = "https://example.com/holder/recipes.git";
  project.git_provider = "github";
  project.privacy_mode = "plain";
  project.id_scheme = holder::model::IdScheme::Uuid7;
  project.created_at = 101;
  project.updated_at = 202;

  holder::git::RealGitOps git;
  holder::project::write_project_manifest(git, project);

  const auto manifest = nlohmann::json::parse(
      read_file(std::filesystem::path(project.root_path) / holder::project::kProjectManifestPath)
  );
  REQUIRE(manifest.at("id_scheme") == "uuid7");

  const auto recovered = holder::project::read_project_manifest(project.root_path);
  REQUIRE(recovered.project_id == project.project_id);
  REQUIRE(recovered.name == project.name);
  REQUIRE(recovered.root_path == project.root_path);
  REQUIRE(recovered.git_remote_url == project.git_remote_url);
  REQUIRE(recovered.git_provider == project.git_provider);
  REQUIRE(recovered.privacy_mode == "plain");
  REQUIRE(recovered.id_scheme == holder::model::IdScheme::Uuid7);
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
  project.id_scheme = holder::model::IdScheme::Uuid7;
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
      [] {
        return std::string("manifest-key");
      }
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
  REQUIRE(recovered.id_scheme == holder::model::IdScheme::Uuid7);
}

TEST_CASE("project manifest missing id_scheme defaults to UUID4", "[project][manifest]") {
  const auto root = holder::test::make_temp_dir() / "legacy-project";
  std::filesystem::create_directories(root / ".holder");
  std::ofstream(root / holder::project::kProjectBootstrapPath)
      << R"({"version":1,"project_id":"project-1","mode":"plain"})";
  std::ofstream(root / holder::project::kProjectManifestPath)
      << R"({"version":1,"project_id":"project-1","name":"Legacy","created_at":1,"updated_at":2})";

  const auto recovered = holder::project::read_project_manifest(root);
  REQUIRE(recovered.id_scheme == holder::model::IdScheme::Uuid4);
}

TEST_CASE(
    "project manifest rejects bootstrap and payload identity mismatch",
    "[project][manifest]"
) {
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

  REQUIRE_THROWS_WITH(
      holder::project::read_project_manifest(project.root_path),
      Catch::Matchers::ContainsSubstring("project manifest id does not match bootstrap")
  );
}

TEST_CASE("project manifest validates render inputs", "[project][manifest]") {
  holder::model::Project project;
  project.name = "Project";
  project.privacy_mode = "plain";
  REQUIRE_THROWS_WITH(
      holder::project::render_project_bootstrap(project),
      Catch::Matchers::ContainsSubstring("project_id must not be empty")
  );

  project.project_id = "project-1";
  project.privacy_mode = "unknown";
  REQUIRE_THROWS_WITH(
      holder::project::render_project_bootstrap(project),
      Catch::Matchers::ContainsSubstring("unsupported project privacy mode: unknown")
  );

  project.privacy_mode = "encrypted_git";
  REQUIRE_THROWS_WITH(
      holder::project::render_project_bootstrap(project),
      Catch::Matchers::ContainsSubstring("encrypted project must have project_key_id")
  );
  REQUIRE_THROWS_WITH(
      holder::project::render_project_manifest(project),
      Catch::Matchers::ContainsSubstring("encrypted project must have project_key_id")
  );

  project.privacy_mode = "plain";
  project.name.clear();
  REQUIRE_THROWS_WITH(
      holder::project::render_project_manifest(project),
      Catch::Matchers::ContainsSubstring("project manifest requires project_id and name")
  );
}

TEST_CASE("project manifest rejects malformed durable metadata", "[project][manifest]") {
  const auto root = holder::test::make_temp_dir() / "project";
  const auto holder_dir = root / ".holder";
  std::filesystem::create_directories(holder_dir);
  const auto bootstrap_path = root / holder::project::kProjectBootstrapPath;
  const auto manifest_path = root / holder::project::kProjectManifestPath;
  const auto write_metadata = [&](const std::string& bootstrap, const std::string& manifest) {
    std::ofstream(bootstrap_path, std::ios::trunc) << bootstrap;
    std::ofstream(manifest_path, std::ios::trunc) << manifest;
  };
  const std::string valid_bootstrap = R"({"version":1,"project_id":"project-1","mode":"plain"})";
  const std::string valid_manifest =
      R"({"version":1,"project_id":"project-1","name":"Project","created_at":1,"updated_at":2})";

  REQUIRE_THROWS_WITH(
      holder::project::read_project_manifest(root / "missing"),
      Catch::Matchers::ContainsSubstring("failed to open project metadata")
  );

  write_metadata(R"({"version":2})", valid_manifest);
  REQUIRE_THROWS_WITH(
      holder::project::read_project_manifest(root),
      Catch::Matchers::ContainsSubstring("unsupported project metadata version")
  );

  write_metadata(R"({"version":1,"mode":"plain"})", valid_manifest);
  REQUIRE_THROWS_WITH(
      holder::project::read_project_manifest(root),
      Catch::Matchers::ContainsSubstring("project metadata field 'project_id' is missing")
  );

  write_metadata(R"({"version":1,"project_id":"","mode":"plain"})", valid_manifest);
  REQUIRE_THROWS_WITH(
      holder::project::read_project_manifest(root),
      Catch::Matchers::ContainsSubstring("project metadata field 'project_id' is empty")
  );

  write_metadata(R"({"version":1,"project_id":"project-1","mode":"unknown"})", valid_manifest);
  REQUIRE_THROWS_WITH(
      holder::project::read_project_manifest(root),
      Catch::Matchers::ContainsSubstring("unsupported project privacy mode 'unknown")
  );

  write_metadata(
      R"({"version":1,"project_id":"project-1","mode":"encrypted_git"})",
      valid_manifest
  );
  REQUIRE_THROWS_WITH(
      holder::project::read_project_manifest(root),
      Catch::Matchers::ContainsSubstring("encrypted project bootstrap has no key_id")
  );

  write_metadata(valid_bootstrap, R"({"version":2})");
  REQUIRE_THROWS_WITH(
      holder::project::read_project_manifest(root),
      Catch::Matchers::ContainsSubstring("unsupported project metadata version")
  );

  write_metadata(
      valid_bootstrap,
      R"({"version":1,"project_id":"project-1","created_at":1,"updated_at":2})"
  );
  REQUIRE_THROWS_WITH(
      holder::project::read_project_manifest(root),
      Catch::Matchers::ContainsSubstring("project metadata field 'name' is missing")
  );

  write_metadata(
      valid_bootstrap,
      R"({"version":1,"project_id":"project-1","name":"","created_at":1,"updated_at":2})"
  );
  REQUIRE_THROWS_WITH(
      holder::project::read_project_manifest(root),
      Catch::Matchers::ContainsSubstring("project metadata field 'name' is empty")
  );

  write_metadata(
      valid_bootstrap,
      R"({"version":1,"project_id":"project-1","name":"Project","id_scheme":"uuid8","created_at":1,"updated_at":2})"
  );
  REQUIRE_THROWS_WITH(
      holder::project::read_project_manifest(root),
      Catch::Matchers::ContainsSubstring("unsupported project id scheme 'uuid8")
  );

  write_metadata(
      valid_bootstrap,
      R"({"version":1,"project_id":"project-1","name":"Project","id_scheme":7,"created_at":1,"updated_at":2})"
  );
  REQUIRE_THROWS_WITH(
      holder::project::read_project_manifest(root),
      Catch::Matchers::ContainsSubstring("project metadata field 'id_scheme' is not a string")
  );
}
