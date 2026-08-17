#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "git/PushResult.h"

TEST_CASE("push_status_name covers all PushStatus values", "[git]") {
  using holder::git::push_status_name;
  using holder::git::PushStatus;

  REQUIRE(std::string(push_status_name(PushStatus::Pushed)) == "pushed");
  REQUIRE(std::string(push_status_name(PushStatus::UpToDate)) == "up_to_date");
  REQUIRE(std::string(push_status_name(PushStatus::AuthFailed)) == "auth_failed");
  REQUIRE(std::string(push_status_name(PushStatus::NotFound)) == "not_found");
  REQUIRE(std::string(push_status_name(PushStatus::NetworkError)) == "network_error");
  REQUIRE(std::string(push_status_name(PushStatus::NonFastForward)) == "non_fast_forward");
  REQUIRE(std::string(push_status_name(PushStatus::RemoteUnset)) == "remote_unset");
  REQUIRE(std::string(push_status_name(PushStatus::UnknownError)) == "unknown_error");
}
