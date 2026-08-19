#pragma once

typedef struct git_credential git_credential;

namespace holder::git {

// Pluggable strategy for producing libgit2 credentials during a git network
// operation (probe/push/pull). GitRepo defaults to an
// SshAgentAndFileCredentialProvider (see SshAgentAndFileCredentialProvider.h),
// matching desktop's existing ssh-agent/~/.ssh behavior exactly. Platforms
// without a filesystem-based SSH identity (e.g. Android) can instead install
// a provider backed by a platform keystore -- see
// EcdsaDerSigningCredentialProvider.h for the concrete signing-callback case.
class GitCredentialProvider {
 public:
  virtual ~GitCredentialProvider() = default; // LCOV_EXCL_LINE

  // Mirrors libgit2's git_credential_acquire_cb contract, minus the payload
  // parameter (each provider instance is its own payload). Returns true and
  // sets *out if a credential was produced for this request; returns false
  // (leaving *out untouched) to fall through to GIT_PASSTHROUGH, e.g. because
  // allowed_types doesn't include a type this provider handles.
  virtual bool acquire(
      git_credential** out,
      const char* url,
      const char* username_from_url,
      unsigned int allowed_types
  ) = 0;
};

} // namespace holder::git
