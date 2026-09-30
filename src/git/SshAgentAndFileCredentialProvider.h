#pragma once

#include "git/GitCredentialProvider.h"

namespace holder::git {

// Default credential provider: tries the running ssh-agent first, then falls
// back to ~/.ssh/id_ed25519 and ~/.ssh/id_rsa. This is desktop's existing
// behavior. Authentication retries advance to the next identity, rather than
// repeatedly offering the agent. Windows uses USERPROFILE when HOME is unset.
class SshAgentAndFileCredentialProvider final : public GitCredentialProvider {
 public:
  void begin_operation() override { next_identity_ = 0; }
  bool acquire(
      git_credential** out,
      const char* url,
      const char* username_from_url,
      unsigned int allowed_types
  ) override;

 private:
  unsigned int next_identity_ = 0;
};

} // namespace holder::git
