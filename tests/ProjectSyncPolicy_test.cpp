#include "sync/ProjectSyncPolicy.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("ProjectSyncPolicy allows first push when never pushed", "[sync][policy]") {
  const bool decision = holder::sync::should_attempt_push(
      {.last_push_at = std::nullopt,
       .next_retry_at = std::nullopt,
       .now = 1000,
       .push_interval_seconds = 1200}
  );
  REQUIRE(decision);
}

TEST_CASE("ProjectSyncPolicy blocks push before retry window", "[sync][policy]") {
  const bool decision = holder::sync::should_attempt_push(
      {.last_push_at = 1000, .next_retry_at = 1400, .now = 1200, .push_interval_seconds = 1200}
  );
  REQUIRE_FALSE(decision);
}

TEST_CASE("ProjectSyncPolicy allows push after interval and retry gate", "[sync][policy]") {
  const bool decision = holder::sync::should_attempt_push(
      {.last_push_at = 1000, .next_retry_at = 1300, .now = 2500, .push_interval_seconds = 1200}
  );
  REQUIRE(decision);
}

TEST_CASE("ProjectSyncPolicy allows first pull when never pulled", "[sync][policy]") {
  const bool decision = holder::sync::should_attempt_pull(
      {.last_pull_at = std::nullopt,
       .next_pull_retry_at = std::nullopt,
       .now = 1000,
       .pull_interval_seconds = 300}
  );
  REQUIRE(decision);
}

TEST_CASE("ProjectSyncPolicy blocks pull before retry window", "[sync][policy]") {
  const bool decision = holder::sync::should_attempt_pull(
      {.last_pull_at = 1000, .next_pull_retry_at = 1400, .now = 1200, .pull_interval_seconds = 300}
  );
  REQUIRE_FALSE(decision);
}

TEST_CASE("ProjectSyncPolicy allows pull after interval and retry gate", "[sync][policy]") {
  const bool decision = holder::sync::should_attempt_pull(
      {.last_pull_at = 1000, .next_pull_retry_at = 1200, .now = 1500, .pull_interval_seconds = 300}
  );
  REQUIRE(decision);
}
