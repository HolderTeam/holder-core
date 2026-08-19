#pragma once

#include "git/GitCredentialProvider.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

typedef struct _LIBSSH2_SESSION LIBSSH2_SESSION;

namespace holder::git {

// A signing function for an ecdsa-sha2-nistp256 (P-256/secp256r1) SSH key
// whose private key never leaves its owner (e.g. a platform keystore). Given
// the challenge bytes libssh2 wants signed, it must return a DER-encoded
// ECDSA-Sig-Value{r,s} -- the ordinary output of e.g. Android Keystore's
// Signature.getInstance("SHA256withECDSA") or OpenSSL's ECDSA_do_sign +
// i2d_ECDSA_SIG. An empty vector, or a thrown exception, is treated as a
// signing failure for that auth attempt.
using EcdsaDerSignFn = std::function<std::vector<unsigned char>(const unsigned char* data, size_t data_len)>;

// Credential provider for an SSH identity backed by a non-exportable ECDSA
// P-256 private key (Android Keystore, a hardware token, etc). Bridges
// libssh2's raw sign challenge to sign_raw, then reshapes the DER signature
// it returns into the SSH wire format libssh2 expects for
// ecdsa-sha2-nistp256 (RFC 5656 3.1.2: mpint(r) || mpint(s); libssh2 itself
// adds the outer string("ecdsa-sha2-nistp256") envelope).
//
// This class is specifically about ECDSA + DER: an Ed25519 hardware key
// would need a sibling class, not a variant of this one, since Ed25519 SSH
// signatures are the raw 64 bytes with no DER encoding and no mpint
// reshaping at all.
class EcdsaDerSigningCredentialProvider final : public GitCredentialProvider {
 public:
  // public_key_blob is the raw SSH wire-format ecdsa-sha2-nistp256 public
  // key blob (RFC 5656 3.1) -- NOT base64-encoded, NOT the
  // "ecdsa-sha2-nistp256 AAAA... comment" line.
  //
  // default_username is used only when a remote URL doesn't embed one of its
  // own (e.g. "ssh://host/repo.git" with no "user@"); acquire() otherwise
  // uses the URL's username, matching SshAgentAndFileCredentialProvider's
  // behavior. libgit2 commits to a username during an earlier stage of the
  // SSH handshake than this callback -- offering anything else here fails
  // with "username does not match previous request", so this provider must
  // not just always assume e.g. "git" regardless of what a caller's URL
  // actually says.
  EcdsaDerSigningCredentialProvider(
      std::string default_username,
      std::vector<unsigned char> public_key_blob,
      EcdsaDerSignFn sign_raw
  );

  bool acquire(
      git_credential** out,
      const char* url,
      const char* username_from_url,
      unsigned int allowed_types
  ) override;

  // Exposed for unit tests: reshapes a DER ECDSA-Sig-Value{r,s} into the SSH
  // wire format (mpint(r) || mpint(s)). Returns an empty vector if der is not
  // a well-formed ECDSA signature.
  static std::vector<unsigned char> der_to_ssh_wire_signature_for_tests(
      const std::vector<unsigned char>& der
  );

 private:
  static int sign_trampoline(
      LIBSSH2_SESSION* session,
      unsigned char** sig,
      size_t* sig_len,
      const unsigned char* data,
      size_t data_len,
      void** abstract
  );

  std::string default_username_;
  std::vector<unsigned char> public_key_blob_;
  EcdsaDerSignFn sign_raw_;
};

} // namespace holder::git
