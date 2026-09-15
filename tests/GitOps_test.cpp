#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "git/GitOps.h"
#include "git/RepoLocks.h"
#include "core_test_helpers.h"

#include <fstream>
#include <sstream>

TEST_CASE("GitOps default credential-provider hook is a no-op", "[git]") {
  holder::git::RealGitOps ops;
  REQUIRE_NOTHROW(ops.GitOps::set_credential_provider(nullptr));
}

TEST_CASE("GitOps default stage_paths stages every supplied path", "[git]") {
  const auto dir = holder::test::make_temp_dir() / "repo";
  holder::git::RealGitOps ops;
  ops.open_or_init(dir);
  ops.write_file("first.txt", "first");
  ops.write_file("second.txt", "second");

  REQUIRE_NOTHROW(ops.GitOps::stage_paths({"first.txt", "second.txt"}));
  REQUIRE_NOTHROW(ops.commit("Stage through the default batch implementation"));
}

TEST_CASE("canonical_repo_key tolerates an empty unresolved path", "[git]") {
  REQUIRE(holder::git::canonical_repo_key({}).empty());
  const auto oversized = std::filesystem::path("/") / std::string(5000, 'x');
  REQUIRE(holder::git::canonical_repo_key(oversized) == oversized.lexically_normal());
}

TEST_CASE("RealGitOps probe_remote throws when repo is not opened", "[git]") {
  holder::git::RealGitOps ops;
  REQUIRE_THROWS(ops.probe_remote("origin"));
}

TEST_CASE("URL probes use detached remotes and preserve repository configuration", "[git][probe-url]") {
  const auto dir = holder::test::make_temp_dir();
  holder::git::GitRepo remote;
  remote.open_or_init(dir / "remote");
  remote.write_file("seed.txt", "seed");
  remote.stage_path("seed.txt");
  remote.commit("Seed remote");
  holder::git::RealGitOps ops;
  const auto detached = ops.probe_remote_url((dir / "remote").string());
  REQUIRE(detached.status == holder::git::RemoteProbeStatus::Reachable);
  REQUIRE(detached.remote_has_head);
  REQUIRE(ops.repo_dir().empty());

  ops.open_or_init(dir / "local");
  ops.set_remote("origin", (dir / "original-remote").string());
  const auto config = dir / "local" / ".git" / "config";
  auto read_config = [&] {
    std::ifstream in(config, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
  };
  const auto before = read_config();
  const auto probe = ops.probe_remote_url((dir / "remote").string());
  REQUIRE(probe.status == holder::git::RemoteProbeStatus::Reachable);
  REQUIRE(probe.remote_has_head);
  REQUIRE(probe.error_message.empty());
  REQUIRE(read_config() == before);
  REQUIRE(ops.probe_remote_url("").status == holder::git::RemoteProbeStatus::RemoteUnset);
  const auto invalid = ops.probe_remote_url("nosuchscheme://example.invalid/repo.git");
  CAPTURE(invalid.error_message);
  REQUIRE(invalid.status == holder::git::RemoteProbeStatus::InvalidRemoteUrl);
  REQUIRE(ops.probe_remote_url((dir / "missing").string()).status !=
          holder::git::RemoteProbeStatus::Reachable);
  REQUIRE(read_config() == before);
  REQUIRE(ops.probe_remote("origin").status != holder::git::RemoteProbeStatus::Reachable);
}

TEST_CASE("URL probes leave absent origins absent", "[git][probe-url]") {
  const auto dir = holder::test::make_temp_dir();
  holder::git::RealGitOps remote;
  remote.open_or_init(dir / "remote");
  holder::git::RealGitOps ops;
  ops.open_or_init(dir / "local");
  REQUIRE(ops.probe_remote_url((dir / "remote").string()).status ==
          holder::git::RemoteProbeStatus::Reachable);
  REQUIRE(ops.probe_remote("origin").status == holder::git::RemoteProbeStatus::RemoteUnset);
  REQUIRE(ops.GitOps::probe_remote_url("url").status == holder::git::RemoteProbeStatus::UnknownError);
}

TEST_CASE("RealGitOps push_branch throws when repo is not opened", "[git]") {
  holder::git::RealGitOps ops;
  REQUIRE_THROWS(ops.push_branch("origin", "cards", true));
}

TEST_CASE("RealGitOps probe_remote delegates to repo after open", "[git]") {
  const auto dir = holder::test::make_temp_dir();
  holder::git::RealGitOps ops;
  ops.open_or_init(dir / "repo");

  const auto result = ops.probe_remote("origin");
  REQUIRE(result.status == holder::git::RemoteProbeStatus::RemoteUnset);
  REQUIRE(result.remote_has_head == false);
}

TEST_CASE("RealGitOps delegates remote mutation and pull methods", "[git]") {
  const auto dir = holder::test::make_temp_dir();
  holder::git::RealGitOps ops;
  ops.open_or_init(dir / "repo");

  ops.set_remote("origin", "https://example.invalid/holder.git");
  const auto configured = ops.probe_remote("origin");
  REQUIRE(configured.status != holder::git::RemoteProbeStatus::RemoteUnset);

  REQUIRE_THROWS(ops.pull_remote_ff_only("origin"));

  ops.remove_remote("origin");
  const auto removed = ops.probe_remote("origin");
  REQUIRE(removed.status == holder::git::RemoteProbeStatus::RemoteUnset);
}
