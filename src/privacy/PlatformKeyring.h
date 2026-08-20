#pragma once

#include "privacy/PrivacyError.h"

#include <cstdint>
#include <functional>
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

// Runtime-pluggable keyring backend for platforms with no compiled-in branch of
// their own (currently: Android, bridged to Keystore-backed EncryptedSharedPreferences
// via JNI in holder-android's native layer -- see holder_keyring_set_provider in
// holder.h, the C ABI entry point that ultimately calls this). Checked after the
// _for_tests hooks (which exist specifically to let a test fully override behavior)
// but before any compiled #if HOLDER_HAVE_* branch, and makes
// platform_keyring_supported() report true while installed.
//
// store/remove return an error message on failure, or std::nullopt on success --
// matching the _for_tests hooks' contract -- since the common case (nothing went
// wrong) needs no allocation.
using PlatformKeyringExternalLookupFn =
    std::function<PlatformKeyringLookupResult(const PlatformKeyringSecretRef&)>;
using PlatformKeyringExternalStoreFn = std::function<
    std::optional<std::string>(const PlatformKeyringSecretRef&, const std::string& label, const std::string& secret)>;
using PlatformKeyringExternalRemoveFn =
    std::function<std::optional<std::string>(const PlatformKeyringSecretRef&)>;

void platform_keyring_set_external_provider(
    PlatformKeyringExternalLookupFn lookup,
    PlatformKeyringExternalStoreFn store,
    PlatformKeyringExternalRemoveFn remove
);
void platform_keyring_clear_external_provider();

using PlatformKeyringLookupHook = PlatformKeyringLookupResult (*)(const PlatformKeyringSecretRef&);
using PlatformKeyringStoreHook =
    std::optional<std::string> (*)(const PlatformKeyringSecretRef&, const std::string&);
using PlatformKeyringRemoveHook = std::optional<std::string> (*)(const PlatformKeyringSecretRef&);

void platform_keyring_set_lookup_hook_for_tests(PlatformKeyringLookupHook hook);
void platform_keyring_set_store_hook_for_tests(PlatformKeyringStoreHook hook);
void platform_keyring_set_remove_hook_for_tests(PlatformKeyringRemoveHook hook);

} // namespace holder::privacy
