#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "git/GitRepo.h"
#include "git/RepoSyncMetrics.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <git2.h>

namespace {

std::filesystem::path make_temp_dir() {
  const auto base = std::filesystem::temp_directory_path();
  const auto suffix = std::to_string(
      static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())
  );
  auto dir = base / ("holder_repo_sync_metrics_test_" + suffix);
  std::filesystem::create_directories(dir);
  return dir;
}

} // namespace

TEST_CASE("RepoSyncMetrics ignores .holder/privacy.json in uncommitted count", "[git][sync]") {
  const auto dir = make_temp_dir();
  holder::git::GitRepo repo;
  repo.open_or_init(dir);

  std::filesystem::create_directories(dir / ".holder");
  {
    std::ofstream privacy_file(dir / ".holder" / "privacy.json");
    REQUIRE(privacy_file.is_open());
    privacy_file << "{\"version\":1}";
  }
  {
    std::ofstream card_file(dir / "cards.md");
    REQUIRE(card_file.is_open());
    card_file << "card body";
  }

  const auto metrics = holder::git::inspect_repo_sync_metrics(dir);
  REQUIRE(metrics.uncommitted_changes_count == 1);
}

TEST_CASE(
    "RepoSyncMetrics returns zero unpushed when remote-tracking ref is missing",
    "[git][sync]"
) {
  const auto dir = make_temp_dir();
  holder::git::GitRepo repo;
  repo.open_or_init(dir);
  repo.write_file("cards/a.md", "hello");
  repo.stage_path("cards/a.md");
  repo.commit("seed");

  const auto metrics = holder::git::inspect_repo_sync_metrics(dir);
  REQUIRE(metrics.unpushed_commits_count == 0);
}

TEST_CASE("RepoSyncMetrics counts staged index changes via head_to_index path", "[git][sync]") {
  const auto dir = make_temp_dir();
  holder::git::GitRepo repo;
  repo.open_or_init(dir);
  repo.write_file("cards/a.md", "v1");
  repo.stage_path("cards/a.md");
  repo.commit("seed");

  // Stage an update and leave workdir clean so status path is sourced from head_to_index.
  repo.write_file("cards/a.md", "v2");
  repo.stage_path("cards/a.md");

  const auto metrics = holder::git::inspect_repo_sync_metrics(dir);
  REQUIRE(metrics.uncommitted_changes_count == 1);
}

TEST_CASE("RepoSyncMetrics throws for invalid remote name lookup", "[git][sync]") {
  const auto dir = make_temp_dir();
  holder::git::GitRepo repo;
  repo.open_or_init(dir);
  repo.write_file("cards/a.md", "hello");
  repo.stage_path("cards/a.md");
  repo.commit("seed");

  REQUIRE_THROWS(holder::git::inspect_repo_sync_metrics(dir, ".."));
}

TEST_CASE("RepoSyncMetrics throws when repo open hits filesystem error", "[git][sync]") {
#ifdef _WIN32
  SKIP("Windows symlink-loop handling does not produce the same repo-open failure");
#else
  const auto dir = make_temp_dir();
  const auto loop_path = dir / "loop";
  std::filesystem::create_symlink("loop", loop_path);
  REQUIRE_THROWS(holder::git::inspect_repo_sync_metrics(loop_path));
#endif
}

TEST_CASE("RepoSyncMetrics throws when HEAD is malformed", "[git][sync]") {
  const auto dir = make_temp_dir();
  holder::git::GitRepo repo;
  repo.open_or_init(dir);
  repo.write_file("cards/a.md", "hello");
  repo.stage_path("cards/a.md");
  repo.commit("seed");

  std::ofstream head_file(dir / ".git" / "HEAD", std::ios::trunc);
  REQUIRE(head_file.is_open());
  head_file << "this-is-not-a-valid-head\n";
  head_file.close();

  try {
    (void)holder::git::inspect_repo_sync_metrics(dir);
    FAIL("Expected inspect_repo_sync_metrics to throw for malformed HEAD");
  } catch (const std::runtime_error& e) {
    const std::string msg(e.what());
    REQUIRE(
        (msg.find("git_repository_head failed") != std::string::npos ||
         msg.find("git_status_list_new failed") != std::string::npos)
    );
  }
}

TEST_CASE("RepoSyncMetrics throws when remote ref has no direct target oid", "[git][sync]") {
  const auto dir = make_temp_dir();
  holder::git::GitRepo repo;
  repo.open_or_init(dir);
  repo.write_file("cards/a.md", "hello");
  repo.stage_path("cards/a.md");
  repo.commit("seed");

  git_repository* raw = nullptr;
  REQUIRE(git_repository_open(&raw, dir.string().c_str()) == 0);

  git_reference* head_ref = nullptr;
  REQUIRE(git_repository_head(&head_ref, raw) == 0);
  const char* head_name = git_reference_name(head_ref);
  REQUIRE(head_name != nullptr);
  const std::string head_ref_name(head_name);
  REQUIRE(head_ref_name.rfind("refs/heads/", 0) == 0);
  const std::string branch = head_ref_name.substr(std::string("refs/heads/").size());
  git_reference_free(head_ref);

  git_reference* remote_ref = nullptr;
  const std::string remote_ref_name = "refs/remotes/origin/" + branch;
  REQUIRE(
      git_reference_symbolic_create(
          &remote_ref,
          raw,
          remote_ref_name.c_str(),
          "refs/remotes/origin/missing",
          1,
          nullptr
      ) == 0
  );
  git_reference_free(remote_ref);
  git_repository_free(raw);

  try {
    (void)holder::git::inspect_repo_sync_metrics(dir, "origin");
    FAIL("Expected inspect_repo_sync_metrics to throw for symbolic remote ref");
  } catch (const std::runtime_error& e) {
    REQUIRE(std::string(e.what()).find("Remote branch has no target oid") != std::string::npos);
  }
}

TEST_CASE(
    "RepoSyncMetrics throws when graph ahead-behind cannot resolve remote oid",
    "[git][sync]"
) {
  const auto dir = make_temp_dir();
  holder::git::GitRepo repo;
  repo.open_or_init(dir);
  repo.write_file("cards/a.md", "hello");
  repo.stage_path("cards/a.md");
  repo.commit("seed");

  git_repository* raw = nullptr;
  REQUIRE(git_repository_open(&raw, dir.string().c_str()) == 0);

  git_reference* head_ref = nullptr;
  REQUIRE(git_repository_head(&head_ref, raw) == 0);
  const char* head_name = git_reference_name(head_ref);
  REQUIRE(head_name != nullptr);
  const std::string head_ref_name(head_name);
  REQUIRE(head_ref_name.rfind("refs/heads/", 0) == 0);
  const std::string branch = head_ref_name.substr(std::string("refs/heads/").size());
  git_reference_free(head_ref);

  const std::string remote_ref_name = "refs/remotes/origin/" + branch;
  const auto remote_ref_path = dir / ".git" / remote_ref_name;
  std::filesystem::create_directories(remote_ref_path.parent_path());
  std::ofstream remote_ref_file(remote_ref_path, std::ios::trunc);
  REQUIRE(remote_ref_file.is_open());
  remote_ref_file << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n";
  remote_ref_file.close();
  git_repository_free(raw);

  try {
    (void)holder::git::inspect_repo_sync_metrics(dir, "origin");
    FAIL("Expected inspect_repo_sync_metrics to throw for invalid remote graph target");
  } catch (const std::runtime_error& e) {
    REQUIRE(std::string(e.what()).find("git_graph_ahead_behind failed") != std::string::npos);
  }
}

TEST_CASE("RepoSyncMetrics malformed HEAD can fail at git_repository_head", "[git][sync]") {
  const auto dir = make_temp_dir();
  holder::git::GitRepo repo;
  repo.open_or_init(dir);
  repo.write_file("cards/a.md", "hello");
  repo.stage_path("cards/a.md");
  repo.commit("seed");

  std::ofstream head_file(dir / ".git" / "HEAD", std::ios::trunc);
  REQUIRE(head_file.is_open());
  head_file << "ref: refs/heads/\n";
  head_file.close();

  try {
    (void)holder::git::inspect_repo_sync_metrics(dir);
    FAIL("Expected inspect_repo_sync_metrics to throw for malformed HEAD ref");
  } catch (const std::runtime_error& e) {
    const std::string msg(e.what());
    REQUIRE(
        (msg.find("git_repository_head failed") != std::string::npos ||
         msg.find("git_status_list_new failed") != std::string::npos)
    );
  }
}
