#include "git/SshAgentAndFileCredentialProvider.h"

#include <git2.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>

namespace holder::git {
namespace {

std::string home_dir_or_empty() {
  const char* home = std::getenv("HOME");
#ifdef _WIN32
  if (home == nullptr || home[0] == '\0') home = std::getenv("USERPROFILE");
#endif
  return home ? std::string(home) : std::string();
}

} // namespace

bool SshAgentAndFileCredentialProvider::acquire(
    git_credential** out,
    const char* /*url*/,
    const char* username_from_url,
    unsigned int allowed_types
) {
  if ((allowed_types & GIT_CREDENTIAL_SSH_KEY) == 0U) {
    return false;
  }

  const char* user = username_from_url && username_from_url[0] != '\0' ? username_from_url : "git";
  // Creation succeeds even when no agent is running. libgit2 calls us again
  // after authentication fails; advance instead of returning the agent forever.
  if (next_identity_ == 0) {
    ++next_identity_;
    if (git_credential_ssh_key_from_agent(out, user) == 0) return true;
  }

  const auto home = home_dir_or_empty();
  if (!home.empty() && (allowed_types & GIT_CREDENTIAL_SSH_KEY) != 0U) {
    const std::array<std::pair<std::string, std::string>, 2> keypairs = {
        std::make_pair(home + "/.ssh/id_ed25519.pub", home + "/.ssh/id_ed25519"),
        std::make_pair(home + "/.ssh/id_rsa.pub", home + "/.ssh/id_rsa"),
    };
    while (next_identity_ <= keypairs.size()) {
      const auto& keypair = keypairs[next_identity_++ - 1];
      std::error_code ec;
      if (!std::filesystem::is_regular_file(keypair.second, ec)) continue;
      const int rc =
          git_credential_ssh_key_new(out, user, keypair.first.c_str(), keypair.second.c_str(), "");
      if (rc == 0) return true;
    }
  }

  return false;
}

} // namespace holder::git
