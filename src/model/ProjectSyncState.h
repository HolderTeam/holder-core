#pragma once

#include <optional>
#include <string>

namespace holder::model {

struct ProjectSyncState {
  std::string project_id;
  std::optional<long long> last_commit_at;
  std::optional<long long> last_push_at;
  std::optional<long long> last_pull_at;
  int uncommitted_changes_count = 0;
  int unpushed_commits_count = 0;
  std::optional<std::string> last_push_status;
  std::optional<std::string> last_pull_status;
  std::optional<std::string> last_sync_error;
  std::optional<long long> last_sync_error_at;
  int retry_count = 0;
  std::optional<long long> next_retry_at;
  int pull_retry_count = 0;
  std::optional<long long> next_pull_retry_at;
  long long updated_at = 0;
};

} // namespace holder::model
