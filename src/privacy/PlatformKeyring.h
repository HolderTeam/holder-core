#pragma once

#include "privacy/PrivacyError.h"

#include <cstdint>
#include <optional>
#include <string>

namespace holder::privacy {

enum class PlatformKeyringSecretKind : std::uint8_t {
  GenericSecret,
  ProjectKey,
};

struct PlatformKeyringSecretRef {
  PlatformKeyringSecretKind kind;
  std::string service;
  std::string account;
  std::optional<std::string> project_id;
};

struct PlatformKeyringLookupResult {
  std::optional<std::string> secret;
  std::optional<std::string> error_message;
};

bool platform_keyring_supported();

PlatformKeyringLookupResult platform_keyring_lookup_secret(const PlatformKeyringSecretRef& ref);

void platform_keyring_store_secret(
    const PlatformKeyringSecretRef& ref,
    const std::string& label,
    const std::string& secret
);

void platform_keyring_remove_secret(const PlatformKeyringSecretRef& ref);

using PlatformKeyringLookupHook = PlatformKeyringLookupResult (*)(const PlatformKeyringSecretRef&);
using PlatformKeyringStoreHook =
    std::optional<std::string> (*)(const PlatformKeyringSecretRef&, const std::string&);
using PlatformKeyringRemoveHook = std::optional<std::string> (*)(const PlatformKeyringSecretRef&);

void platform_keyring_set_lookup_hook_for_tests(PlatformKeyringLookupHook hook);
void platform_keyring_set_store_hook_for_tests(PlatformKeyringStoreHook hook);
void platform_keyring_set_remove_hook_for_tests(PlatformKeyringRemoveHook hook);

} // namespace holder::privacy
