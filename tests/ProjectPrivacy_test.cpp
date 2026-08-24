#include "git/GitOps.h"
#include "core_test_helpers.h"
#include "model/Project.h"
#include "privacy/CryptoService.h"
#include "privacy/PlatformKeyring.h"
#include "privacy/ProjectPrivacy.h"
#include "project/ProjectRepo.h"

#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

#if CARD_SERVER_HAVE_LIBGIT2
#include <git2.h>
#endif

namespace {

using holder::test::EnvUnsetGuard;

std::filesystem::path make_temp_dir_local() {
  const auto base = std::filesystem::temp_directory_path();
  const auto suffix = std::to_string(
      static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())
  );
  const auto dir = base / ("holder_project_privacy_test_" + suffix);
  std::filesystem::create_directories(dir);
  return dir;
}

void write_file(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  REQUIRE(out.is_open());
  out << text;
}

class PlatformKeyringStoreHookGuard {
 public:
  explicit PlatformKeyringStoreHookGuard(holder::privacy::PlatformKeyringStoreHook hook) {
    holder::privacy::platform_keyring_set_store_hook_for_tests(hook);
  }

  ~PlatformKeyringStoreHookGuard() {
    holder::privacy::platform_keyring_set_store_hook_for_tests(nullptr);
  }
};

class PlatformKeyringLookupHookGuard {
 public:
  explicit PlatformKeyringLookupHookGuard(holder::privacy::PlatformKeyringLookupHook hook) {
    holder::privacy::platform_keyring_set_lookup_hook_for_tests(hook);
  }

  ~PlatformKeyringLookupHookGuard() {
    holder::privacy::platform_keyring_set_lookup_hook_for_tests(nullptr);
  }
};

std::optional<std::string>
forced_keyring_store_error(const holder::privacy::PlatformKeyringSecretRef&, const std::string&) {
  return std::string("forced keyring store failure");
}

holder::privacy::PlatformKeyringLookupResult
forced_keyring_lookup_error(const holder::privacy::PlatformKeyringSecretRef&) {
  return {.secret = std::nullopt, .error_message = std::string("forced keyring lookup failure")};
}

holder::privacy::PlatformKeyringLookupResult
forced_keyring_lookup_missing(const holder::privacy::PlatformKeyringSecretRef&) {
  return {.secret = std::nullopt, .error_message = std::nullopt};
}

holder::privacy::PlatformKeyringLookupResult
forced_keyring_lookup_success(const holder::privacy::PlatformKeyringSecretRef&) {
  return {
      .secret = holder::privacy::key_to_base64(holder::privacy::generate_random_key()),
      .error_message = std::nullopt
  };
}

bool is_thread_sanitizer_run() { return std::getenv("TSAN_OPTIONS") != nullptr; }

} // namespace

TEST_CASE("Privacy safety check passes when cards directory is missing", "[privacy]") {
  const auto root = make_temp_dir_local();
  const auto check = holder::privacy::run_encryption_safety_check(root.string());
  REQUIRE(check.ok == true);
  REQUIRE(check.checked_files == 0);
  REQUIRE(check.unsafe_paths.empty());
}

TEST_CASE("Privacy safety check reports plaintext card blobs", "[privacy]") {
  const auto root = make_temp_dir_local();
  write_file(root / "cards" / "ab" / "plain.md", "# title\nhello\n");

  const auto check = holder::privacy::run_encryption_safety_check(root.string());
  REQUIRE(check.ok == false);
  REQUIRE(check.checked_files == 1);
  REQUIRE(check.unsafe_paths.size() == 1);
  REQUIRE(check.unsafe_paths[0] == "cards/ab/plain.md");
  REQUIRE_THROWS(holder::privacy::assert_encryption_push_safe(root.string()));
}

TEST_CASE("Privacy safety check accepts HolderPriv1 envelope blobs", "[privacy]") {
  const auto root = make_temp_dir_local();
  write_file(root / "cards" / "ab" / "enc.md", "HolderPriv1\n{}\nAA==\n");

  const auto check = holder::privacy::run_encryption_safety_check(root.string());
  REQUIRE(check.ok == true);
  REQUIRE(check.checked_files == 1);
  REQUIRE(check.unsafe_paths.empty());
  REQUIRE_NOTHROW(holder::privacy::assert_encryption_push_safe(root.string()));
}

TEST_CASE("Privacy safety check covers Resource and Location manifests", "[privacy]") {
  const auto root = make_temp_dir_local();
  write_file(root / "resources" / "ab" / "resource.json", "{\"resource\":{}}\n");
  write_file(root / "locations" / "cd" / "location.json", "HolderPriv1\n{}\nAA==\n");

  const auto check = holder::privacy::run_encryption_safety_check(root.string());
  REQUIRE_FALSE(check.ok);
  REQUIRE(check.checked_files == 2);
  REQUIRE(check.unsafe_paths == std::vector<std::string>{"resources/ab/resource.json"});
}

TEST_CASE("ensure_encrypted_project_ready stores 32-byte privacy key material", "[privacy]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto db = holder::test::open_db_with_schema(db_path);
  holder::project::ProjectRepo repo(db);

  holder::model::Project project;
  project.project_id = "proj-1";
  project.name = "Project";
  project.root_path = (dir / "repo").string();
  project.privacy_mode = "encrypted_git";
  project.created_at = 1;
  project.updated_at = 1;
  repo.create(project);

  holder::git::RealGitOps git;
  holder::privacy::ensure_encrypted_project_ready(
      git,
      repo,
      project.project_id,
      project.root_path,
      std::nullopt,
      2,
      []() {
        return std::string("key-1");
      }
  );

  const auto fetched = repo.get("proj-1");
  REQUIRE(fetched.has_value());
  REQUIRE(fetched->project_key_id.has_value());
  const auto key_path = dir / "keystore" / (fetched->project_key_id.value() + ".key");
  REQUIRE(std::filesystem::exists(key_path));

  std::ifstream in(key_path, std::ios::binary);
  REQUIRE(in.good());
  std::string key_b64((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const auto decoded = holder::privacy::key_from_base64(key_b64);
  REQUIRE(decoded.size() == holder::privacy::kPrivacyKeyBytes);
}

TEST_CASE("ensure_project_key_material reuses existing project key id", "[privacy]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto db = holder::test::open_db_with_schema(db_path);
  holder::project::ProjectRepo repo(db);

  holder::model::Project project;
  project.project_id = "proj-existing-key";
  project.name = "Project";
  project.root_path = (dir / "repo").string();
  project.privacy_mode = "encrypted_git";
  project.project_key_id = std::string("existing-key");
  project.created_at = 1;
  project.updated_at = 1;
  repo.create(project);

  const auto key_id = holder::privacy::ensure_project_key_material(
      repo,
      project.project_id,
      project.project_key_id,
      2,
      []() {
        return std::string("new-key");
      }
  );

  REQUIRE(key_id == "existing-key");
  REQUIRE_FALSE(std::filesystem::exists(dir / "keystore" / "new-key.key"));
}

TEST_CASE("recovery token metadata and import preserve project key and remote", "[privacy]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto db = holder::test::open_db_with_schema(db_path);
  holder::project::ProjectRepo repo(db);

  holder::model::Project project;
  project.project_id = "proj-token-metadata";
  project.name = "Home";
  project.root_path = (dir / "repo").string();
  project.privacy_mode = "encrypted_git";
  project.created_at = 1;
  project.updated_at = 1;
  repo.create(project);

  holder::git::RealGitOps git;
  holder::privacy::ensure_encrypted_project_ready(
      git,
      repo,
      project.project_id,
      project.root_path,
      std::nullopt,
      2,
      []() {
        return std::string("key-token-metadata");
      }
  );

  const auto fetched = repo.get(project.project_id);
  REQUIRE(fetched.has_value());
  REQUIRE(fetched->project_key_id.has_value());
  const auto key_id = fetched->project_key_id.value();
  const std::string remote = "git@example.com:org/repo.git";
  const auto token = holder::privacy::export_recovery_token(
      project.project_id,
      key_id,
      "1234",
      std::optional<std::string>("Home"),
      std::optional<std::string>(remote)
  );

  const auto metadata = holder::privacy::inspect_recovery_token("1234", token);
  REQUIRE(metadata.project_id == project.project_id);
  REQUIRE(metadata.project_key_id == key_id);
  REQUIRE(metadata.project_name == std::optional<std::string>("Home"));
  REQUIRE(metadata.git_remote_url == std::optional<std::string>(remote));

  repo.update_project_key_id(project.project_id, std::optional<std::string>("old-key"), 3);
  repo.update_git_remote(project.project_id, std::nullopt, 3);

  REQUIRE_NOTHROW(holder::privacy::import_recovery_token(
      repo,
      project.project_id,
      "1234",
      token,
      4
  ));

  const auto imported = repo.get(project.project_id);
  REQUIRE(imported.has_value());
  REQUIRE(imported->project_key_id == std::optional<std::string>(key_id));
  REQUIRE(imported->git_remote_url == std::optional<std::string>(remote));
}

TEST_CASE("recovery token import maps project id mismatch to RecoveryTokenInvalid", "[privacy]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto db = holder::test::open_db_with_schema(db_path);
  holder::project::ProjectRepo repo(db);

  holder::model::Project project;
  project.project_id = "proj-token-mismatch";
  project.name = "Project";
  project.root_path = (dir / "repo").string();
  project.privacy_mode = "encrypted_git";
  project.created_at = 1;
  project.updated_at = 1;
  repo.create(project);

  holder::git::RealGitOps git;
  holder::privacy::ensure_encrypted_project_ready(
      git,
      repo,
      project.project_id,
      project.root_path,
      std::nullopt,
      2,
      []() {
        return std::string("key-token-mismatch");
      }
  );

  const auto fetched = repo.get(project.project_id);
  REQUIRE(fetched.has_value());
  REQUIRE(fetched->project_key_id.has_value());
  const auto token = holder::privacy::export_recovery_token(
      project.project_id,
      fetched->project_key_id.value(),
      "1234"
  );

  try {
    holder::privacy::import_recovery_token(repo, "different-project", "1234", token, 3);
    FAIL("Expected recovery token mismatch");
  } catch (const holder::privacy::PrivacyError& ex) {
    REQUIRE(ex.code() == holder::privacy::PrivacyErrorCode::RecoveryTokenInvalid);
  }
}

TEST_CASE("staged card blob fails index safety check without explicit encryption", "[privacy]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto db = holder::test::open_db_with_schema(db_path);
  holder::project::ProjectRepo repo(db);

  holder::model::Project project;
  project.project_id = "proj-1";
  project.name = "Project";
  project.root_path = (dir / "repo").string();
  project.privacy_mode = "encrypted_git";
  project.created_at = 1;
  project.updated_at = 1;
  repo.create(project);

  holder::git::RealGitOps git;
  holder::privacy::ensure_encrypted_project_ready(
      git,
      repo,
      project.project_id,
      project.root_path,
      std::nullopt,
      2,
      []() {
        return std::string("key-1");
      }
  );

  const std::string rel = "cards/aa/aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa.md";
  git.write_file(rel, "# hello\nworld\n");
  git.stage_path(rel);

  REQUIRE_THROWS_AS(
      holder::privacy::assert_encryption_index_paths_safe(project.root_path, {rel}),
      holder::privacy::PrivacyError
  );
}

TEST_CASE("staged plaintext card blob fails index safety check", "[privacy]") {
  const auto dir = make_temp_dir_local();
  holder::git::RealGitOps git;
  git.open_or_init(dir);
  const std::string rel = "cards/aa/plain.md";
  git.write_file(rel, "# plain\n");
  git.stage_path(rel);

  REQUIRE_THROWS_AS(
      holder::privacy::assert_encryption_index_paths_safe(dir.string(), {rel}),
      holder::privacy::PrivacyError
  );
}

TEST_CASE("recovery token import wrong PIN returns typed privacy error", "[privacy]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto db = holder::test::open_db_with_schema(db_path);
  holder::project::ProjectRepo repo(db);

  holder::model::Project project;
  project.project_id = "proj-1";
  project.name = "Project";
  project.root_path = (dir / "repo").string();
  project.privacy_mode = "encrypted_git";
  project.created_at = 1;
  project.updated_at = 1;
  repo.create(project);

  holder::git::RealGitOps git;
  holder::privacy::ensure_encrypted_project_ready(
      git,
      repo,
      project.project_id,
      project.root_path,
      std::nullopt,
      2,
      []() {
        return std::string("key-1");
      }
  );

  const auto fetched = repo.get("proj-1");
  REQUIRE(fetched.has_value());
  REQUIRE(fetched->project_key_id.has_value());
  const std::string key_id = fetched->project_key_id.value();

  const std::string token = holder::privacy::export_recovery_token("proj-1", key_id, "1234");
  try {
    holder::privacy::import_recovery_token(repo, "proj-1", "wrong", token, 3);
    FAIL("Expected privacy error");
  } catch (const holder::privacy::PrivacyError& ex) {
    REQUIRE(ex.code() == holder::privacy::PrivacyErrorCode::RecoveryTokenInvalid);
  }
}

TEST_CASE("export_recovery_token rejects empty PIN", "[privacy]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto db = holder::test::open_db_with_schema(db_path);
  holder::project::ProjectRepo repo(db);

  holder::model::Project project;
  project.project_id = "proj-pin-empty";
  project.name = "Project";
  project.root_path = (dir / "repo").string();
  project.privacy_mode = "encrypted_git";
  project.created_at = 1;
  project.updated_at = 1;
  repo.create(project);

  holder::git::RealGitOps git;
  holder::privacy::ensure_encrypted_project_ready(
      git,
      repo,
      project.project_id,
      project.root_path,
      std::nullopt,
      2,
      []() {
        return std::string("key-empty-pin");
      }
  );

  const auto fetched = repo.get(project.project_id);
  REQUIRE(fetched.has_value());
  REQUIRE(fetched->project_key_id.has_value());
  REQUIRE_THROWS(holder::privacy::export_recovery_token(
      project.project_id,
      fetched->project_key_id.value(),
      ""
  ));
}

TEST_CASE(
    "export_recovery_token fails when key material is missing from test keystore",
    "[privacy]"
) {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  holder::test::EnvGuard keystore_env(
      "HOLDER_TEST_KEYSTORE_DIR",
      (dir / "empty-keystore").string()
  );
  auto db = holder::test::open_db_with_schema(db_path);

  REQUIRE_THROWS_AS(
      holder::privacy::export_recovery_token("proj-missing", "key-missing", "1234"),
      holder::privacy::PrivacyError
  );
}

TEST_CASE("inspect_recovery_token maps wrong PIN to RecoveryTokenInvalid", "[privacy]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto db = holder::test::open_db_with_schema(db_path);
  holder::project::ProjectRepo repo(db);

  holder::model::Project project;
  project.project_id = "proj-inspect";
  project.name = "Project";
  project.root_path = (dir / "repo").string();
  project.privacy_mode = "encrypted_git";
  project.created_at = 1;
  project.updated_at = 1;
  repo.create(project);

  holder::git::RealGitOps git;
  holder::privacy::ensure_encrypted_project_ready(
      git,
      repo,
      project.project_id,
      project.root_path,
      std::nullopt,
      2,
      []() {
        return std::string("key-inspect");
      }
  );

  const auto fetched = repo.get(project.project_id);
  REQUIRE(fetched.has_value());
  REQUIRE(fetched->project_key_id.has_value());

  const std::string token = holder::privacy::export_recovery_token(
      project.project_id,
      fetched->project_key_id.value(),
      "1234",
      std::optional<std::string>("Home"),
      std::optional<std::string>("git@example.com:org/repo.git")
  );

  try {
    (void)holder::privacy::inspect_recovery_token("wrong", token);
    FAIL("Expected recovery token error");
  } catch (const holder::privacy::PrivacyError& ex) {
    REQUIRE(ex.code() == holder::privacy::PrivacyErrorCode::RecoveryTokenInvalid);
  }
}

TEST_CASE("inspect_recovery_token maps malformed token JSON to RecoveryTokenInvalid", "[privacy]") {
  try {
    (void)holder::privacy::inspect_recovery_token("1234", "{not-json");
    FAIL("Expected recovery token error");
  } catch (const holder::privacy::PrivacyError& ex) {
    REQUIRE(ex.code() == holder::privacy::PrivacyErrorCode::RecoveryTokenInvalid);
  }
}

TEST_CASE("privacy safety check treats unreadable card file as unsafe", "[privacy]") {
#ifdef _WIN32
  SKIP("POSIX permission-bit unreadable-file test is not meaningful on Windows");
#else
  const auto root = make_temp_dir_local();
  const auto path = root / "cards" / "ab" / "locked.md";
  write_file(path, "HolderPriv1\n{}\nAA==\n");
  std::filesystem::permissions(
      path,
      std::filesystem::perms::none,
      std::filesystem::perm_options::replace
  );

  const auto check = holder::privacy::run_encryption_safety_check(root.string());
  REQUIRE(check.ok == false);
  REQUIRE(check.checked_files == 1);
  REQUIRE(check.unsafe_paths.size() == 1);
#endif
}

TEST_CASE("index safety check ignores unrelated paths and missing protected paths", "[privacy]") {
  const auto root = make_temp_dir_local();
  holder::git::RealGitOps git;
  git.open_or_init(root);

  REQUIRE_NOTHROW(holder::privacy::assert_encryption_index_paths_safe(
      root.string(),
      {"notes/a.md", "cards/aa/missing.md", "resources/aa/missing.json"}
  ));
}

TEST_CASE("staged plaintext Resource and Location manifests fail index safety check", "[privacy]") {
  const auto root = make_temp_dir_local();
  holder::git::RealGitOps git;
  git.open_or_init(root);
  const std::string resource = "resources/aa/plain.json";
  const std::string location = "locations/bb/plain.json";
  git.write_file(resource, "{}\n");
  git.write_file(location, "{}\n");
  git.stage_path(resource);
  git.stage_path(location);

  REQUIRE_THROWS_AS(
      holder::privacy::assert_encryption_index_paths_safe(root.string(), {resource, location}),
      holder::privacy::PrivacyError
  );
}

TEST_CASE("index safety check errors when repository cannot be opened", "[privacy]") {
  const auto root = make_temp_dir_local();
  REQUIRE_THROWS_AS(
      holder::privacy::assert_encryption_index_paths_safe(root.string(), {"cards/aa/a.md"}),
      holder::privacy::PrivacyError
  );
}

TEST_CASE("inspect_recovery_token maps empty pin to RecoveryTokenInvalid", "[privacy]") {
  const auto token = nlohmann::json{{"version", 1}}.dump();
  try {
    (void)holder::privacy::inspect_recovery_token("", token);
    FAIL("Expected recovery token error");
  } catch (const holder::privacy::PrivacyError& ex) {
    REQUIRE(ex.code() == holder::privacy::PrivacyErrorCode::RecoveryTokenInvalid);
  }
}

TEST_CASE("inspect_recovery_token maps unsupported version to RecoveryTokenInvalid", "[privacy]") {
  const auto token = nlohmann::json{{"version", 2}}.dump();
  try {
    (void)holder::privacy::inspect_recovery_token("1234", token);
    FAIL("Expected recovery token error");
  } catch (const holder::privacy::PrivacyError& ex) {
    REQUIRE(ex.code() == holder::privacy::PrivacyErrorCode::RecoveryTokenInvalid);
  }
}

TEST_CASE("encrypt_project_blob rethrows PrivacyError for invalid key material", "[privacy]") {
  const auto dir = holder::test::make_temp_dir();
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  std::filesystem::create_directories(dir / "keystore");
  {
    std::ofstream out(dir / "keystore" / "key-invalid.key", std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open());
    out << "not_base64";
  }

  try {
    (void)holder::privacy::encrypt_project_blob("proj-1", "key-invalid", "hello");
    FAIL("Expected privacy error");
  } catch (const holder::privacy::PrivacyError& ex) {
    REQUIRE(ex.code() == holder::privacy::PrivacyErrorCode::EnvelopeInvalid);
  }
}

TEST_CASE(
    "ensure_project_key_material fails when test keystore key path is a directory",
    "[privacy]"
) {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);
  holder::project::ProjectRepo repo(db);

  holder::model::Project project;
  project.project_id = "proj-key-open-fail";
  project.name = "Project";
  project.root_path = (dir / "repo").string();
  project.privacy_mode = "encrypted_git";
  project.created_at = 1;
  project.updated_at = 1;
  repo.create(project);

  const auto keystore_dir = dir / "keystore";
  std::filesystem::create_directories(keystore_dir / "key-open-fail.key");
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", keystore_dir.string());

  REQUIRE_THROWS(holder::privacy::ensure_project_key_material(
      repo,
      project.project_id,
      std::nullopt,
      2,
      []() {
        return std::string("key-open-fail");
      }
  ));
}

TEST_CASE(
    "ensure_encrypted_git_setup fails when privacy metadata target is a directory",
    "[privacy]"
) {
  const auto dir = holder::test::make_temp_dir();
  const auto repo_root = dir / "repo";

  holder::git::RealGitOps git;
  git.open_or_init(repo_root);

  // Force std::ofstream open failure for .holder/privacy.json.
  std::filesystem::create_directories(repo_root / ".holder" / "privacy.json");

  REQUIRE_THROWS(holder::privacy::ensure_encrypted_git_setup(
      git,
      repo_root.string(),
      "proj-meta-fail",
      "key-meta-fail"
  ));
}

TEST_CASE(
    "ensure_project_key_material reports keyring unavailable when platform keyring store fails",
    "[privacy]"
) {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);
  holder::project::ProjectRepo repo(db);

  holder::model::Project project;
  project.project_id = "proj-keyring-store";
  project.name = "Project";
  project.root_path = (dir / "repo").string();
  project.privacy_mode = "encrypted_git";
  project.created_at = 1;
  project.updated_at = 1;
  repo.create(project);

  EnvUnsetGuard unset_test_keystore("HOLDER_TEST_KEYSTORE_DIR");
  PlatformKeyringStoreHookGuard hook_guard(&forced_keyring_store_error);

  try {
    (void
    )holder::privacy::ensure_project_key_material(repo, project.project_id, std::nullopt, 2, []() {
      return std::string("keyring-store");
    });
    FAIL("Expected keyring failure");
  } catch (const holder::privacy::PrivacyError& ex) {
    REQUIRE(ex.code() == holder::privacy::PrivacyErrorCode::KeyringUnavailable);
  }
}

#if HOLDER_HAVE_LIBSECRET || HOLDER_HAVE_MACOS_KEYCHAIN
TEST_CASE(
    "export_recovery_token maps platform keyring lookup failure to KeyMaterialMissing",
    "[privacy]"
) {
  EnvUnsetGuard unset_test_keystore("HOLDER_TEST_KEYSTORE_DIR");
  PlatformKeyringLookupHookGuard hook_guard(&forced_keyring_lookup_error);

  try {
    (void)holder::privacy::export_recovery_token(
        "proj-platform-missing",
        "key-platform-missing",
        "1234"
    );
    FAIL("Expected missing key material error");
  } catch (const holder::privacy::PrivacyError& ex) {
    REQUIRE(ex.code() == holder::privacy::PrivacyErrorCode::KeyMaterialMissing);
  }
}

TEST_CASE("export_recovery_token maps platform keyring miss to KeyMaterialMissing", "[privacy]") {
  EnvUnsetGuard unset_test_keystore("HOLDER_TEST_KEYSTORE_DIR");
  PlatformKeyringLookupHookGuard hook_guard(&forced_keyring_lookup_missing);

  try {
    (void)holder::privacy::export_recovery_token("proj-platform-miss", "key-platform-miss", "1234");
    FAIL("Expected missing key material error");
  } catch (const holder::privacy::PrivacyError& ex) {
    REQUIRE(ex.code() == holder::privacy::PrivacyErrorCode::KeyMaterialMissing);
  }
}

TEST_CASE("export_recovery_token can read key material from platform keyring", "[privacy]") {
  EnvUnsetGuard unset_test_keystore("HOLDER_TEST_KEYSTORE_DIR");
  PlatformKeyringLookupHookGuard hook_guard(&forced_keyring_lookup_success);

  const auto token = holder::privacy::export_recovery_token(
      "proj-platform-success",
      "key-platform-success",
      "1234"
  );
  REQUIRE_FALSE(token.empty());
}
#endif

TEST_CASE("import_recovery_token rethrows non-token privacy errors", "[privacy]") {
  if (is_thread_sanitizer_run()) {
    SKIP("Real libsecret failure path starts GLib/GDBus helper threads that are noisy under TSan");
  }

  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);
  holder::project::ProjectRepo repo(db);

  holder::model::Project project;
  project.project_id = "proj-import-rethrow";
  project.name = "Project";
  project.root_path = (dir / "repo").string();
  project.privacy_mode = "encrypted_git";
  project.created_at = 1;
  project.updated_at = 1;
  repo.create(project);

  // Create token while test keystore override is enabled.
  std::string token;
  {
    holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
    holder::git::RealGitOps git;
    holder::privacy::ensure_encrypted_project_ready(
        git,
        repo,
        project.project_id,
        project.root_path,
        std::nullopt,
        2,
        []() {
          return std::string("key-import-rethrow");
        }
    );

    const auto fetched = repo.get(project.project_id);
    REQUIRE(fetched.has_value());
    REQUIRE(fetched->project_key_id.has_value());
    token = holder::privacy::export_recovery_token(
        project.project_id,
        fetched->project_key_id.value(),
        "1234"
    );
  }

  // Import with platform keyring storage forced to fail so store_key_material throws
  // KeyringUnavailable.
  EnvUnsetGuard unset_test_keystore("HOLDER_TEST_KEYSTORE_DIR");
  PlatformKeyringStoreHookGuard hook_guard(&forced_keyring_store_error);

  try {
    holder::privacy::import_recovery_token(repo, project.project_id, "1234", token, 3);
    FAIL("Expected keyring failure");
  } catch (const holder::privacy::PrivacyError& ex) {
    REQUIRE(ex.code() == holder::privacy::PrivacyErrorCode::KeyringUnavailable);
  }
}

TEST_CASE(
    "inspect_recovery_token maps invalid base64 payload to RecoveryTokenInvalid",
    "[privacy]"
) {
  nlohmann::json token;
  token["version"] = 1;
  token["kdf"] = {
      {"name", "PBKDF2-HMAC-SHA256"},
      {"iterations", 210000},
      {"salt_b64", "!!!!not-b64!!!!"},
  };
  token["cipher"] = {
      {"name", "holder-privacy-envelope-v1"},
      {"wrapped", "ignored"},
  };

  try {
    (void)holder::privacy::inspect_recovery_token("1234", token.dump());
    FAIL("Expected recovery token invalid error");
  } catch (const holder::privacy::PrivacyError& ex) {
    REQUIRE(ex.code() == holder::privacy::PrivacyErrorCode::RecoveryTokenInvalid);
  }
}

#if CARD_SERVER_HAVE_LIBGIT2
TEST_CASE("index safety check errors when git index cannot be loaded", "[privacy]") {
  const auto root = make_temp_dir_local();
  holder::git::RealGitOps git;
  git.open_or_init(root);

  const auto index_path = root / ".git" / "index";
  if (std::filesystem::exists(index_path) && std::filesystem::is_regular_file(index_path)) {
    std::filesystem::remove(index_path);
  }
  std::filesystem::create_directories(index_path);

  REQUIRE_THROWS_AS(
      holder::privacy::assert_encryption_index_paths_safe(root.string(), {"cards/aa/a.md"}),
      holder::privacy::PrivacyError
  );
}

TEST_CASE("index safety check treats unreadable staged blob as unsafe", "[privacy]") {
  const auto root = make_temp_dir_local();
  holder::git::RealGitOps git;
  git.open_or_init(root);

  const std::string rel = "cards/aa/blob-missing.md";
  git.write_file(rel, "HolderPriv1\n{}\nAA==\n");
  git.stage_path(rel);

  git_repository* repo = nullptr;
  REQUIRE(git_repository_open(&repo, root.string().c_str()) == 0);
  git_index* index = nullptr;
  REQUIRE(git_repository_index(&index, repo) == 0);
  const git_index_entry* entry = git_index_get_bypath(index, rel.c_str(), 0);
  REQUIRE(entry != nullptr);
  char oid_hex[GIT_OID_HEXSZ + 1]{};
  git_oid_tostr(oid_hex, sizeof(oid_hex), &entry->id);
  git_index_free(index);
  git_repository_free(repo);

  const std::string hex(oid_hex);
  REQUIRE(hex.size() == GIT_OID_HEXSZ);
  const auto object_path = root / ".git" / "objects" / hex.substr(0, 2) / hex.substr(2);
  REQUIRE(std::filesystem::exists(object_path));
  std::filesystem::remove(object_path);

  REQUIRE_THROWS_AS(
      holder::privacy::assert_encryption_index_paths_safe(root.string(), {rel}),
      holder::privacy::PrivacyError
  );
}
#endif
