#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "ai/AiThreadManifest.h"
#include "core_test_helpers.h"
#include "git/GitOps.h"
#include "privacy/ProjectPrivacy.h"
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

TEST_CASE("AI thread manifest preserves title and card association", "[ai][thread][manifest]") {
  const auto dir = holder::test::make_temp_dir();
  holder::model::Project project;
  project.project_id = "project-thread";
  project.name = "Threads";
  project.root_path = (dir / "project").string();
  project.privacy_mode = "plain";

  holder::model::AiThread thread;
  thread.thread_id = "thread-1234";
  thread.project_id = project.project_id;
  thread.card_id = "card-1234";
  thread.title = "A useful title";
  thread.created_at = 10;
  thread.updated_at = 20;

  holder::git::RealGitOps git;
  holder::ai::write_ai_thread_manifest(git, project, thread);
  const auto path = std::filesystem::path(project.root_path) /
                    holder::ai::ai_thread_manifest_rel_path(thread.thread_id);
  const auto recovered = holder::ai::read_ai_thread_manifest(project, path);
  REQUIRE(recovered.thread_id == thread.thread_id);
  REQUIRE(recovered.project_id == thread.project_id);
  REQUIRE(recovered.card_id == thread.card_id);
  REQUIRE(recovered.title == thread.title);
  REQUIRE(recovered.created_at == thread.created_at);
  REQUIRE(recovered.updated_at == thread.updated_at);
}

TEST_CASE("encrypted AI thread manifest hides title", "[ai][thread][manifest][privacy]") {
  const auto dir = holder::test::make_temp_dir();
  const auto keystore = dir / "keystore";
  std::filesystem::create_directories(keystore);
  holder::test::EnvGuard key_env("HOLDER_TEST_KEYSTORE_DIR", keystore.string());
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  holder::model::Project project;
  project.project_id = "encrypted-thread-project";
  project.name = "Encrypted";
  project.root_path = (dir / "encrypted").string();
  project.privacy_mode = "encrypted_git";
  project.created_at = 1;
  project.updated_at = 1;
  holder::project::ProjectRepo projects(db);
  projects.create(project);
  holder::git::RealGitOps git;
  holder::privacy::ensure_encrypted_project_ready(
      git, projects, project.project_id, project.root_path, std::nullopt, 1,
      [] { return std::string("thread-key"); }
  );
  project = *projects.get(project.project_id);

  holder::model::AiThread thread;
  thread.thread_id = "thread-secret";
  thread.project_id = project.project_id;
  thread.title = "Secret conversation";
  thread.created_at = 1;
  thread.updated_at = 1;
  holder::ai::write_ai_thread_manifest(git, project, thread);

  const auto path = std::filesystem::path(project.root_path) /
                    holder::ai::ai_thread_manifest_rel_path(thread.thread_id);
  const auto raw = read_file(path);
  REQUIRE(raw.rfind("HolderPriv1\n", 0) == 0);
  REQUIRE(raw.find(thread.title) == std::string::npos);
  REQUIRE(holder::ai::read_ai_thread_manifest(project, path).title == thread.title);
}
