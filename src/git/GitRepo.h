#pragma once
#include "git/PushResult.h"
#include "git/RemoteProbe.h"

#include <filesystem>
#include <string>

namespace holder::git {

class GitRepo {
 public:
  GitRepo();
  ~GitRepo();

  GitRepo(const GitRepo&) = delete;
  GitRepo& operator=(const GitRepo&) = delete;

  // Open existing or init a new repo at repo_dir (non-bare, has working tree).
  void open_or_init(const std::filesystem::path& repo_dir);

  // Write a file under the repo working tree (creates directories).
  void write_file(const std::filesystem::path& relative_path, const std::string& content);

  // Stage (add) a path (relative to repo root).
  void stage_path(const std::filesystem::path& relative_path);
  // Stage a deletion (relative to repo root).
  void remove_path(const std::filesystem::path& relative_path);

  // Create a commit from current index (tree).
  // If it's the first commit, parent list is empty.
  void commit(const std::string& message);

  // Ensure a remote exists with the given URL (create or update).
  void set_remote(const std::string& name, const std::string& url);

  // Remove a remote if it exists.
  void remove_remote(const std::string& name);

  // Pull from remote into current branch (fast-forward only).
  void pull_remote_ff_only(const std::string& name);
  // Probe remote reachability and whether it has a default HEAD.
  RemoteProbeResult probe_remote(const std::string& name);
  // Push local branch to remote.
  PushResult push_branch(const std::string& name, const std::string& branch, bool set_upstream);

  // Test hook: invoke the internal credential callback and report whether a credential was
  // produced.
  static int credential_callback_for_tests(
      unsigned int allowed_types,
      const char* username_from_url,
      bool* out_credential_created
  );
  static RemoteProbeStatus classify_remote_probe_error_for_tests(const std::string& message);
  static PushStatus classify_push_error_for_tests(const std::string& message);
  static std::string configured_default_branch_name_for_tests();

  std::filesystem::path repo_dir() const { return repo_dir_; }

 private:
  void ensure_open() const;

  // Create author/committer signature.
  // Uses git config if present, otherwise falls back to placeholders.
  void make_signature(void** out_sig) const; // out_sig is git_signature*

  void* repo_ = nullptr; // git_repository*
  std::filesystem::path repo_dir_;
};

} // namespace holder::git
