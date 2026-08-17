#pragma once

#include <optional>

namespace holder::sync {

struct PushDecisionInput {
  std::optional<long long> last_push_at;
  std::optional<long long> next_retry_at;
  long long now = 0;
  int push_interval_seconds = 1200;
};

struct PullDecisionInput {
  std::optional<long long> last_pull_at;
  std::optional<long long> next_pull_retry_at;
  long long now = 0;
  int pull_interval_seconds = 300;
};

bool should_attempt_push(const PushDecisionInput& input);
bool should_attempt_pull(const PullDecisionInput& input);

} // namespace holder::sync
