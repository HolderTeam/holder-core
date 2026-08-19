#pragma once

#include "git/GitCredentialProvider.h"

namespace holder::git {

// Default credential provider: tries the running ssh-agent first, then falls
// back to ~/.ssh/id_ed25519 and ~/.ssh/id_rsa. This is desktop's existing
// (pre-provider-seam) behavior, preserved verbatim as the default so GitRepo
// keeps working unchanged unless a caller explicitly installs a different
// provider via GitRepo::set_credential_provider.
class SshAgentAndFileCredentialProvider final : public GitCredentialProvider {
 public:
  bool acquire(
      git_credential** out,
      const char* url,
      const char* username_from_url,
      unsigned int allowed_types
  ) override;
};

} // namespace holder::git
