#include "git/RepoSyncMetrics.h"

#include <git2.h>

#include <climits>
#include <cstddef>
#include <stdexcept>
#include <string>

namespace holder::git {
namespace {

std::runtime_error git_err(const std::string& what, int rc) {
  const git_error* e = git_error_last();
  std::string message = what + " (rc=" + std::to_string(rc) + ")";
  if (e != nullptr && e->message != nullptr) {
    message += ": ";
    message += e->message;
  }
  return std::runtime_error(message);
}

int clamp_size_t_to_int(std::size_t value) {
  if (value > static_cast<std::size_t>(INT_MAX)) {
    return INT_MAX; // LCOV_EXCL_LINE
  }
  return static_cast<int>(value);
}

bool is_ignored_uncommitted_path(const char* path) {
  if (path == nullptr) {
    return false; // LCOV_EXCL_LINE
  }
  return std::string(path) == ".holder/privacy.json";
}

int filtered_uncommitted_count(git_status_list* status_list) {
  const std::size_t n = git_status_list_entrycount(status_list);
  int count = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const git_status_entry* entry = git_status_byindex(status_list, i);
    if (entry == nullptr) {
      continue; // LCOV_EXCL_LINE
    }
    const char* path = nullptr;
    if (entry->index_to_workdir != nullptr && entry->index_to_workdir->new_file.path != nullptr) {
      path = entry->index_to_workdir->new_file.path;
    } else if (entry->head_to_index != nullptr && entry->head_to_index->new_file.path != nullptr) {
      path = entry->head_to_index->new_file.path;
    }
    if (is_ignored_uncommitted_path(path)) {
      continue;
    }
    count += 1;
  }
  return count;
}

std::string current_local_branch_name(git_repository* repo) {
  git_reference* head = nullptr;
  const int rc = git_repository_head(&head, repo);
  if (rc == GIT_EUNBORNBRANCH || rc == GIT_ENOTFOUND) {
    return {};
  }
  if (rc != 0 || head == nullptr) {
    throw git_err("git_repository_head failed", rc); // LCOV_EXCL_LINE
  }

  const char* full_name = git_reference_name(head);
  std::string branch;
  if (full_name != nullptr) {
    constexpr const char* kHeadsPrefix = "refs/heads/";
    const std::string ref(full_name);
    if (ref.rfind(kHeadsPrefix, 0) == 0) {
      branch = ref.substr(std::char_traits<char>::length(kHeadsPrefix));
    }
  }
  git_reference_free(head);
  return branch;
}

} // namespace

RepoSyncMetrics inspect_repo_sync_metrics(
    const std::filesystem::path& repo_dir,
    const std::string& remote_name
) {
  RepoSyncMetrics metrics;
  git_libgit2_init();

  git_repository* repo = nullptr;
  int rc = git_repository_open(&repo, repo_dir.string().c_str());
  if (rc == GIT_ENOTFOUND) {
    git_libgit2_shutdown();
    return metrics;
  }
  if (rc != 0 || repo == nullptr) {
    git_libgit2_shutdown();
    throw git_err("git_repository_open failed", rc);
  }

  git_status_options status_opts{};
  rc = git_status_options_init(&status_opts, GIT_STATUS_OPTIONS_VERSION);
  if (rc != 0) throw git_err("git_status_options_init failed", rc); // LCOV_EXCL_LINE
  status_opts.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
  status_opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS |
                      GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX;
  git_status_list* status_list = nullptr;
  rc = git_status_list_new(&status_list, repo, &status_opts);
  if (rc != 0 || status_list == nullptr) {
    git_repository_free(repo);
    git_libgit2_shutdown();
    throw git_err("git_status_list_new failed", rc);
  }
  metrics.uncommitted_changes_count = filtered_uncommitted_count(status_list);
  git_status_list_free(status_list);

  const std::string branch = current_local_branch_name(repo);
  if (branch.empty()) {
    metrics.unpushed_commits_count = 0;
    git_repository_free(repo);
    git_libgit2_shutdown();
    return metrics;
  }

  git_reference* head_ref = nullptr;
  rc = git_reference_lookup(&head_ref, repo, ("refs/heads/" + branch).c_str());
  if (rc != 0 || head_ref == nullptr) {
    git_repository_free(repo); // LCOV_EXCL_LINE
    git_libgit2_shutdown(); // LCOV_EXCL_LINE
    throw git_err("git_reference_lookup for local branch failed", rc); // LCOV_EXCL_LINE
  }
  const git_oid* local_oid = git_reference_target(head_ref);
  if (local_oid == nullptr) {
    git_reference_free(head_ref); // LCOV_EXCL_LINE
    git_repository_free(repo); // LCOV_EXCL_LINE
    git_libgit2_shutdown(); // LCOV_EXCL_LINE
    throw std::runtime_error("Local branch has no target oid"); // LCOV_EXCL_LINE
  }

  git_reference* remote_ref = nullptr;
  rc = git_reference_lookup(
      &remote_ref,
      repo,
      ("refs/remotes/" + remote_name + "/" + branch).c_str()
  );
  if (rc == GIT_ENOTFOUND) {
    git_reference_free(head_ref);
    // Missing remote-tracking ref is common immediately after push in this local clone.
    // Treat as unknown/clean rather than "all local commits unpushed".
    metrics.unpushed_commits_count = 0;
    git_repository_free(repo);
    git_libgit2_shutdown();
    return metrics;
  }
  if (rc != 0 || remote_ref == nullptr) {
    git_reference_free(head_ref);
    git_repository_free(repo);
    git_libgit2_shutdown();
    throw git_err("git_reference_lookup for remote branch failed", rc);
  }
  const git_oid* remote_oid = git_reference_target(remote_ref);
  if (remote_oid == nullptr) {
    git_reference_free(remote_ref);
    git_reference_free(head_ref);
    git_repository_free(repo);
    git_libgit2_shutdown();
    throw std::runtime_error("Remote branch has no target oid");
  }

  size_t ahead = 0;
  size_t behind = 0;
  rc = git_graph_ahead_behind(&ahead, &behind, repo, local_oid, remote_oid);
  (void)behind;
  git_reference_free(remote_ref);
  git_reference_free(head_ref);
  git_repository_free(repo);
  git_libgit2_shutdown();
  if (rc != 0) {
    throw git_err("git_graph_ahead_behind failed", rc);
  }
  metrics.unpushed_commits_count = clamp_size_t_to_int(ahead);
  return metrics;
}

} // namespace holder::git
