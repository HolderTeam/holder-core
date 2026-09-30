#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "git/GitHubHostTrust.h"
#include "git/GitRepo.h"
#include "core_test_helpers.h"
#include <sodium.h>
#include <array>
#include <cstdlib>

TEST_CASE("GitHub SSH trust accepts only the published SHA256 fingerprints", "[git][ssh-trust]") {
  git_libgit2_init();
  git_cert_hostkey key{};
  key.parent.cert_type = GIT_CERT_HOSTKEY_LIBSSH2;
  key.type = GIT_CERT_SSH_SHA256;
  for (const auto* fingerprint : {
      "uNiVztksCsDhcc0u9e8BujQXVUpKZIDTMczCvj3tD2s",
      "p2QAMXNIC1TJYWeIOttrVc98/R1BUFWu3/LiyKgUfQM",
      "+DiY3wvvV6TuJJhbpZisF/zLDA0zPMSvHdkr4UvCOqU"}) {
    REQUIRE(sodium_base642bin(key.hash_sha256, sizeof key.hash_sha256, fingerprint, 43,
        nullptr, nullptr, nullptr, sodium_base64_VARIANT_ORIGINAL_NO_PADDING) == 0);
    REQUIRE(holder::git::check_github_host_certificate(&key.parent, 0, "github.com", nullptr) == 0);
    REQUIRE(holder::git::check_github_host_certificate(&key.parent, 0, "GitHub.COM", nullptr) == 0);
    key.hash_sha256[0] ^= 1;
    REQUIRE(holder::git::check_github_host_certificate(&key.parent, 0, "github.com", nullptr) == GIT_ECERTIFICATE);
    // A locally trusted key cannot override the published identity.
    REQUIRE(holder::git::check_github_host_certificate(&key.parent, 1, "github.com", nullptr) == GIT_ECERTIFICATE);
  }
  key.type = GIT_CERT_SSH_SHA1;
  REQUIRE(holder::git::check_github_host_certificate(&key.parent, 1, "github.com", nullptr) == GIT_ECERTIFICATE);
}

TEST_CASE("GitHub SSH trust preserves validation for other hosts and HTTPS", "[git][ssh-trust]") {
  git_cert_hostkey key{};
  key.parent.cert_type = GIT_CERT_HOSTKEY_LIBSSH2;
  key.type = GIT_CERT_SSH_SHA256;
  for (const auto* host : {"example.com", "github.com.example.com", "evilgithub.com", "github.com.", ""}) {
    REQUIRE(holder::git::check_github_host_certificate(&key.parent, 0, host, nullptr) == GIT_PASSTHROUGH);
    REQUIRE(holder::git::check_github_host_certificate(&key.parent, 1, host, nullptr) == GIT_PASSTHROUGH);
  }
  git_cert https{};
  https.cert_type = GIT_CERT_X509;
  REQUIRE(holder::git::check_github_host_certificate(&https, 0, "github.com", nullptr) == GIT_PASSTHROUGH);
  REQUIRE(holder::git::check_github_host_certificate(nullptr, 0, "github.com", nullptr) == GIT_PASSTHROUGH);
  REQUIRE(holder::git::check_github_host_certificate(&key.parent, 0, nullptr, nullptr) == GIT_PASSTHROUGH);
}

// Read-only live check. Credentials retain their normal HOME, while libgit2's
// host-trust directory is empty. No user known_hosts or remote refs are changed.
TEST_CASE("GitHub SSH connects with an empty trust directory", "[.github-ssh-smoke]") {
  const char* remote = std::getenv("HOLDER_TEST_GITHUB_SSH_REMOTE");
  if (remote == nullptr) SKIP("Set HOLDER_TEST_GITHUB_SSH_REMOTE for a read-only live check");
  git_libgit2_init();
  git_buf previous = GIT_BUF_INIT;
  REQUIRE(git_libgit2_opts(GIT_OPT_GET_HOMEDIR, &previous) == 0);
  const auto root = holder::test::make_temp_dir();
  struct Restore {
    git_buf& previous;
    std::filesystem::path root;
    ~Restore() {
      git_libgit2_opts(GIT_OPT_SET_HOMEDIR, previous.ptr);
      git_buf_dispose(&previous);
      std::filesystem::remove_all(root);
    }
  } restore{previous, root};
  REQUIRE(git_libgit2_opts(GIT_OPT_SET_HOMEDIR, root.string().c_str()) == 0);
  holder::git::GitRepo repo;
  const auto result = repo.probe_remote_url(remote);
  INFO(result.error_message);
  REQUIRE(result.status == holder::git::RemoteProbeStatus::Reachable);
  REQUIRE_FALSE(std::filesystem::exists(root / ".ssh/known_hosts"));
}
