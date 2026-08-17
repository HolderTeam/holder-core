#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "git/RemoteProbe.h"

TEST_CASE("remote_probe_status_name covers all RemoteProbeStatus values", "[git]") {
  using holder::git::remote_probe_status_name;
  using holder::git::RemoteProbeStatus;

  REQUIRE(std::string(remote_probe_status_name(RemoteProbeStatus::Reachable)) == "reachable");
  REQUIRE(std::string(remote_probe_status_name(RemoteProbeStatus::AuthFailed)) == "auth_failed");
  REQUIRE(std::string(remote_probe_status_name(RemoteProbeStatus::NotFound)) == "not_found");
  REQUIRE(
      std::string(remote_probe_status_name(RemoteProbeStatus::NetworkError)) == "network_error"
  );
  REQUIRE(
      std::string(remote_probe_status_name(RemoteProbeStatus::InvalidRemoteUrl)) ==
      "invalid_remote_url"
  );
  REQUIRE(std::string(remote_probe_status_name(RemoteProbeStatus::RemoteUnset)) == "remote_unset");
  REQUIRE(
      std::string(remote_probe_status_name(RemoteProbeStatus::UnknownError)) == "unknown_error"
  );
}
