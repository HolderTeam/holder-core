#pragma once

#include "privacy/PrivacyError.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace holder::privacy {

struct SecretMetadata {
  std::string service;
  std::string account;
  std::string preview;
  long long created_at = 0;
  long long updated_at = 0;
};

struct StoredSecret {
  SecretMetadata metadata;
  std::string secret;
};

class SecretStore {
 public:
  virtual ~SecretStore() = default; // LCOV_EXCL_LINE

  virtual std::optional<StoredSecret> get(const std::string& service, const std::string& account)
      const = 0;
  virtual std::vector<SecretMetadata> list(const std::string& service) const = 0;
  virtual void set(
      const std::string& service,
      const std::string& account,
      const std::string& secret,
      const std::string& preview,
      long long created_at,
      long long updated_at
  ) = 0;
  virtual void remove(const std::string& service, const std::string& account) = 0;
};

std::unique_ptr<SecretStore> make_default_secret_store(const std::filesystem::path& server_dir);
std::unique_ptr<SecretStore> make_encrypted_file_secret_store_for_tests(
    const std::filesystem::path& server_dir
);

#if HOLDER_HAVE_LIBSECRET
struct LibsecretLookupResult {
  std::optional<std::string> secret;
  std::optional<std::string> error_message;
};

using LibsecretLookupHook =
    LibsecretLookupResult (*)(const std::string& service, const std::string& account);

void secret_store_set_libsecret_lookup_hook_for_tests(LibsecretLookupHook hook);

struct LibsecretApiLookupResult {
  std::optional<std::string> secret;
  std::optional<std::string> error_message;
};

using LibsecretApiLookupHook =
    LibsecretApiLookupResult (*)(const std::string& service, const std::string& account);

void secret_store_set_libsecret_api_lookup_hook_for_tests(LibsecretApiLookupHook hook);
#endif

} // namespace holder::privacy
