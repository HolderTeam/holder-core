#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "privacy/PrivacyError.h"

#include <cstdint>
#include <limits>
#include <string>

TEST_CASE("PrivacyError stores code and message", "[privacy]") {
  holder::privacy::PrivacyError ex(
      holder::privacy::PrivacyErrorCode::KeyMaterialMissing,
      "missing key"
  );
  REQUIRE(ex.code() == holder::privacy::PrivacyErrorCode::KeyMaterialMissing);
  REQUIRE(std::string(ex.what()) == "missing key");
}

TEST_CASE("privacy_error_code_name maps all known codes", "[privacy]") {
  using holder::privacy::privacy_error_code_name;
  using holder::privacy::PrivacyErrorCode;

  REQUIRE(
      std::string(privacy_error_code_name(PrivacyErrorCode::KeyMaterialMissing)) ==
      "privacy_key_material_missing"
  );
  REQUIRE(
      std::string(privacy_error_code_name(PrivacyErrorCode::KeyringUnavailable)) ==
      "privacy_keyring_unavailable"
  );
  REQUIRE(
      std::string(privacy_error_code_name(PrivacyErrorCode::RecoveryTokenInvalid)) ==
      "privacy_recovery_token_invalid"
  );
  REQUIRE(
      std::string(privacy_error_code_name(PrivacyErrorCode::EnvelopeInvalid)) ==
      "privacy_envelope_invalid"
  );
  REQUIRE(
      std::string(privacy_error_code_name(PrivacyErrorCode::EnvelopeMetadataMismatch)) ==
      "privacy_envelope_metadata_mismatch"
  );
  REQUIRE(
      std::string(privacy_error_code_name(PrivacyErrorCode::CryptMetadataMissing)) ==
      "privacy_metadata_missing"
  );
  REQUIRE(
      std::string(privacy_error_code_name(PrivacyErrorCode::EncryptionSafetyCheckFailed)) ==
      "privacy_safety_check_failed"
  );
  REQUIRE(
      std::string(privacy_error_code_name(PrivacyErrorCode::PrivacyCryptoFailed)) ==
      "privacy_crypto_failed"
  );
}

TEST_CASE("privacy_error_code_name falls back for unknown values", "[privacy]") {
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  auto unknown = static_cast<holder::privacy::PrivacyErrorCode>(
      std::numeric_limits<std::uint8_t>::max()
  );
  REQUIRE(std::string(holder::privacy::privacy_error_code_name(unknown)) == "privacy_error");
}
