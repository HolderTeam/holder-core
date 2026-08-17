#pragma once

#include "git/GitRepo.h"
#include "git/PushResult.h"
#include "git/RemoteProbe.h"

#include <filesystem>
#include <string>

namespace holder::git {

class GitOps {
 public:
  virtual ~GitOps() = default; // LCOV_EXCL_LINE

  virtual void open_or_init(const std::filesystem::path& repo_dir) = 0;
  virtual void write_file(
      const std::filesystem::path& relative_path,
      const std::string& content
  ) = 0;
  virtual void stage_path(const std::filesystem::path& relative_path) = 0;
  virtual void remove_path(const std::filesystem::path& relative_path) = 0;
  virtual void commit(const std::string& message) = 0;
  virtual void set_remote(const std::string& name, const std::string& url) = 0;
  virtual void remove_remote(const std::string& name) = 0;
  virtual void pull_remote_ff_only(const std::string& name) = 0;
  virtual RemoteProbeResult probe_remote(const std::string& name) = 0;
  virtual PushResult push_branch(
      const std::string& name,
      const std::string& branch,
      bool set_upstream
  ) = 0;
  virtual std::filesystem::path repo_dir() const = 0;
};

class RealGitOps final : public GitOps {
 public:
  void open_or_init(const std::filesystem::path& repo_dir) override;
  void write_file(const std::filesystem::path& relative_path, const std::string& content) override;
  void stage_path(const std::filesystem::path& relative_path) override;
  void remove_path(const std::filesystem::path& relative_path) override;
  void commit(const std::string& message) override;
  void set_remote(const std::string& name, const std::string& url) override;
  void remove_remote(const std::string& name) override;
  void pull_remote_ff_only(const std::string& name) override;
  RemoteProbeResult probe_remote(const std::string& name) override;
  PushResult push_branch(const std::string& name, const std::string& branch, bool set_upstream)
      override;
  std::filesystem::path repo_dir() const override;

 private:
  GitRepo repo_;
};

} // namespace holder::git
