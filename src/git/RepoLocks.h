#pragma once

#include <filesystem>
#include <memory>
#include <mutex>

namespace holder::git {

// Process-wide registry of per-repository locks.
//
// Every Git-mutating (and, for now, Git-reading) operation in holder-core runs
// through a RealGitOps whose open_or_init(root) call establishes which project
// it is touching. That call takes the lock returned here for the canonical
// repository root and holds it for the whole RealGitOps operation, so two
// concurrent operations on the same on-disk repository can never share or free
// each other's git_repository*.
//
// The lock is a recursive_mutex: a single thread legitimately nests store
// objects for one project (the backup-restore flow constructs a ProjectStore
// and then a CardStore for the freshly created project, both alive at once),
// and each would otherwise re-lock the same repository on the same thread.
//
// Locks are keyed by std::filesystem::weakly_canonical(root), so operations on
// different projects use independent locks and run concurrently. The registry
// holds weak references; the mutex for a repository lives only while some
// RealGitOps still holds it.
std::shared_ptr<std::recursive_mutex> repo_mutex_for(const std::filesystem::path& repo_root);

// Normalizes a repository root to the key used by repo_mutex_for. Exposed so a
// holder can tell whether a second open_or_init targets the same repository it
// already locked.
std::filesystem::path canonical_repo_key(const std::filesystem::path& repo_root);

} // namespace holder::git
