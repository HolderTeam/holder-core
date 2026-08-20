#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "git/EcdsaDerSigningCredentialProvider.h"
#include "git/GitRepo.h"
#include "git/SshAgentAndFileCredentialProvider.h"

#include <git2.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>

#include <memory>
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

  auto append_ssh_string = [](std::vector<unsigned char>& out, const unsigned char* data, size_t len) {
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

TEST_CASE("EcdsaDerSigningCredentialProvider reshapes DER signature to SSH mpint wire format", "[git]") {
  using holder::git::EcdsaDerSigningCredentialProvider;

  SECTION("both r and s fit without padding") {
    const auto der = der_signature_from_hex_rs("0102030405", "0605040302");
    const auto wire = EcdsaDerSigningCredentialProvider::der_to_ssh_wire_signature_for_tests(der);

    const std::vector<unsigned char> expected = {
        0x00, 0x00, 0x00, 0x05, 0x01, 0x02, 0x03, 0x04, 0x05, // mpint(r), no padding needed
        0x00, 0x00, 0x00, 0x05, 0x06, 0x05, 0x04, 0x03, 0x02, // mpint(s), no padding needed
    };
    REQUIRE(wire == expected);
  }

  SECTION("high bit set requires a leading zero pad byte") {
    // r's top byte (0x80) has the high bit set -- SSH mpint must prepend 0x00
    // to keep it unambiguously positive.
    const auto der = der_signature_from_hex_rs("80010203", "01");
    const auto wire = EcdsaDerSigningCredentialProvider::der_to_ssh_wire_signature_for_tests(der);

    const std::vector<unsigned char> expected = {
        0x00, 0x00, 0x00, 0x05, 0x00, 0x80, 0x01, 0x02, 0x03, // mpint(r), padded
        0x00, 0x00, 0x00, 0x01, 0x01, // mpint(s), no padding needed
    };
    REQUIRE(wire == expected);
  }

  SECTION("malformed DER yields an empty result") {
    const std::vector<unsigned char> not_der = {0xDE, 0xAD, 0xBE, 0xEF};
    const auto wire = EcdsaDerSigningCredentialProvider::der_to_ssh_wire_signature_for_tests(not_der);
    REQUIRE(wire.empty());
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
    const bool produced = provider.acquire(&cred, "ssh://example.invalid/repo.git", "git", GIT_CREDENTIAL_SSH_KEY);
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
    REQUIRE_FALSE(sign_called); // acquire() only builds the credential; signing happens later, during auth.
    git_credential_free(cred);
  }
}

TEST_CASE("EcdsaDerSigningCredentialProvider prefers the URL's username over its default", "[git]") {
  // See the identical comment in the "only handles GIT_CREDENTIAL_SSH_CUSTOM" test above.
  git_libgit2_init();
  using holder::git::EcdsaDerSigningCredentialProvider;

  EcdsaDerSigningCredentialProvider provider(
      "git",
      dummy_p256_ssh_pubkey_blob(),
      [](const unsigned char*, size_t) -> std::vector<unsigned char> { return {}; }
  );

  SECTION("URL supplies a username") {
    git_credential* cred = nullptr;
    REQUIRE(provider.acquire(&cred, "ssh://zeth@example.invalid/repo.git", "zeth", GIT_CREDENTIAL_SSH_CUSTOM));
    REQUIRE(std::string(git_credential_get_username(cred)) == "zeth");
    git_credential_free(cred);
  }

  SECTION("URL has no username: falls back to the provider's default") {
    git_credential* cred = nullptr;
    REQUIRE(provider.acquire(&cred, "ssh://example.invalid/repo.git", "", GIT_CREDENTIAL_SSH_CUSTOM));
    REQUIRE(std::string(git_credential_get_username(cred)) == "git");
    git_credential_free(cred);
  }

  SECTION("URL username is null: falls back to the provider's default") {
    git_credential* cred = nullptr;
    REQUIRE(provider.acquire(&cred, "ssh://example.invalid/repo.git", nullptr, GIT_CREDENTIAL_SSH_CUSTOM));
    REQUIRE(std::string(git_credential_get_username(cred)) == "git");
    git_credential_free(cred);
  }
}

TEST_CASE("GitRepo defaults to an SshAgentAndFileCredentialProvider and honors set_credential_provider", "[git]") {
  holder::git::GitRepo repo;
  REQUIRE(repo.credential_provider_for_tests() != nullptr);

  auto custom = std::make_shared<holder::git::SshAgentAndFileCredentialProvider>();
  auto* raw = custom.get();
  repo.set_credential_provider(custom);
  REQUIRE(repo.credential_provider_for_tests() == raw);
}
