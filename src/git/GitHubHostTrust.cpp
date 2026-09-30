#include "git/GitHubHostTrust.h"
#if __has_include(<git2/sys/errors.h>)
#include <git2/sys/errors.h>
#else
// Older libgit2 releases expose git_error_set_str in the public error header.
#include <git2/errors.h>
#endif

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

namespace holder::git {

int check_github_host_certificate(git_cert* certificate, int, const char* host, void*) {
  if (certificate == nullptr || certificate->cert_type != GIT_CERT_HOSTKEY_LIBSSH2 ||
      host == nullptr) return GIT_PASSTHROUGH;

  const std::string_view hostname(host);
  constexpr std::string_view github = "github.com";
  const bool is_github = hostname.size() == github.size() &&
      std::equal(hostname.begin(), hostname.end(), github.begin(), [](unsigned char a, char b) {
        return std::tolower(a) == b;
      });
  if (!is_github) return GIT_PASSTHROUGH;

  const auto* key = reinterpret_cast<const git_cert_hostkey*>(certificate);
  // SHA-256 pins, verified 2026-09-30 against GitHub's official documentation:
  // https://docs.github.com/en/authentication/keeping-your-account-and-data-secure/githubs-ssh-key-fingerprints
  // If GitHub rotates a key, verify the replacement there before updating pins.
  constexpr std::array<std::string_view, 3> fingerprints = {
      "uNiVztksCsDhcc0u9e8BujQXVUpKZIDTMczCvj3tD2s", // RSA
      "p2QAMXNIC1TJYWeIOttrVc98/R1BUFWu3/LiyKgUfQM", // ECDSA
      "+DiY3wvvV6TuJJhbpZisF/zLDA0zPMSvHdkr4UvCOqU", // Ed25519
  };
  if ((key->type & GIT_CERT_SSH_SHA256) != 0) {
    std::array<char, sodium_base64_ENCODED_LEN(32, sodium_base64_VARIANT_ORIGINAL_NO_PADDING)> encoded{};
    sodium_bin2base64(encoded.data(), encoded.size(), key->hash_sha256, sizeof key->hash_sha256,
                     sodium_base64_VARIANT_ORIGINAL_NO_PADDING);
    if (std::find(fingerprints.begin(), fingerprints.end(), std::string_view(encoded.data())) !=
        fingerprints.end()) return 0;
  }
  git_error_set_str(GIT_ERROR_SSH,
      "GitHub SSH host key does not match its published fingerprints. "
      "Check GitHub's SSH key announcements and update Holder before retrying.");
  return GIT_ECERTIFICATE;
}

} // namespace holder::git
