#pragma once
#include "git/GitCredentialProvider.h"
#include "git/PushResult.h"
#include "git/RemoteProbe.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace holder::git {

// Thrown by pull_remote_ff_only when local and remote have diverged (neither is an ancestor of
// the other). Carries both OIDs (hex) so a caller that wants to resolve the divergence -- rather
// than just surface the failure -- doesn't have to re-derive them via its own branch/ref lookup.
class NonFastForwardPullError : public std::runtime_error {
 public:
  NonFastForwardPullError(std::string local_oid, std::string remote_oid, std::string remote)
      : std::runtime_error("Non-fast-forward pull is not supported"),
        local_oid_hex(std::move(local_oid)),
        remote_oid_hex(std::move(remote_oid)),
        remote_name(std::move(remote)) {}

  std::string local_oid_hex;
  std::string remote_oid_hex;
  std::string remote_name;
};

class GitRepo {
 public:
  GitRepo();
  ~GitRepo();

  GitRepo(const GitRepo&) = delete;
  GitRepo& operator=(const GitRepo&) = delete;

  // Installs the credential provider used for subsequent network operations
  // (probe/push/pull). Defaults to an SshAgentAndFileCredentialProvider, so
  // this only needs to be called to install a different one (e.g. a
  // platform-keystore-backed signer on Android).
  void set_credential_provider(std::shared_ptr<GitCredentialProvider> provider);

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

  // Pull from remote into current branch (fast-forward only). Throws NonFastForwardPullError
  // (not a plain std::runtime_error) if local and remote have diverged.
  void pull_remote_ff_only(const std::string& name);
  // Probe remote reachability and whether it has a default HEAD.
  RemoteProbeResult probe_remote(const std::string& name);
  // Push local branch to remote.
  PushResult push_branch(const std::string& name, const std::string& branch, bool set_upstream);

  struct DivergedMergeResult {
    // Relative paths changed on both sides since the merge-base -- the file-level "conflict"
    // set. Remote's version wins for these in the resulting commit; a caller that wants to
    // preserve the pre-merge local version (e.g. as a duplicate card) can read it via
    // read_blob_at(local_oid_hex, path), using the local_oid_hex from the
    // NonFastForwardPullError this followed.
    std::vector<std::string> conflicted_paths;
  };

  // Resolves a diverged pull at the file level: remote's version wins for any path touched on
  // both sides since the merge-base; paths touched only locally are preserved; paths touched
  // only remotely are taken as-is. Deliberately never attempts a line-level content merge for a
  // path touched on both sides, even if the touched lines don't overlap -- see conflicted_paths.
  // Creates a real two-parent merge commit (parents: local_oid_hex, remote_oid_hex) and updates
  // HEAD to it.
  DivergedMergeResult merge_remote_taking_theirs_for_conflicts(
      const std::string& name,
      const std::string& local_oid_hex,
      const std::string& remote_oid_hex
  );

  // Reads a file's content at a specific historical commit, without touching the working tree,
  // index, or HEAD. Returns nullopt if the path doesn't exist at that commit.
  std::optional<std::string> read_blob_at(
      const std::string& commit_oid_hex,
      const std::filesystem::path& relative_path
  );

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
  GitCredentialProvider* credential_provider_for_tests() const { return credential_provider_.get(); }

  std::filesystem::path repo_dir() const { return repo_dir_; }

 private:
  void ensure_open() const;

  // Create author/committer signature.
  // Uses git config if present, otherwise falls back to placeholders.
  void make_signature(void** out_sig) const; // out_sig is git_signature*

  void* repo_ = nullptr; // git_repository*
  std::filesystem::path repo_dir_;
  std::shared_ptr<GitCredentialProvider> credential_provider_;
};

} // namespace holder::git
