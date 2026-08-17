#include "git/GitOps.h"

namespace holder::git {

void RealGitOps::open_or_init(const std::filesystem::path& repo_dir) {
  repo_.open_or_init(repo_dir);
}

void RealGitOps::write_file(
    const std::filesystem::path& relative_path,
    const std::string& content
) {
  repo_.write_file(relative_path, content);
}

void RealGitOps::stage_path(const std::filesystem::path& relative_path) {
  repo_.stage_path(relative_path);
}

void RealGitOps::remove_path(const std::filesystem::path& relative_path) {
  repo_.remove_path(relative_path);
}

void RealGitOps::commit(const std::string& message) { repo_.commit(message); }

void RealGitOps::set_remote(const std::string& name, const std::string& url) {
  repo_.set_remote(name, url);
}

void RealGitOps::remove_remote(const std::string& name) { repo_.remove_remote(name); }

void RealGitOps::pull_remote_ff_only(const std::string& name) { repo_.pull_remote_ff_only(name); }

RemoteProbeResult RealGitOps::probe_remote(const std::string& name) {
  return repo_.probe_remote(name);
}

PushResult RealGitOps::push_branch(
    const std::string& name,
    const std::string& branch,
    bool set_upstream
) {
  return repo_.push_branch(name, branch, set_upstream);
}

std::filesystem::path RealGitOps::repo_dir() const { return repo_.repo_dir(); }

} // namespace holder::git
