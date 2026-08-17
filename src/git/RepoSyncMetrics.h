#pragma once

#include <filesystem>
#include <string>

namespace holder::git {

struct RepoSyncMetrics {
  int uncommitted_changes_count = 0;
  int unpushed_commits_count = 0;
};

RepoSyncMetrics inspect_repo_sync_metrics(
    const std::filesystem::path& repo_dir,
    const std::string& remote_name = "origin"
);

} // namespace holder::git
