#include "git/EcdsaDerSigningCredentialProvider.h"

#include <git2.h>
#include <openssl/bn.h>
#include <openssl/ecdsa.h>

#include <cstdlib>
#include <cstring>
#include <utility>

namespace holder::git {
namespace {

void append_u32(std::vector<unsigned char>& out, uint32_t value) {
  out.push_back(static_cast<unsigned char>((value >> 24) & 0xFF));
  out.push_back(static_cast<unsigned char>((value >> 16) & 0xFF));
  out.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
  out.push_back(static_cast<unsigned char>(value & 0xFF));
}

void append_ssh_string(std::vector<unsigned char>& out, const unsigned char* data, size_t len) {
  append_u32(out, static_cast<uint32_t>(len));
  out.insert(out.end(), data, data + len);
}

// SSH mpint (RFC 4251 5): minimal big-endian, with a leading 0x00 if the high bit would
// otherwise be set (to keep it unambiguously positive).
void append_mpint_from_bn(std::vector<unsigned char>& out, const BIGNUM* bn) {
  const int n = BN_num_bytes(bn);
  std::vector<unsigned char> raw(static_cast<size_t>(n));
  BN_bn2bin(bn, raw.data());
  if (n > 0 && (raw[0] & 0x80) != 0) {
    std::vector<unsigned char> padded;
    padded.push_back(0x00);
    padded.insert(padded.end(), raw.begin(), raw.end());
    append_ssh_string(out, padded.data(), padded.size());
  } else {
    append_ssh_string(out, raw.data(), raw.size());
  }
}

// Reshapes a DER ECDSA-Sig-Value{r,s} into the SSH wire format libssh2 expects
// for its sign_callback return value: mpint(r) || mpint(s). Returns an empty
// vector if der is not a well-formed ECDSA signature.
std::vector<unsigned char> der_to_ssh_wire_signature(const std::vector<unsigned char>& der) {
  const unsigned char* der_ptr = der.data();
  ECDSA_SIG* sig = d2i_ECDSA_SIG(nullptr, &der_ptr, static_cast<long>(der.size()));
  if (sig == nullptr) return {};

  const BIGNUM* r = nullptr;
  const BIGNUM* s = nullptr;
  ECDSA_SIG_get0(sig, &r, &s);

  std::vector<unsigned char> wire;
  append_mpint_from_bn(wire, r);
  append_mpint_from_bn(wire, s);
  ECDSA_SIG_free(sig);
  return wire;
}

} // namespace

// libssh2-facing sign callback. payload (via *abstract) is the
// EcdsaDerSigningCredentialProvider instance passed to git_credential_ssh_custom_new.
int EcdsaDerSigningCredentialProvider::sign_trampoline(
    LIBSSH2_SESSION* /*session*/,
    unsigned char** sig,
    size_t* sig_len,
    const unsigned char* data,
    size_t data_len,
    void** abstract
) {
  auto* provider = static_cast<EcdsaDerSigningCredentialProvider*>(*abstract);

  std::vector<unsigned char> der;
  try {
    der = provider->sign_raw_(data, data_len);
  } catch (...) {
    return -1;
  }
  if (der.empty()) return -1;

  const auto wire = der_to_ssh_wire_signature(der);
  if (wire.empty()) return -1;

  // libssh2 frees this buffer with LIBSSH2_FREE, which is plain free() unless a
  // custom allocator is configured -- match that with malloc, not new[].
  *sig = static_cast<unsigned char*>(malloc(wire.size()));
  if (*sig == nullptr) return -1;
  memcpy(*sig, wire.data(), wire.size());
  *sig_len = wire.size();
  return 0;
}

EcdsaDerSigningCredentialProvider::EcdsaDerSigningCredentialProvider(
    std::string default_username,
    std::vector<unsigned char> public_key_blob,
    EcdsaDerSignFn sign_raw
)
    : default_username_(std::move(default_username)),
      public_key_blob_(std::move(public_key_blob)),
      sign_raw_(std::move(sign_raw)) {}

std::vector<unsigned char> EcdsaDerSigningCredentialProvider::der_to_ssh_wire_signature_for_tests(
    const std::vector<unsigned char>& der
) {
  return der_to_ssh_wire_signature(der);
}

bool EcdsaDerSigningCredentialProvider::acquire(
    git_credential** out,
    const char* /*url*/,
    const char* username_from_url,
    unsigned int allowed_types
) {
  if ((allowed_types & GIT_CREDENTIAL_SSH_CUSTOM) == 0U) return false;

  // libgit2 commits to a username earlier in the handshake than this
  // callback runs; offering anything else here fails with "username does
  // not match previous request", so prefer the URL's username when it has
  // one (matching SshAgentAndFileCredentialProvider's behavior) and only
  // fall back to default_username_ when the URL didn't specify one.
  const char* user = (username_from_url != nullptr && username_from_url[0] != '\0')
                          ? username_from_url
                          : default_username_.c_str();

  const int rc = git_credential_ssh_custom_new(
      out,
      user,
      reinterpret_cast<const char*>(public_key_blob_.data()),
      public_key_blob_.size(),
      sign_trampoline,
      this
  );
  return rc == 0;
}

} // namespace holder::git
