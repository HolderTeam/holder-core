#include "sync/ProjectSyncPolicy.h"

namespace holder::sync {

bool should_attempt_push(const PushDecisionInput& input) {
  if (input.next_retry_at.has_value() && input.now < input.next_retry_at.value()) {
    return false;
  }

  if (!input.last_push_at.has_value()) {
    return true;
  }

  const long long elapsed = input.now - input.last_push_at.value();
  return elapsed >= static_cast<long long>(input.push_interval_seconds);
}

bool should_attempt_pull(const PullDecisionInput& input) {
  if (input.next_pull_retry_at.has_value() && input.now < input.next_pull_retry_at.value()) {
    return false;
  }

  if (!input.last_pull_at.has_value()) {
    return true;
  }

  const long long elapsed = input.now - input.last_pull_at.value();
  return elapsed >= static_cast<long long>(input.pull_interval_seconds);
}

} // namespace holder::sync
