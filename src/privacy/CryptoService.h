#pragma once

#include "privacy/PrivacyError.h"

#include <array>
#include <string>

namespace holder::privacy {

constexpr std::size_t kPrivacyKeyBytes = 32;

struct EnvelopeParts {
  std::string key_id;
  std::string iv_b64;
  std::string ciphertext_b64;
};

std::array<unsigned char, kPrivacyKeyBytes> generate_random_key();

std::string key_to_base64(const std::array<unsigned char, kPrivacyKeyBytes>& key);
std::array<unsigned char, kPrivacyKeyBytes> key_from_base64(const std::string& b64);

std::string encrypt_envelope_v1(
    const std::string& plaintext,
    const std::array<unsigned char, kPrivacyKeyBytes>& key,
    const std::string& key_id
);
// iv here means the 24-byte unique per-message value for XChaCha20-Poly1305.

std::string decrypt_envelope_v1(
    const std::string& envelope,
    const std::array<unsigned char, kPrivacyKeyBytes>& key,
    const std::string& expected_key_id
);

} // namespace holder::privacy
