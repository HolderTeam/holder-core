#pragma once

#include <filesystem>
#include <memory>
#include <mutex>

namespace holder::git {

class GitOperationGuard {
 public:
  GitOperationGuard(
      std::recursive_mutex& git_ops_mutex,
      const std::filesystem::path& repo_root
  );

  GitOperationGuard(const GitOperationGuard&) = delete;
  GitOperationGuard& operator=(const GitOperationGuard&) = delete;
  GitOperationGuard(GitOperationGuard&&) = delete;
  GitOperationGuard& operator=(GitOperationGuard&&) = delete;

 private:
  // Take the project lock first so legitimate same-project nesting through
  // distinct GitOps objects cannot deadlock while acquiring an inner object.
  std::shared_ptr<std::recursive_mutex> repo_mutex_;
  std::unique_lock<std::recursive_mutex> repo_lock_;
  // A GitOps implementation owns mutable repository state. This additionally
  // prevents one shared adapter from interleaving whole logical operations.
  std::unique_lock<std::recursive_mutex> git_ops_lock_;
};

// Process-wide registry of per-repository locks.
//
// Every Git-mutating (and, for now, Git-reading) logical operation in
// holder-core explicitly holds a GitOperationGuard. The guard takes the lock
// returned here before the first repository-dependent work and releases it on
// every return and exception path after the associated Git/DB work is done.
//
// The lock is a recursive_mutex: a single thread legitimately nests store
// objects for one project (the backup-restore flow constructs a ProjectStore
// and then a CardStore for the freshly created project, both alive at once),
// and each would otherwise re-lock the same repository on the same thread.
//
// Locks are keyed by std::filesystem::weakly_canonical(root), so operations on
// different projects use independent locks and run concurrently. The registry
// holds weak references; the mutex for a repository lives only while some
// operation guard still holds it.
std::shared_ptr<std::recursive_mutex> repo_mutex_for(const std::filesystem::path& repo_root);

// Normalizes a repository root to the key used by repo_mutex_for.
std::filesystem::path canonical_repo_key(const std::filesystem::path& repo_root);

} // namespace holder::git
