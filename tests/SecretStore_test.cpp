#include "ai/AiProviderCredentialRecovery.h"
#include "ai/AiProviderCredentialRepo.h"
#include "core_test_helpers.h"
#include "platform/Db.h"
#include "privacy/PlatformKeyring.h"
#include "privacy/SecretStore.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace {

using holder::test::EnvUnsetGuard;

#if HOLDER_HAVE_LIBSECRET || HOLDER_HAVE_MACOS_KEYCHAIN
class PlatformKeyringStoreHookGuard {
 public:
  explicit PlatformKeyringStoreHookGuard(holder::privacy::PlatformKeyringStoreHook hook) {
    holder::privacy::platform_keyring_set_store_hook_for_tests(hook);
  }

  ~PlatformKeyringStoreHookGuard() {
    holder::privacy::platform_keyring_set_store_hook_for_tests(nullptr);
  }
};

class PlatformKeyringRemoveHookGuard {
 public:
  explicit PlatformKeyringRemoveHookGuard(holder::privacy::PlatformKeyringRemoveHook hook) {
    holder::privacy::platform_keyring_set_remove_hook_for_tests(hook);
  }

  ~PlatformKeyringRemoveHookGuard() {
    holder::privacy::platform_keyring_set_remove_hook_for_tests(nullptr);
  }
};

std::optional<holder::privacy::PlatformKeyringSecretRef> recorded_store_ref;
std::optional<holder::privacy::PlatformKeyringSecretRef> recorded_remove_ref;
std::string recorded_store_secret;

std::optional<std::string> record_platform_store(
    const holder::privacy::PlatformKeyringSecretRef& ref,
    const std::string& secret
) {
  recorded_store_ref = ref;
  recorded_store_secret = secret;
  return std::nullopt;
}

std::optional<std::string> record_platform_remove(
    const holder::privacy::PlatformKeyringSecretRef& ref
) {
  recorded_remove_ref = ref;
  return std::nullopt;
}

std::optional<std::string> fail_platform_remove(const holder::privacy::PlatformKeyringSecretRef&) {
  return std::string("forced remove failure");
}
#endif

} // namespace

TEST_CASE("SecretStore set/get/list/remove persists through index-backed test store", "[privacy]") {
  const auto dir = holder::test::make_temp_dir();
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());

  auto store = holder::privacy::make_default_secret_store(dir / "server");
  store->set(
      "holder.ai_provider_credentials",
      "chocolatefactory",
      "cf_test_secret",
      "cf_****cret",
      100,
      120
  );

  const auto item = store->get("holder.ai_provider_credentials", "chocolatefactory");
  REQUIRE(item.has_value());
  REQUIRE(item->secret == "cf_test_secret");
  REQUIRE(item->metadata.preview == "cf_****cret");
  REQUIRE(item->metadata.created_at == 100);
  REQUIRE(item->metadata.updated_at == 120);

  const auto listed = store->list("holder.ai_provider_credentials");
  REQUIRE(listed.size() == 1);
  REQUIRE(listed[0].account == "chocolatefactory");
  REQUIRE(listed[0].preview == "cf_****cret");

  auto store_again = holder::privacy::make_default_secret_store(dir / "server");
  const auto item_again = store_again->get("holder.ai_provider_credentials", "chocolatefactory");
  REQUIRE(item_again.has_value());
  REQUIRE(item_again->secret == "cf_test_secret");

  store_again->remove("holder.ai_provider_credentials", "chocolatefactory");
  REQUIRE_FALSE(store_again->get("holder.ai_provider_credentials", "chocolatefactory").has_value());
  REQUIRE(store_again->list("holder.ai_provider_credentials").empty());
}

TEST_CASE(
    "AI provider credential metadata rebuilds from SecretStore after sqlite deletion",
    "[startup][recovery]"
) {
  const auto dir = holder::test::make_temp_dir();
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());

  auto store = holder::privacy::make_default_secret_store(dir / "server");
  store->set(
      "holder.ai_provider_credentials",
      "switchyard",
      "sw_secret_123",
      "sw_****123",
      200,
      240
  );

  const auto db_path = dir / "holder.db";
  holder::platform::Db db;
  db.open(db_path);
  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);

  holder::ai::recover_ai_provider_credentials_from_secret_store(db, *store);

  holder::ai::AiProviderCredentialRepo repo(db);
  const auto row = repo.get("switchyard");
  REQUIRE(row.has_value());
  REQUIRE(row->api_key_preview == "sw_****123");
  REQUIRE(row->created_at == 200);
  REQUIRE(row->updated_at == 240);
}

TEST_CASE(
    "AI provider credential recovery reconciles missing entries individually",
    "[startup][recovery]"
) {
  const auto dir = holder::test::make_temp_dir();
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());

  auto store = holder::privacy::make_default_secret_store(dir / "server");
  store->set(
      "holder.ai_provider_credentials",
      "switchyard",
      "sw_secret_123",
      "sw_****123",
      200,
      240
  );

  const auto db_path = dir / "holder.db";
  holder::platform::Db db;
  db.open(db_path);
  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);

  holder::ai::AiProviderCredentialRepo repo(db);
  repo.upsert("already-present", "ap_****000", 10, 20);

  holder::ai::recover_ai_provider_credentials_from_secret_store(db, *store);

  const auto existing = repo.get("already-present");
  REQUIRE(existing.has_value());
  REQUIRE(existing->api_key_preview == "ap_****000");
  REQUIRE(existing->created_at == 10);
  REQUIRE(existing->updated_at == 20);

  const auto recovered = repo.get("switchyard");
  REQUIRE(recovered.has_value());
  REQUIRE(recovered->api_key_preview == "sw_****123");
}

TEST_CASE("SecretStore handles sanitized filenames and tolerant metadata edge cases", "[privacy]") {
  const auto dir = holder::test::make_temp_dir();
  const auto keystore_dir = dir / "keystore";
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", keystore_dir.string());

  auto store = holder::privacy::make_default_secret_store(dir / "server");

  const std::string service = "holder.ai/provider creds";
  const std::string account = "acct:with spaces/and?chars";
  store->set(service, account, "secret-1", "se_****_1", 10, 20);

  const auto item = store->get(service, account);
  REQUIRE(item.has_value());
  REQUIRE(item->secret == "secret-1");
  REQUIRE(item->metadata.preview == "se_****_1");

  const auto sanitized_secret_path = keystore_dir /
                                     "holder.ai_provider_creds__acct_with_spaces_and_chars.secret";
  REQUIRE(std::filesystem::exists(sanitized_secret_path));

  std::filesystem::remove(sanitized_secret_path);
  REQUIRE_FALSE(store->get(service, account).has_value());

  const auto metadata_path = keystore_dir / "secret_store_index.json";
  {
    std::ofstream out(metadata_path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open());
  }
  REQUIRE(store->list(service).empty());

  {
    std::ofstream out(metadata_path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open());
    out << nlohmann::json{
        {"bogus", "not-an-object"},
        {"holder.ai/provider creds\nacct:with spaces/and?chars",
         {{"service", service},
          {"account", account},
          {"preview", "se_****_1"},
          {"created_at", 10},
          {"updated_at", 20}}},
    }
               .dump(2);
  }

  const auto listed = store->list(service);
  REQUIRE(listed.size() == 1);
  REQUIRE(listed[0].service == service);
  REQUIRE(listed[0].account == account);
}

TEST_CASE(
    "SecretStore encrypted fallback backend persists through encrypted file store",
    "[privacy]"
) {
  const auto dir = holder::test::make_temp_dir();
  EnvUnsetGuard unset_test_keystore("HOLDER_TEST_KEYSTORE_DIR");

  auto store = holder::privacy::make_encrypted_file_secret_store_for_tests(dir / "server");
  store->set(
      "holder.ai_provider_credentials",
      "fallback-provider",
      "fallback-secret-123",
      "fb_****_123",
      300,
      360
  );

  const auto item = store->get("holder.ai_provider_credentials", "fallback-provider");
  REQUIRE(item.has_value());
  REQUIRE(item->secret == "fallback-secret-123");
  REQUIRE(item->metadata.preview == "fb_****_123");

  const auto store_path = dir / "server" / "secret_store_fallback.enc";
  const auto key_path = dir / "server" / "secret_store_master.key";
  REQUIRE(std::filesystem::exists(store_path));
  REQUIRE(std::filesystem::exists(key_path));

  auto store_again = holder::privacy::make_encrypted_file_secret_store_for_tests(dir / "server");
  const auto item_again = store_again->get("holder.ai_provider_credentials", "fallback-provider");
  REQUIRE(item_again.has_value());
  REQUIRE(item_again->secret == "fallback-secret-123");

  const auto listed = store_again->list("holder.ai_provider_credentials");
  REQUIRE(listed.size() == 1);
  REQUIRE(listed[0].account == "fallback-provider");

  store_again->remove("holder.ai_provider_credentials", "fallback-provider");
  REQUIRE_FALSE(store_again->get("holder.ai_provider_credentials", "fallback-provider").has_value()
  );

  store_again->set(
      "holder.ai_provider_credentials",
      "fallback-metadata-only",
      "fallback-secret-456",
      "fb_****_456",
      400,
      460
  );
  std::filesystem::remove(dir / "server" / "secret_store_fallback.enc");
  REQUIRE_FALSE(
      store_again->get("holder.ai_provider_credentials", "fallback-metadata-only").has_value()
  );
}

#if HOLDER_HAVE_LIBSECRET || HOLDER_HAVE_MACOS_KEYCHAIN
TEST_CASE("SecretStore default platform backend stores and removes through keyring", "[privacy]") {
  const auto dir = holder::test::make_temp_dir();
  EnvUnsetGuard unset_test_keystore("HOLDER_TEST_KEYSTORE_DIR");
  recorded_store_ref.reset();
  recorded_remove_ref.reset();
  recorded_store_secret.clear();
  PlatformKeyringStoreHookGuard store_hook(&record_platform_store);
  PlatformKeyringRemoveHookGuard remove_hook(&record_platform_remove);

  auto store = holder::privacy::make_default_secret_store(dir / "server");
  store
      ->set("holder.ai_provider_credentials", "platform-provider", "secret-value", "sec****", 1, 2);

  REQUIRE(recorded_store_ref.has_value());
  REQUIRE(recorded_store_ref->kind == holder::privacy::PlatformKeyringSecretKind::GenericSecret);
  REQUIRE(recorded_store_ref->service == "holder.ai_provider_credentials");
  REQUIRE(recorded_store_ref->account == "platform-provider");
  REQUIRE_FALSE(recorded_store_ref->project_id.has_value());
  REQUIRE(recorded_store_secret == "secret-value");
  REQUIRE(store->list("holder.ai_provider_credentials").size() == 1);

  store->remove("holder.ai_provider_credentials", "platform-provider");
  REQUIRE(recorded_remove_ref.has_value());
  REQUIRE(recorded_remove_ref->kind == holder::privacy::PlatformKeyringSecretKind::GenericSecret);
  REQUIRE(recorded_remove_ref->service == "holder.ai_provider_credentials");
  REQUIRE(recorded_remove_ref->account == "platform-provider");
  REQUIRE(store->list("holder.ai_provider_credentials").empty());
}

TEST_CASE("SecretStore platform backend remove maps keyring failure", "[privacy]") {
  const auto dir = holder::test::make_temp_dir();
  EnvUnsetGuard unset_test_keystore("HOLDER_TEST_KEYSTORE_DIR");
  PlatformKeyringRemoveHookGuard remove_hook(&fail_platform_remove);

  auto store = holder::privacy::make_default_secret_store(dir / "server");
  try {
    store->remove("holder.ai_provider_credentials", "platform-provider");
    FAIL("Expected platform remove failure");
  } catch (const holder::privacy::PrivacyError& ex) {
    REQUIRE(ex.code() == holder::privacy::PrivacyErrorCode::KeyringUnavailable);
  }
}

TEST_CASE("PlatformKeyring project key lookup reports missing project id", "[privacy]") {
  const auto result = holder::privacy::platform_keyring_lookup_secret(
      holder::privacy::PlatformKeyringSecretRef{
          .kind = holder::privacy::PlatformKeyringSecretKind::ProjectKey,
          .service = "org.holder.ProjectKey",
          .account = "key-without-project",
          .project_id = std::nullopt,
      }
  );

  REQUIRE_FALSE(result.secret.has_value());
  REQUIRE(result.error_message.has_value());
}
#endif

namespace {

// A fake external keyring provider (what Android's JNI bridge to
// EncryptedSharedPreferences plays the role of in production), backed by a
// plain in-memory map so these tests exercise platform_keyring_set_external_provider
// itself rather than any real OS keyring.
class FakeExternalKeyring {
 public:
  ~FakeExternalKeyring() { holder::privacy::platform_keyring_clear_external_provider(); }

  void install() {
    holder::privacy::platform_keyring_set_external_provider(
        [this](const holder::privacy::PlatformKeyringSecretRef& ref) {
          return lookup(ref);
        },
        [this](
            const holder::privacy::PlatformKeyringSecretRef& ref,
            const std::string& label,
            const std::string& secret
        ) { return store(ref, label, secret); },
        [this](const holder::privacy::PlatformKeyringSecretRef& ref) { return remove(ref); }
    );
  }

  bool fail_next_store = false;

 private:
  static std::string key_for(const holder::privacy::PlatformKeyringSecretRef& ref) {
    return ref.kind == holder::privacy::PlatformKeyringSecretKind::ProjectKey
               ? "project:" + ref.project_id.value_or("") + ":" + ref.account
               : "generic:" + ref.service + ":" + ref.account;
  }

  holder::privacy::PlatformKeyringLookupResult lookup(
      const holder::privacy::PlatformKeyringSecretRef& ref
  ) {
    const auto it = values_.find(key_for(ref));
    if (it == values_.end()) return {.secret = std::nullopt, .error_message = std::nullopt};
    return {.secret = it->second, .error_message = std::nullopt};
  }

  std::optional<std::string> store(
      const holder::privacy::PlatformKeyringSecretRef& ref,
      const std::string& /*label*/,
      const std::string& secret
  ) {
    if (fail_next_store) {
      fail_next_store = false;
      return std::string("forced external store failure");
    }
    values_[key_for(ref)] = secret;
    return std::nullopt;
  }

  std::optional<std::string> remove(const holder::privacy::PlatformKeyringSecretRef& ref) {
    values_.erase(key_for(ref));
    return std::nullopt;
  }

  std::map<std::string, std::string> values_;
};

} // namespace

TEST_CASE("PlatformKeyring external provider is used for lookup/store/remove", "[privacy]") {
  FakeExternalKeyring fake;
  // Not asserting platform_keyring_supported() is false before install(): on a build
  // with a compiled platform branch (e.g. this dev machine's libsecret), it's already
  // true. What's guaranteed regardless of platform is that it becomes true once an
  // external provider is installed -- the property Android (no compiled branch at all)
  // actually depends on.
  fake.install();
  REQUIRE(holder::privacy::platform_keyring_supported());

  const holder::privacy::PlatformKeyringSecretRef ref{
      .kind = holder::privacy::PlatformKeyringSecretKind::GenericSecret,
      .service = "holder.test",
      .account = "acct",
      .project_id = std::nullopt,
  };

  REQUIRE_FALSE(holder::privacy::platform_keyring_lookup_secret(ref).secret.has_value());

  holder::privacy::platform_keyring_store_secret(ref, "label", "secret-value");
  const auto looked_up = holder::privacy::platform_keyring_lookup_secret(ref);
  REQUIRE(looked_up.secret == "secret-value");

  holder::privacy::platform_keyring_remove_secret(ref);
  REQUIRE_FALSE(holder::privacy::platform_keyring_lookup_secret(ref).secret.has_value());
}

TEST_CASE("PlatformKeyring external provider store failure maps to PrivacyError", "[privacy]") {
  FakeExternalKeyring fake;
  fake.install();
  fake.fail_next_store = true;

  const holder::privacy::PlatformKeyringSecretRef ref{
      .kind = holder::privacy::PlatformKeyringSecretKind::GenericSecret,
      .service = "holder.test",
      .account = "acct",
      .project_id = std::nullopt,
  };

  try {
    holder::privacy::platform_keyring_store_secret(ref, "label", "secret-value");
    FAIL("Expected external provider store failure");
  } catch (const holder::privacy::PrivacyError& ex) {
    REQUIRE(ex.code() == holder::privacy::PrivacyErrorCode::KeyringUnavailable);
  }
}

TEST_CASE("PlatformKeyring _for_tests hooks still take priority over an external provider", "[privacy]") {
  FakeExternalKeyring fake;
  fake.install();

  holder::privacy::platform_keyring_set_store_hook_for_tests(
      +[](const holder::privacy::PlatformKeyringSecretRef&, const std::string&) -> std::optional<std::string> {
        return std::nullopt;
      }
  );

  const holder::privacy::PlatformKeyringSecretRef ref{
      .kind = holder::privacy::PlatformKeyringSecretKind::GenericSecret,
      .service = "holder.test",
      .account = "acct",
      .project_id = std::nullopt,
  };
  holder::privacy::platform_keyring_store_secret(ref, "label", "secret-value");

  // The _for_tests hook, not the external provider, handled the store above --
  // the fake never recorded it, so a lookup through the fake finds nothing.
  REQUIRE_FALSE(holder::privacy::platform_keyring_lookup_secret(ref).secret.has_value());

  holder::privacy::platform_keyring_set_store_hook_for_tests(nullptr);
}

TEST_CASE("SecretStore selects the platform backend once an external keyring provider is installed", "[privacy]") {
  const auto dir = holder::test::make_temp_dir();
  EnvUnsetGuard unset_test_keystore("HOLDER_TEST_KEYSTORE_DIR");
  FakeExternalKeyring fake;
  fake.install();

  auto store = holder::privacy::make_default_secret_store(dir / "server");
  store->set("holder.ai_provider_credentials", "acct", "secret-value", "sec****", 1, 2);

  const auto fetched = store->get("holder.ai_provider_credentials", "acct");
  REQUIRE(fetched.has_value());
  REQUIRE(fetched->secret == "secret-value");

  store->remove("holder.ai_provider_credentials", "acct");
  REQUIRE_FALSE(store->get("holder.ai_provider_credentials", "acct").has_value());
}

#if HOLDER_HAVE_LIBSECRET
namespace {

class LibsecretLookupHookGuard {
 public:
  explicit LibsecretLookupHookGuard(holder::privacy::LibsecretLookupHook hook) {
    holder::privacy::secret_store_set_libsecret_lookup_hook_for_tests(hook);
  }

  ~LibsecretLookupHookGuard() {
    holder::privacy::secret_store_set_libsecret_lookup_hook_for_tests(nullptr);
  }
};

class LibsecretApiLookupHookGuard {
 public:
  explicit LibsecretApiLookupHookGuard(holder::privacy::LibsecretApiLookupHook hook) {
    holder::privacy::secret_store_set_libsecret_api_lookup_hook_for_tests(hook);
  }

  ~LibsecretApiLookupHookGuard() {
    holder::privacy::secret_store_set_libsecret_api_lookup_hook_for_tests(nullptr);
  }
};

holder::privacy::LibsecretLookupResult lookup_error(const std::string&, const std::string&) {
  return {.secret = std::nullopt, .error_message = std::string("lookup failed for test")};
}

holder::privacy::LibsecretLookupResult lookup_missing(const std::string&, const std::string&) {
  return {.secret = std::nullopt, .error_message = std::nullopt};
}

holder::privacy::LibsecretLookupResult lookup_success(const std::string&, const std::string&) {
  return {.secret = std::string("libsecret-test-secret"), .error_message = std::nullopt};
}

holder::privacy::LibsecretApiLookupResult api_lookup_error(const std::string&, const std::string&) {
  return {.secret = std::nullopt, .error_message = std::string("default lookup failed for test")};
}

holder::privacy::LibsecretApiLookupResult
api_lookup_missing(const std::string&, const std::string&) {
  return {.secret = std::nullopt, .error_message = std::nullopt};
}

holder::privacy::LibsecretApiLookupResult
api_lookup_success(const std::string&, const std::string&) {
  return {.secret = std::string("default-libsecret-secret"), .error_message = std::nullopt};
}

void seed_secret_store_index(
    const std::filesystem::path& server_dir,
    const std::string& service,
    const std::string& account
) {
  std::filesystem::create_directories(server_dir);
  std::ofstream out(server_dir / "secret_store_index.json", std::ios::binary | std::ios::trunc);
  REQUIRE(out.is_open());
  out << nlohmann::json{
      {service + "\n" + account,
       {{"service", service},
        {"account", account},
        {"preview", "ls_****ing"},
        {"created_at", 1},
        {"updated_at", 1}}},
  }
             .dump(2);
}

} // namespace

TEST_CASE("SecretStore get maps libsecret lookup failure to KeyMaterialMissing", "[privacy]") {
  const auto dir = holder::test::make_temp_dir();
  EnvUnsetGuard unset_test_keystore("HOLDER_TEST_KEYSTORE_DIR");
  const auto server_dir = dir / "server";
  LibsecretLookupHookGuard hook_guard(&lookup_error);
  seed_secret_store_index(server_dir, "holder.ai_provider_credentials", "libsecret-direct-missing");
  auto store = holder::privacy::make_default_secret_store(server_dir);

  try {
    (void)store->get("holder.ai_provider_credentials", "libsecret-direct-missing");
    FAIL("Expected libsecret lookup failure");
  } catch (const holder::privacy::PrivacyError& ex) {
    REQUIRE(ex.code() == holder::privacy::PrivacyErrorCode::KeyMaterialMissing);
  }
}

TEST_CASE(
    "SecretStore get returns nullopt when libsecret lookup misses existing metadata",
    "[privacy]"
) {
  const auto dir = holder::test::make_temp_dir();
  EnvUnsetGuard unset_test_keystore("HOLDER_TEST_KEYSTORE_DIR");
  const auto server_dir = dir / "server";
  LibsecretLookupHookGuard hook_guard(&lookup_missing);
  seed_secret_store_index(server_dir, "holder.ai_provider_credentials", "libsecret-missing");

  auto store = holder::privacy::make_default_secret_store(server_dir);
  const auto item = store->get("holder.ai_provider_credentials", "libsecret-missing");
  REQUIRE_FALSE(item.has_value());
}

TEST_CASE("SecretStore get returns secret when libsecret lookup succeeds", "[privacy]") {
  const auto dir = holder::test::make_temp_dir();
  EnvUnsetGuard unset_test_keystore("HOLDER_TEST_KEYSTORE_DIR");
  const auto server_dir = dir / "server";
  LibsecretLookupHookGuard hook_guard(&lookup_success);
  seed_secret_store_index(server_dir, "holder.ai_provider_credentials", "libsecret-success");

  auto store = holder::privacy::make_default_secret_store(server_dir);
  const auto item = store->get("holder.ai_provider_credentials", "libsecret-success");
  REQUIRE(item.has_value());
  REQUIRE(item->secret == "libsecret-test-secret");
  REQUIRE(item->metadata.account == "libsecret-success");
}

TEST_CASE("SecretStore get covers default libsecret lookup via low-level API seam", "[privacy]") {
  const auto dir = holder::test::make_temp_dir();
  EnvUnsetGuard unset_test_keystore("HOLDER_TEST_KEYSTORE_DIR");
  const auto server_dir = dir / "server";

  SECTION("error maps to KeyMaterialMissing") {
    LibsecretLookupHookGuard lookup_guard(nullptr);
    LibsecretApiLookupHookGuard api_guard(&api_lookup_error);
    seed_secret_store_index(
        server_dir,
        "holder.ai_provider_credentials",
        "default-libsecret-error"
    );

    auto store = holder::privacy::make_default_secret_store(server_dir);
    try {
      (void)store->get("holder.ai_provider_credentials", "default-libsecret-error");
      FAIL("Expected libsecret lookup failure");
    } catch (const holder::privacy::PrivacyError& ex) {
      REQUIRE(ex.code() == holder::privacy::PrivacyErrorCode::KeyMaterialMissing);
    }
  }

  SECTION("missing secret returns nullopt") {
    LibsecretLookupHookGuard lookup_guard(nullptr);
    LibsecretApiLookupHookGuard api_guard(&api_lookup_missing);
    seed_secret_store_index(
        server_dir,
        "holder.ai_provider_credentials",
        "default-libsecret-missing"
    );

    auto store = holder::privacy::make_default_secret_store(server_dir);
    REQUIRE_FALSE(
        store->get("holder.ai_provider_credentials", "default-libsecret-missing").has_value()
    );
  }

  SECTION("successful lookup returns stored secret") {
    LibsecretLookupHookGuard lookup_guard(nullptr);
    LibsecretApiLookupHookGuard api_guard(&api_lookup_success);
    seed_secret_store_index(
        server_dir,
        "holder.ai_provider_credentials",
        "default-libsecret-success"
    );

    auto store = holder::privacy::make_default_secret_store(server_dir);
    const auto item = store->get("holder.ai_provider_credentials", "default-libsecret-success");
    REQUIRE(item.has_value());
    REQUIRE(item->secret == "default-libsecret-secret");
  }
}
#endif
