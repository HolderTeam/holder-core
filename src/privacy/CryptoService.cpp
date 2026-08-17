#include "privacy/CryptoService.h"

#include <nlohmann/json.hpp>
#include <sodium.h>

#include <array>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace holder::privacy {
namespace {

constexpr const char* kEnvelopeMagic = "HolderPriv1";
constexpr unsigned char kEnvelopeVersion = 1;
constexpr const char* kCipherName = "xchacha20poly1305";

void ensure_sodium_ready() {
  if (sodium_init() < 0) {
    throw holder::privacy::PrivacyError(
        holder::privacy::PrivacyErrorCode::PrivacyCryptoFailed,
        "failed to initialize libsodium"
    ); // LCOV_EXCL_LINE
  }
}

std::string b64_encode(const unsigned char* data, std::size_t len) {
  std::string out(sodium_base64_encoded_len(len, sodium_base64_VARIANT_ORIGINAL), '\0');
  sodium_bin2base64(out.data(), out.size(), data, len, sodium_base64_VARIANT_ORIGINAL);
  while (!out.empty() && out.back() == '\0') {
    out.pop_back();
  }
  return out;
} // LCOV_EXCL_LINE

std::vector<unsigned char> b64_decode(const std::string& text) {
  std::vector<unsigned char> out(text.size(), 0);
  std::size_t out_len = 0;
  if (sodium_base642bin(
          out.data(),
          out.size(),
          text.c_str(),
          text.size(),
          nullptr,
          &out_len,
          nullptr,
          sodium_base64_VARIANT_ORIGINAL
      ) != 0) {
    throw holder::privacy::PrivacyError(
        holder::privacy::PrivacyErrorCode::EnvelopeInvalid,
        "invalid base64 in privacy envelope"
    );
  }
  out.resize(out_len);
  return out;
}

EnvelopeParts parse_envelope(const std::string& envelope) {
  std::istringstream in(envelope);
  std::string magic;
  std::string meta_line;
  std::string cipher_line;

  if (!std::getline(in, magic) || magic != kEnvelopeMagic) {
    throw holder::privacy::PrivacyError(
        holder::privacy::PrivacyErrorCode::EnvelopeInvalid,
        "invalid privacy envelope magic"
    );
  }
  if (!std::getline(in, meta_line)) {
    throw holder::privacy::PrivacyError(
        holder::privacy::PrivacyErrorCode::EnvelopeInvalid,
        "privacy envelope metadata missing"
    );
  }
  if (!std::getline(in, cipher_line)) {
    throw holder::privacy::PrivacyError(
        holder::privacy::PrivacyErrorCode::EnvelopeInvalid,
        "privacy envelope ciphertext missing"
    );
  }

  const auto meta = nlohmann::json::parse(meta_line);
  if (!meta.is_object()) {
    throw holder::privacy::PrivacyError(
        holder::privacy::PrivacyErrorCode::EnvelopeInvalid,
        "privacy envelope metadata is not an object"
    );
  }
  if (!meta.contains("version") ||
      meta.at("version").get<int>() != static_cast<int>(kEnvelopeVersion)) {
    throw holder::privacy::PrivacyError(
        holder::privacy::PrivacyErrorCode::EnvelopeInvalid,
        "unsupported privacy envelope version"
    );
  }
  if (!meta.contains("cipher") || meta.at("cipher").get<std::string>() != kCipherName) {
    throw holder::privacy::PrivacyError(
        holder::privacy::PrivacyErrorCode::EnvelopeInvalid,
        "unsupported privacy envelope cipher"
    );
  }
  if (!meta.contains("key_id") || !meta.contains("iv_b64")) {
    throw holder::privacy::PrivacyError(
        holder::privacy::PrivacyErrorCode::EnvelopeInvalid,
        "privacy envelope missing key_id or iv_b64"
    );
  }

  EnvelopeParts parts;
  parts.key_id = meta.at("key_id").get<std::string>();
  parts.iv_b64 = meta.at("iv_b64").get<std::string>();
  parts.ciphertext_b64 = cipher_line;
  return parts;
}

} // namespace

std::array<unsigned char, kPrivacyKeyBytes> generate_random_key() {
  ensure_sodium_ready();
  std::array<unsigned char, kPrivacyKeyBytes> key{};
  randombytes_buf(key.data(), key.size());
  return key;
}

std::string key_to_base64(const std::array<unsigned char, kPrivacyKeyBytes>& key) {
  ensure_sodium_ready();
  return b64_encode(key.data(), key.size());
}

std::array<unsigned char, kPrivacyKeyBytes> key_from_base64(const std::string& b64) {
  ensure_sodium_ready();
  const auto raw = b64_decode(b64);
  if (raw.size() != kPrivacyKeyBytes) {
    throw holder::privacy::PrivacyError(
        holder::privacy::PrivacyErrorCode::EnvelopeInvalid,
        "privacy key has wrong length"
    );
  }
  std::array<unsigned char, kPrivacyKeyBytes> key{};
  std::memcpy(key.data(), raw.data(), key.size());
  return key;
}

std::string encrypt_envelope_v1(
    const std::string& plaintext,
    const std::array<unsigned char, kPrivacyKeyBytes>& key,
    const std::string& key_id
) {
  ensure_sodium_ready();
  if (key_id.empty()) {
    throw holder::privacy::PrivacyError(
        holder::privacy::PrivacyErrorCode::EnvelopeInvalid,
        "privacy key_id must not be empty"
    );
  }

  std::array<unsigned char, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES> iv{};
  randombytes_buf(iv.data(), iv.size());

  std::vector<unsigned char> ciphertext(
      plaintext.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES
  );
  unsigned long long ciphertext_len = 0;
  if (crypto_aead_xchacha20poly1305_ietf_encrypt(
          ciphertext.data(),
          &ciphertext_len,
          reinterpret_cast<const unsigned char*>(plaintext.data()),
          static_cast<unsigned long long>(plaintext.size()),
          nullptr,
          0,
          nullptr,
          iv.data(),
          key.data()
      ) != 0) {
    throw holder::privacy::PrivacyError(
        holder::privacy::PrivacyErrorCode::PrivacyCryptoFailed,
        "privacy encryption failed"
    ); // LCOV_EXCL_LINE
  }
  ciphertext.resize(static_cast<std::size_t>(ciphertext_len));

  nlohmann::json meta = {
      {"version", kEnvelopeVersion},
      {"cipher", kCipherName},
      {"key_id", key_id},
      {"iv_b64", b64_encode(iv.data(), iv.size())},
  };

  std::ostringstream out;
  out << kEnvelopeMagic << '\n';
  out << meta.dump() << '\n';
  out << b64_encode(ciphertext.data(), ciphertext.size()) << '\n';
  return out.str();
}

std::string decrypt_envelope_v1(
    const std::string& envelope,
    const std::array<unsigned char, kPrivacyKeyBytes>& key,
    const std::string& expected_key_id
) {
  ensure_sodium_ready();
  const EnvelopeParts parts = parse_envelope(envelope);
  if (!expected_key_id.empty() && parts.key_id != expected_key_id) {
    throw holder::privacy::PrivacyError(
        holder::privacy::PrivacyErrorCode::EnvelopeMetadataMismatch,
        "privacy envelope key_id mismatch"
    );
  }

  const auto iv_raw = b64_decode(parts.iv_b64);
  if (iv_raw.size() != crypto_aead_xchacha20poly1305_ietf_NPUBBYTES) {
    throw holder::privacy::PrivacyError(
        holder::privacy::PrivacyErrorCode::EnvelopeInvalid,
        "privacy envelope iv length is invalid"
    );
  }

  const auto ciphertext = b64_decode(parts.ciphertext_b64);
  if (ciphertext.size() < crypto_aead_xchacha20poly1305_ietf_ABYTES) {
    throw holder::privacy::PrivacyError(
        holder::privacy::PrivacyErrorCode::EnvelopeInvalid,
        "privacy envelope ciphertext is too short"
    );
  }

  std::vector<unsigned char> plaintext(ciphertext.size(), 0);
  unsigned long long plaintext_len = 0;
  if (crypto_aead_xchacha20poly1305_ietf_decrypt(
          plaintext.data(),
          &plaintext_len,
          nullptr,
          ciphertext.data(),
          static_cast<unsigned long long>(ciphertext.size()),
          nullptr,
          0,
          reinterpret_cast<const unsigned char*>(iv_raw.data()),
          key.data()
      ) != 0) {
    throw holder::privacy::PrivacyError(
        holder::privacy::PrivacyErrorCode::EnvelopeInvalid,
        "privacy envelope authentication failed"
    );
  }
  plaintext.resize(static_cast<std::size_t>(plaintext_len));
  return std::string(reinterpret_cast<const char*>(plaintext.data()), plaintext.size());
}

} // namespace holder::privacy
