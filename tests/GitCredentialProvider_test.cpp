#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "core_test_helpers.h"
#include "git/EcdsaDerSigningCredentialProvider.h"
#include "git/GitRepo.h"
#include "git/SshAgentAndFileCredentialProvider.h"

#include <git2.h>
#include <git2/sys/credential.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>

#include <cstdlib>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

std::vector<unsigned char> der_signature_from_hex_rs(const char* r_hex, const char* s_hex) {
  BIGNUM* r = nullptr;
  BIGNUM* s = nullptr;
  BN_hex2bn(&r, r_hex);
  BN_hex2bn(&s, s_hex);

  ECDSA_SIG* sig = ECDSA_SIG_new();
  ECDSA_SIG_set0(sig, r, s); // sig takes ownership of r and s

  unsigned char* der = nullptr;
  const int der_len = i2d_ECDSA_SIG(sig, &der);
  std::vector<unsigned char> out(der, der + der_len);
  OPENSSL_free(der);
  ECDSA_SIG_free(sig);
  return out;
}

// Trivial P-256 SSH wire-format public key blob. Only used to exercise
// git_credential_ssh_custom_new's own validation, never actually signs
// anything in these tests.
std::vector<unsigned char> dummy_p256_ssh_pubkey_blob() {
  EVP_PKEY* key = EVP_EC_gen("P-256");

  std::vector<unsigned char> q(65);
  size_t q_len = 0;
  EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_PUB_KEY, q.data(), q.size(), &q_len);
  EVP_PKEY_free(key);

  auto append_ssh_string =
      [](std::vector<unsigned char>& out, const unsigned char* data, size_t len) {
        out.push_back(static_cast<unsigned char>((len >> 24) & 0xFF));
        out.push_back(static_cast<unsigned char>((len >> 16) & 0xFF));
        out.push_back(static_cast<unsigned char>((len >> 8) & 0xFF));
        out.push_back(static_cast<unsigned char>(len & 0xFF));
        out.insert(out.end(), data, data + len);
      };

  std::vector<unsigned char> blob;
  const std::string type = "ecdsa-sha2-nistp256";
  const std::string curve = "nistp256";
  append_ssh_string(blob, reinterpret_cast<const unsigned char*>(type.data()), type.size());
  append_ssh_string(blob, reinterpret_cast<const unsigned char*>(curve.data()), curve.size());
  append_ssh_string(blob, q.data(), q.size());
  return blob;
}

} // namespace

TEST_CASE("SSH credentials advance from agent to files and reset per operation", "[git]") {
  holder::git::GitRepo repo; // Initializes libgit2.
  const auto home = holder::test::make_temp_dir();
  std::filesystem::create_directories(home / ".ssh");
  std::ofstream(home / ".ssh/id_ed25519") << "test identity";
  std::ofstream(home / ".ssh/id_rsa") << "test identity";
  holder::test::EnvGuard home_guard("HOME", home.string());
#ifdef _WIN32
  holder::test::EnvGuard profile_guard("USERPROFILE", home.string());
  SECTION("HOME takes precedence") {
    holder::test::EnvGuard different_profile("USERPROFILE", (home / "unused").string());
    holder::git::SshAgentAndFileCredentialProvider provider;
    git_credential* raw = nullptr;
    REQUIRE(provider.acquire(&raw, "ssh://example.invalid/repo", "git", GIT_CREDENTIAL_SSH_KEY));
    git_credential_free(raw);
    REQUIRE(provider.acquire(&raw, "ssh://example.invalid/repo", "git", GIT_CREDENTIAL_SSH_KEY));
    std::unique_ptr<git_credential, decltype(&git_credential_free)> cred(raw, git_credential_free);
    REQUIRE(
        std::string(reinterpret_cast<git_credential_ssh_key*>(raw)->privatekey) ==
        home.string() + "/.ssh/id_ed25519"
    );
  }
  SECTION("USERPROFILE supplies home when HOME is unset") {
    holder::test::EnvUnsetGuard unset_home("HOME");
#else
  SECTION("HOME supplies home") {
#endif
    holder::git::SshAgentAndFileCredentialProvider provider;
    auto acquire = [&]() {
      git_credential* raw = nullptr;
      REQUIRE(
          provider.acquire(&raw, "ssh://example.invalid/repo", "alice", GIT_CREDENTIAL_SSH_KEY)
      );
      return std::unique_ptr<git_credential, decltype(&git_credential_free)>(
          raw,
          git_credential_free
      );
    };
    {
      auto cred = acquire();
      REQUIRE(reinterpret_cast<git_credential_ssh_key*>(cred.get())->privatekey == nullptr);
    }
    for (const auto* name : {"id_ed25519", "id_rsa"}) {
      auto cred = acquire();
      REQUIRE(std::string(git_credential_get_username(cred.get())) == "alice");
      REQUIRE(
          std::string(reinterpret_cast<git_credential_ssh_key*>(cred.get())->privatekey) ==
          home.string() + "/.ssh/" + name
      );
    }
    git_credential* raw = nullptr;
    REQUIRE_FALSE(
        provider.acquire(&raw, "ssh://example.invalid/repo", "alice", GIT_CREDENTIAL_SSH_KEY)
    );
    REQUIRE(raw == nullptr);
    provider.begin_operation();
    auto cred = acquire();
    REQUIRE(reinterpret_cast<git_credential_ssh_key*>(cred.get())->privatekey == nullptr);
  }
  std::filesystem::remove_all(home);
}

TEST_CASE("SSH file credentials skip missing keys and decline memory-only requests", "[git]") {
  holder::git::GitRepo repo;
  const auto home = holder::test::make_temp_dir();
  std::filesystem::create_directories(home / ".ssh");
  std::ofstream(home / ".ssh/id_rsa") << "test identity";
  holder::test::EnvGuard home_guard("HOME", home.string());
  holder::git::SshAgentAndFileCredentialProvider provider;
  git_credential* raw = nullptr;
  REQUIRE_FALSE(
      provider.acquire(&raw, "ssh://example.invalid/repo", "git", GIT_CREDENTIAL_SSH_MEMORY)
  );
  REQUIRE(provider.acquire(&raw, "ssh://example.invalid/repo", "git", GIT_CREDENTIAL_SSH_KEY));
  git_credential_free(raw);
  REQUIRE(provider.acquire(&raw, "ssh://example.invalid/repo", "git", GIT_CREDENTIAL_SSH_KEY));
  std::unique_ptr<git_credential, decltype(&git_credential_free)> cred(raw, git_credential_free);
  REQUIRE(
      std::string(reinterpret_cast<git_credential_ssh_key*>(raw)->privatekey) ==
      home.string() + "/.ssh/id_rsa"
  );
  std::filesystem::remove_all(home);
}

// Opt-in: the caller supplies a disposable SSH remote and isolated HOME with
// its trusted known_hosts entry. Never run against a production repository.
TEST_CASE("Git over SSH can push probe and fetch", "[.ssh-smoke]") {
  const char* url = std::getenv("HOLDER_TEST_SSH_REMOTE_URL");
  if (url == nullptr) SKIP("Set HOLDER_TEST_SSH_REMOTE_URL to a disposable SSH remote");
  REQUIRE((git_libgit2_features() & GIT_FEATURE_SSH) != 0);
  holder::test::EnvGuard branch_guard("GIT_DEFAULT_BRANCH", "ssh-smoke");
  const auto root = holder::test::make_temp_dir();
  {
    holder::git::GitRepo source;
    source.open_or_init(root / "source");
    source.write_file("ssh-smoke.txt", "Git over SSH\n");
    source.stage_path("ssh-smoke.txt");
    source.commit("SSH smoke test");
    source.set_remote("origin", url);
    const auto pushed = source.push_branch("origin", "ssh-smoke", true);
    INFO(pushed.error_message);
    REQUIRE(pushed.status == holder::git::PushStatus::Pushed);
    REQUIRE(source.probe_remote("origin").status == holder::git::RemoteProbeStatus::Reachable);
    holder::git::GitRepo destination;
    destination.open_or_init(root / "destination");
    destination.set_remote("origin", url);
    destination.pull_remote_ff_only("origin");
    std::ifstream fetched(root / "destination/ssh-smoke.txt");
    std::string contents;
    std::getline(fetched, contents);
    REQUIRE(contents == "Git over SSH");
  }
  std::filesystem::remove_all(root);
}

TEST_CASE(
    "EcdsaDerSigningCredentialProvider reshapes DER signature to SSH mpint wire format",
    "[git]"
) {
  using holder::git::EcdsaDerSigningCredentialProvider;

  SECTION("both r and s fit without padding") {
    const auto der = der_signature_from_hex_rs("0102030405", "0605040302");
    const auto wire = EcdsaDerSigningCredentialProvider::der_to_ssh_wire_signature_for_tests(der);

    const std::vector<unsigned char> expected = {
        0x00,
        0x00,
        0x00,
        0x05,
        0x01,
        0x02,
        0x03,
        0x04,
        0x05, // mpint(r), no padding needed
        0x00,
        0x00,
        0x00,
        0x05,
        0x06,
        0x05,
        0x04,
        0x03,
        0x02, // mpint(s), no padding needed
    };
    REQUIRE(wire == expected);
  }

  SECTION("high bit set requires a leading zero pad byte") {
    // r's top byte (0x80) has the high bit set -- SSH mpint must prepend 0x00
    // to keep it unambiguously positive.
    const auto der = der_signature_from_hex_rs("80010203", "01");
    const auto wire = EcdsaDerSigningCredentialProvider::der_to_ssh_wire_signature_for_tests(der);

    const std::vector<unsigned char> expected = {
        0x00,
        0x00,
        0x00,
        0x05,
        0x00,
        0x80,
        0x01,
        0x02,
        0x03, // mpint(r), padded
        0x00,
        0x00,
        0x00,
        0x01,
        0x01, // mpint(s), no padding needed
    };
    REQUIRE(wire == expected);
  }

  SECTION("malformed DER yields an empty result") {
    const std::vector<unsigned char> not_der = {0xDE, 0xAD, 0xBE, 0xEF};
    const auto wire = EcdsaDerSigningCredentialProvider::der_to_ssh_wire_signature_for_tests(
        not_der
    );
    REQUIRE(wire.empty());
  }
}

TEST_CASE(
    "EcdsaDerSigningCredentialProvider::sign_trampoline bridges sign_raw_ to libssh2's callback contract",
    "[git]"
) {
  // sign_trampoline never dereferences its LIBSSH2_SESSION* (the type is opaque -- see the
  // header's forward declaration), so it can be called directly without a real libssh2 session.
  using holder::git::EcdsaDerSigningCredentialProvider;

  const unsigned char data[] = {0xAA};

  SECTION("converts a valid signature to SSH wire format and mallocs the output") {
    EcdsaDerSigningCredentialProvider provider("git", {0x01}, [](const unsigned char*, size_t) {
      return der_signature_from_hex_rs("0102030405", "0605040302");
    });
    void* abstract = &provider;
    unsigned char* sig = nullptr;
    size_t sig_len = 0;
    const int rc = EcdsaDerSigningCredentialProvider::sign_trampoline_for_tests(
        &sig,
        &sig_len,
        data,
        sizeof(data),
        &abstract
    );
    REQUIRE(rc == 0);
    REQUIRE(sig != nullptr);
    REQUIRE(sig_len > 0);
    std::free(sig);
  }

  SECTION("sign_raw_ throwing is reported as a failure, not propagated") {
    EcdsaDerSigningCredentialProvider provider(
        "git",
        {0x01},
        [](const unsigned char*, size_t) -> std::vector<unsigned char> {
          throw std::runtime_error("signer exploded");
        }
    );
    void* abstract = &provider;
    unsigned char* sig = nullptr;
    size_t sig_len = 0;
    const int rc = EcdsaDerSigningCredentialProvider::sign_trampoline_for_tests(
        &sig,
        &sig_len,
        data,
        sizeof(data),
        &abstract
    );
    REQUIRE(rc == -1);
  }

  SECTION("sign_raw_ returning an empty signature is reported as a failure") {
    EcdsaDerSigningCredentialProvider provider(
        "git",
        {0x01},
        [](const unsigned char*, size_t) -> std::vector<unsigned char> {
          return {};
        }
    );
    void* abstract = &provider;
    unsigned char* sig = nullptr;
    size_t sig_len = 0;
    const int rc = EcdsaDerSigningCredentialProvider::sign_trampoline_for_tests(
        &sig,
        &sig_len,
        data,
        sizeof(data),
        &abstract
    );
    REQUIRE(rc == -1);
  }

  SECTION("sign_raw_ returning malformed DER is reported as a failure") {
    EcdsaDerSigningCredentialProvider provider(
        "git",
        {0x01},
        [](const unsigned char*, size_t) -> std::vector<unsigned char> {
          return {0xDE, 0xAD, 0xBE, 0xEF};
        }
    );
    void* abstract = &provider;
    unsigned char* sig = nullptr;
    size_t sig_len = 0;
    const int rc = EcdsaDerSigningCredentialProvider::sign_trampoline_for_tests(
        &sig,
        &sig_len,
        data,
        sizeof(data),
        &abstract
    );
    REQUIRE(rc == -1);
  }
}

TEST_CASE("EcdsaDerSigningCredentialProvider only handles GIT_CREDENTIAL_SSH_CUSTOM", "[git]") {
  // git_credential_ssh_custom_new (reached via acquire()) needs libgit2 initialized. Other test
  // files get this for free by constructing a GitRepo first (its constructor calls this), but
  // when Catch2/CTest runs this test case on its own -- as ctest does, one process per test --
  // nothing else has done that yet.
  git_libgit2_init();
  using holder::git::EcdsaDerSigningCredentialProvider;

  bool sign_called = false;
  EcdsaDerSigningCredentialProvider provider(
      "git",
      dummy_p256_ssh_pubkey_blob(),
      [&](const unsigned char*, size_t) -> std::vector<unsigned char> {
        sign_called = true;
        return {};
      }
  );

  SECTION("declines when SSH_CUSTOM is not offered") {
    git_credential* cred = nullptr;
    const bool produced =
        provider.acquire(&cred, "ssh://example.invalid/repo.git", "git", GIT_CREDENTIAL_SSH_KEY);
    REQUIRE_FALSE(produced);
    REQUIRE(cred == nullptr);
    REQUIRE_FALSE(sign_called);
  }

  SECTION("produces a credential when SSH_CUSTOM is offered") {
    git_credential* cred = nullptr;
    const bool produced =
        provider.acquire(&cred, "ssh://example.invalid/repo.git", "git", GIT_CREDENTIAL_SSH_CUSTOM);
    REQUIRE(produced);
    REQUIRE(cred != nullptr);
    REQUIRE_FALSE(
        sign_called
    ); // acquire() only builds the credential; signing happens later, during auth.
    git_credential_free(cred);
  }
}

TEST_CASE(
    "EcdsaDerSigningCredentialProvider prefers the URL's username over its default",
    "[git]"
) {
  // See the identical comment in the "only handles GIT_CREDENTIAL_SSH_CUSTOM" test above.
  git_libgit2_init();
  using holder::git::EcdsaDerSigningCredentialProvider;

  EcdsaDerSigningCredentialProvider provider(
      "git",
      dummy_p256_ssh_pubkey_blob(),
      [](const unsigned char*, size_t) -> std::vector<unsigned char> {
        return {};
      }
  );

  SECTION("URL supplies a username") {
    git_credential* cred = nullptr;
    REQUIRE(provider.acquire(
        &cred,
        "ssh://zeth@example.invalid/repo.git",
        "zeth",
        GIT_CREDENTIAL_SSH_CUSTOM
    ));
    REQUIRE(std::string(git_credential_get_username(cred)) == "zeth");
    git_credential_free(cred);
  }

  SECTION("URL has no username: falls back to the provider's default") {
    git_credential* cred = nullptr;
    REQUIRE(
        provider.acquire(&cred, "ssh://example.invalid/repo.git", "", GIT_CREDENTIAL_SSH_CUSTOM)
    );
    REQUIRE(std::string(git_credential_get_username(cred)) == "git");
    git_credential_free(cred);
  }

  SECTION("URL username is null: falls back to the provider's default") {
    git_credential* cred = nullptr;
    REQUIRE(
        provider
            .acquire(&cred, "ssh://example.invalid/repo.git", nullptr, GIT_CREDENTIAL_SSH_CUSTOM)
    );
    REQUIRE(std::string(git_credential_get_username(cred)) == "git");
    git_credential_free(cred);
  }
}

TEST_CASE(
    "GitRepo defaults to an SshAgentAndFileCredentialProvider and honors set_credential_provider",
    "[git]"
) {
  holder::git::GitRepo repo;
  REQUIRE(repo.credential_provider_for_tests() != nullptr);

  auto custom = std::make_shared<holder::git::SshAgentAndFileCredentialProvider>();
  auto* raw = custom.get();
  repo.set_credential_provider(custom);
  REQUIRE(repo.credential_provider_for_tests() == raw);
}
