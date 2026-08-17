#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace holder::privacy {

enum class PrivacyErrorCode : std::uint8_t {
  KeyMaterialMissing,
  KeyringUnavailable,
  RecoveryTokenInvalid,
  EnvelopeInvalid,
  EnvelopeMetadataMismatch,
  CryptMetadataMissing,
  EncryptionSafetyCheckFailed,
  PrivacyCryptoFailed,
};

class PrivacyError : public std::runtime_error {
 public:
  PrivacyError(PrivacyErrorCode code, const std::string& message)
      : std::runtime_error(message),
        code_(code) {}

  PrivacyErrorCode code() const noexcept { return code_; }

 private:
  PrivacyErrorCode code_;
};

inline const char* privacy_error_code_name(PrivacyErrorCode code) {
  switch (code) {
  case PrivacyErrorCode::KeyMaterialMissing:
    return "privacy_key_material_missing";
  case PrivacyErrorCode::KeyringUnavailable:
    return "privacy_keyring_unavailable";
  case PrivacyErrorCode::RecoveryTokenInvalid:
    return "privacy_recovery_token_invalid";
  case PrivacyErrorCode::EnvelopeInvalid:
    return "privacy_envelope_invalid";
  case PrivacyErrorCode::EnvelopeMetadataMismatch:
    return "privacy_envelope_metadata_mismatch";
  case PrivacyErrorCode::CryptMetadataMissing:
    return "privacy_metadata_missing";
  case PrivacyErrorCode::EncryptionSafetyCheckFailed:
    return "privacy_safety_check_failed";
  case PrivacyErrorCode::PrivacyCryptoFailed:
    return "privacy_crypto_failed";
  }
  return "privacy_error";
}

} // namespace holder::privacy
