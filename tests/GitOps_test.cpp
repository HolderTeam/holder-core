#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "git/GitOps.h"
#include "core_test_helpers.h"

TEST_CASE("RealGitOps probe_remote throws when repo is not opened", "[git]") {
  holder::git::RealGitOps ops;
  REQUIRE_THROWS(ops.probe_remote("origin"));
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
