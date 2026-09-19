#pragma once

#include "git/GitCredentialProvider.h"
#include "git/GitRepo.h"
#include "git/PushResult.h"
#include "git/RemoteProbe.h"
#include "git/RepoLocks.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace holder::git {

class GitOps {
 public:
  virtual ~GitOps() = default; // LCOV_EXCL_LINE

  // The returned non-movable guard is deliberately lexical: it must be
  // destroyed by the same thread that acquired it. Recursive locking supports
  // same-thread nesting by store operations and their helper operations.
  [[nodiscard]] GitOperationGuard lock_operation(
      const std::filesystem::path& repo_dir
  ) {
    return GitOperationGuard(operation_mutex_, repo_dir);
  }

  // Default no-op: implementations that don't override this keep whatever
  // default credential behavior their underlying GitRepo already has (see
  // GitRepo::set_credential_provider). Test doubles are not required to
  // override it.
  virtual void set_credential_provider(std::shared_ptr<GitCredentialProvider> /*provider*/) {}

  virtual void open_or_init(const std::filesystem::path& repo_dir) = 0;
  virtual void write_file(
      const std::filesystem::path& relative_path,
      const std::string& content
  ) = 0;
  virtual void stage_path(const std::filesystem::path& relative_path) = 0;
  // Default: repeated stage_path calls. RealGitOps overrides this to add every path to a
  // single open index and write it once, instead of reopening/rewriting the whole index file
  // per path -- see GitRepo::stage_paths. Callers staging more than a handful of paths at once
  // (CardStore::create_batch's tens-of-thousands-of-cards snapshot restore) should call this,
  // not loop stage_path, or they pay stage_path's per-call index rewrite anyway.
  virtual void stage_paths(const std::vector<std::filesystem::path>& relative_paths) {
    for (const auto& path : relative_paths) {
      stage_path(path);
    }
  }
  virtual void remove_path(const std::filesystem::path& relative_path) = 0;
  virtual void commit(const std::string& message) = 0;
  virtual void set_remote(const std::string& name, const std::string& url) = 0;
  virtual void remove_remote(const std::string& name) = 0;
  virtual void pull_remote_ff_only(const std::string& name) = 0;
  virtual RemoteProbeResult probe_remote(const std::string& name) = 0;
  // Probe an explicit URL without opening or changing a repository. Implementations
  // must reuse their configured credential provider. No fetch or ref updates occur.
  virtual RemoteProbeResult probe_remote_url(const std::string&) {
    return {
        RemoteProbeStatus::UnknownError,
        false,
        "URL probes are not supported by this Git adapter."
    };
  }
  virtual PushResult push_branch(
      const std::string& name,
      const std::string& branch,
      bool set_upstream
  ) = 0;
  // These operations are needed only when a pull reports divergence. Defaults keep
  // lightweight adapters source-compatible while making pull orchestration reusable
  // outside the C API's concrete RealGitOps path.
  virtual GitRepo::DivergedMergeResult merge_remote_taking_theirs_for_conflicts( // LCOV_EXCL_START - explicit unsupported adapter defaults.
      const std::string&,
      const std::string&,
      const std::string&
  ) {
    throw std::runtime_error("Diverged pull resolution is not supported by this Git adapter.");
  } // LCOV_EXCL_STOP
  virtual std::optional<std::string> read_blob_at( // LCOV_EXCL_START
      const std::string&,
      const std::filesystem::path&
  ) {
    throw std::runtime_error("Historical blob reads are not supported by this Git adapter.");
  } // LCOV_EXCL_STOP
  virtual std::filesystem::path repo_dir() const = 0;

 private:
  std::recursive_mutex operation_mutex_;
};

class RealGitOps final : public GitOps {
 public:
  void set_credential_provider(std::shared_ptr<GitCredentialProvider> provider) override;
  void open_or_init(const std::filesystem::path& repo_dir) override;
  void write_file(const std::filesystem::path& relative_path, const std::string& content) override;
  void stage_path(const std::filesystem::path& relative_path) override;
  void stage_paths(const std::vector<std::filesystem::path>& relative_paths) override;
  void remove_path(const std::filesystem::path& relative_path) override;
  void commit(const std::string& message) override;
  void set_remote(const std::string& name, const std::string& url) override;
  void remove_remote(const std::string& name) override;
  void pull_remote_ff_only(const std::string& name) override;
  RemoteProbeResult probe_remote(const std::string& name) override;
  RemoteProbeResult probe_remote_url(const std::string& url) override;
  PushResult push_branch(const std::string& name, const std::string& branch, bool set_upstream)
      override;
  std::filesystem::path repo_dir() const override;

  GitRepo::DivergedMergeResult merge_remote_taking_theirs_for_conflicts(
      const std::string& name,
      const std::string& local_oid_hex,
      const std::string& remote_oid_hex
  ) override;
  std::optional<std::string> read_blob_at(
      const std::string& commit_oid_hex,
      const std::filesystem::path& relative_path
  ) override;

 private:
  GitRepo repo_;
};

} // namespace holder::git
