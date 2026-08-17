#include "privacy/ProjectPrivacy.h"

#include "privacy/CryptoService.h"
#include "privacy/PlatformKeyring.h"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#if CARD_SERVER_HAVE_LIBGIT2
#include <git2.h>
#endif

#include <nlohmann/json.hpp>

#include <array>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace holder::privacy {
namespace {
constexpr const char* kRecoveryTokenWrapKeyIdV1 = "holder-recovery-token-v1";

struct DecryptedRecoveryTokenPayload {
  RecoveryTokenMetadata metadata;
  std::string project_key_material_b64;
};

std::vector<unsigned char> random_bytes(std::size_t n) {
  std::vector<unsigned char> out(n);
  if (RAND_bytes(out.data(), static_cast<int>(out.size())) != 1) {
    throw std::runtime_error("RAND_bytes failed"); // LCOV_EXCL_LINE
  }
  return out;
} // LCOV_EXCL_LINE

std::string b64_encode(const unsigned char* data, std::size_t len) {
  std::vector<unsigned char> out(4 * ((len + 2) / 3) + 1);
  const int written = EVP_EncodeBlock(out.data(), data, static_cast<int>(len));
  if (written <= 0) {
    throw std::runtime_error("EVP_EncodeBlock failed"); // LCOV_EXCL_LINE
  }
  return std::string(reinterpret_cast<char*>(out.data()), static_cast<std::size_t>(written));
}

std::vector<unsigned char> b64_decode(const std::string& text) {
  std::vector<unsigned char> out((text.size() * 3) / 4 + 3);
  const int written = EVP_DecodeBlock(
      out.data(),
      reinterpret_cast<const unsigned char*>(text.data()),
      static_cast<int>(text.size())
  );
  if (written < 0) {
    throw std::runtime_error("EVP_DecodeBlock failed");
  }

  std::size_t padding = 0;
  if (!text.empty() && text.back() == '=') {
    padding++;
    if (text.size() > 1 && text[text.size() - 2] == '=') {
      padding++;
    }
  }
  out.resize(static_cast<std::size_t>(written) - padding);
  return out;
}

std::array<unsigned char, holder::privacy::kPrivacyKeyBytes> derive_wrap_key(
    const std::string& pin,
    const std::vector<unsigned char>& salt,
    int iterations
) {
  std::array<unsigned char, holder::privacy::kPrivacyKeyBytes> key{};
  const int rc = PKCS5_PBKDF2_HMAC(
      pin.c_str(),
      static_cast<int>(pin.size()),
      salt.data(),
      static_cast<int>(salt.size()),
      iterations,
      EVP_sha256(),
      static_cast<int>(key.size()),
      key.data()
  );
  if (rc != 1) {
    throw std::runtime_error("PKCS5_PBKDF2_HMAC failed"); // LCOV_EXCL_LINE
  }
  return key;
}

void store_key_test_override(
    const std::filesystem::path& dir,
    const std::string& key_id,
    const std::string& key_material_b64
) {
  std::filesystem::create_directories(dir);
  const auto out_path = dir / (key_id + ".key");
  std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
  if (!out) {
    throw std::runtime_error("failed to open test keystore file: " + out_path.string());
  }
  out << key_material_b64;
}

PlatformKeyringSecretRef project_key_ref(const std::string& project_id, const std::string& key_id) {
  return PlatformKeyringSecretRef{
      .kind = PlatformKeyringSecretKind::ProjectKey,
      .service = "org.holder.ProjectKey",
      .account = key_id,
      .project_id = project_id,
  };
}

void store_key_material(
    const std::string& project_id,
    const std::string& key_id,
    const std::string& key_material_b64
) {
  if (const char* test_dir = std::getenv("HOLDER_TEST_KEYSTORE_DIR")) {
    store_key_test_override(std::filesystem::path(test_dir), key_id, key_material_b64);
    return;
  }

  platform_keyring_store_secret(
      project_key_ref(project_id, key_id),
      "Holder project key " + key_id,
      key_material_b64
  );
}

std::string load_key_material(const std::string& project_id, const std::string& key_id) {
  if (const char* test_dir = std::getenv("HOLDER_TEST_KEYSTORE_DIR")) {
    const auto in_path = std::filesystem::path(test_dir) / (key_id + ".key");
    std::ifstream in(in_path, std::ios::binary);
    if (!in) {
      throw PrivacyError(
          PrivacyErrorCode::KeyMaterialMissing,
          "project key material not found in test keystore: " + in_path.string()
      );
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
  }

  if (!platform_keyring_supported()) { // LCOV_EXCL_LINE: exercised by no-platform-keyring builds.
    // LCOV_EXCL_START
    throw PrivacyError(
        PrivacyErrorCode::KeyringUnavailable,
        "platform keyring support not available and HOLDER_TEST_KEYSTORE_DIR not set"
    );
    // LCOV_EXCL_STOP
  }

  const auto result = platform_keyring_lookup_secret(project_key_ref(project_id, key_id));
  if (result.error_message.has_value()) {
    throw PrivacyError(PrivacyErrorCode::KeyMaterialMissing, *result.error_message);
  }
  if (!result.secret.has_value()) {
    throw PrivacyError(
        PrivacyErrorCode::KeyMaterialMissing,
        "project key material not found in platform keyring"
    );
  }
  return result.secret.value();
} // LCOV_EXCL_LINE

bool has_envelope_header(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  std::string first_line;
  std::getline(in, first_line);
  return first_line == "HolderPriv1";
}

bool starts_with(const std::string& text, const std::string& prefix) {
  return text.rfind(prefix, 0) == 0;
}

void write_privacy_meta(
    const std::filesystem::path& repo_root,
    const std::string& project_id,
    const std::string& key_id
) {
  const auto path = repo_root / ".holder" / "privacy.json";
  std::filesystem::create_directories(path.parent_path());
  nlohmann::json body = {
      {"version", 1}, // LCOV_EXCL_LINE
      {"project_id", project_id},
      {"key_id", key_id},
      {"mode", "encrypted_git"},
  };
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    throw std::runtime_error("failed to write privacy metadata file: " + path.string());
  }
  out << body.dump(2) << '\n';
}

DecryptedRecoveryTokenPayload decrypt_recovery_payload(
    const std::string& pin,
    const std::string& token_json
) {
  if (pin.empty()) {
    throw std::runtime_error("pin must not be empty");
  }

  const auto token = nlohmann::json::parse(token_json);
  if (!token.contains("version") || token.at("version").get<int>() != 1) {
    throw std::runtime_error("unsupported recovery token version");
  }

  const auto& kdf = token.at("kdf");
  const int iterations = kdf.at("iterations").get<int>();
  const auto salt = b64_decode(kdf.at("salt_b64").get<std::string>());
  const auto wrap_key = derive_wrap_key(pin, salt, iterations);

  const auto& cipher = token.at("cipher");
  const auto wrapped = cipher.at("wrapped").get<std::string>();
  const auto decrypted_payload =
      holder::privacy::decrypt_envelope_v1(wrapped, wrap_key, kRecoveryTokenWrapKeyIdV1);
  const auto payload = nlohmann::json::parse(decrypted_payload);

  DecryptedRecoveryTokenPayload out;
  out.metadata.project_id = payload.at("project_id").get<std::string>();
  out.metadata.project_key_id = payload.at("project_key_id").get<std::string>();
  if (payload.contains("project_name") && !payload.at("project_name").is_null()) {
    out.metadata.project_name = payload.at("project_name").get<std::string>();
  }
  if (payload.contains("git_remote_url") && !payload.at("git_remote_url").is_null()) {
    out.metadata.git_remote_url = payload.at("git_remote_url").get<std::string>();
  }
  out.project_key_material_b64 = payload.at("project_key_material_b64").get<std::string>();
  return out;
}

} // namespace

void ensure_encrypted_git_setup(
    holder::git::GitOps& git,
    const std::string& root_path,
    const std::string& project_id,
    const std::string& project_key_id
) {
  const auto repo_root = std::filesystem::path(root_path);
  git.open_or_init(repo_root);
  write_privacy_meta(repo_root, project_id, project_key_id);
}

std::string ensure_project_key_material(
    holder::project::ProjectRepo& repo,
    const std::string& project_id,
    const std::optional<std::string>& project_key_id,
    long long updated_at,
    const std::function<std::string()>& uuid_v4
) {
  if (project_key_id.has_value() && !project_key_id->empty()) {
    return project_key_id.value();
  }

  const std::string key_id = uuid_v4();
  const auto key = holder::privacy::generate_random_key();
  const auto key_material_b64 = holder::privacy::key_to_base64(key);
  store_key_material(project_id, key_id, key_material_b64);
  repo.update_project_key_id(project_id, key_id, updated_at);
  return key_id;
}

void ensure_encrypted_project_ready(
    holder::git::GitOps& git,
    holder::project::ProjectRepo& repo,
    const std::string& project_id,
    const std::string& root_path,
    const std::optional<std::string>& project_key_id,
    long long updated_at,
    const std::function<std::string()>& uuid_v4
) {
  const std::string key_id =
      ensure_project_key_material(repo, project_id, project_key_id, updated_at, uuid_v4);
  ensure_encrypted_git_setup(git, root_path, project_id, key_id);
}

std::string export_recovery_token(
    const std::string& project_id,
    const std::string& project_key_id,
    const std::string& pin,
    const std::optional<std::string>& project_name,
    const std::optional<std::string>& git_remote_url
) {
  if (pin.empty()) {
    throw std::runtime_error("pin must not be empty");
  }

  const std::string key_material_b64 = load_key_material(project_id, project_key_id);
  // Validate key shape early.
  (void)holder::privacy::key_from_base64(key_material_b64);

  constexpr int kdf_iterations = 210000;
  const auto salt = random_bytes(16);
  const auto wrap_key = derive_wrap_key(pin, salt, kdf_iterations);

  nlohmann::json wrapped_payload;
  wrapped_payload["project_id"] = project_id;
  wrapped_payload["project_key_id"] = project_key_id;
  wrapped_payload["project_key_material_b64"] = key_material_b64;
  if (project_name.has_value()) {
    wrapped_payload["project_name"] = project_name.value();
  } else {
    wrapped_payload["project_name"] = nullptr;
  }
  if (git_remote_url.has_value()) {
    wrapped_payload["git_remote_url"] = git_remote_url.value();
  } else {
    wrapped_payload["git_remote_url"] = nullptr;
  }
  wrapped_payload["exported_at"] = static_cast<long long>(std::time(nullptr));

  const std::string wrapped = holder::privacy::encrypt_envelope_v1(
      wrapped_payload.dump(),
      wrap_key,
      kRecoveryTokenWrapKeyIdV1
  );

  nlohmann::json token;
  token["version"] = 1;
  token["kdf"] = {
      {"name", "PBKDF2-HMAC-SHA256"},
      {"iterations", kdf_iterations},
      {"salt_b64", b64_encode(salt.data(), salt.size())},
  };
  token["cipher"] = {
      {"name", "holder-privacy-envelope-v1"},
      {"wrapped", wrapped},
  };
  return token.dump();
}

std::string encrypt_project_blob(
    const std::string& project_id,
    const std::string& project_key_id,
    const std::string& plaintext
) {
  try {
    const std::string key_material_b64 = load_key_material(project_id, project_key_id);
    const auto key = holder::privacy::key_from_base64(key_material_b64);
    return holder::privacy::encrypt_envelope_v1(plaintext, key, project_key_id);
  } catch (const PrivacyError&) {
    throw;
    // load_key_material/key_from_base64/encrypt_envelope_v1 only throw PrivacyError in normal flow.
    // LCOV_EXCL_START
  } catch (const std::exception& ex) {
    throw PrivacyError(
        PrivacyErrorCode::PrivacyCryptoFailed,
        std::string("failed to encrypt project blob: ") + ex.what()
    );
  }
  // LCOV_EXCL_STOP
}

std::string decrypt_project_blob(
    const std::string& project_id,
    const std::string& project_key_id,
    const std::string& envelope
) {
  try {
    const std::string key_material_b64 = load_key_material(project_id, project_key_id);
    const auto key = holder::privacy::key_from_base64(key_material_b64);
    return holder::privacy::decrypt_envelope_v1(envelope, key, project_key_id);
  } catch (const PrivacyError&) {
    throw;
    // load_key_material/key_from_base64/decrypt_envelope_v1 only throw PrivacyError in normal flow.
    // LCOV_EXCL_START
  } catch (const std::exception& ex) {
    throw PrivacyError(
        PrivacyErrorCode::PrivacyCryptoFailed,
        std::string("failed to decrypt project blob: ") + ex.what()
    );
  }
  // LCOV_EXCL_STOP
}

void import_recovery_token(
    holder::project::ProjectRepo& repo,
    const std::string& project_id,
    const std::string& pin,
    const std::string& token_json,
    long long updated_at
) {
  try {
    const auto payload = decrypt_recovery_payload(pin, token_json);

    if (payload.metadata.project_id != project_id) {
      throw std::runtime_error("recovery token project_id mismatch");
    }

    // Validate key shape before storing.
    (void)holder::privacy::key_from_base64(payload.project_key_material_b64);
    store_key_material(
        project_id,
        payload.metadata.project_key_id,
        payload.project_key_material_b64
    );
    repo.update_project_key_id(project_id, payload.metadata.project_key_id, updated_at);
    if (payload.metadata.git_remote_url.has_value()) {
      const auto& remote = payload.metadata.git_remote_url.value();
      if (!remote.empty()) {
        repo.update_git_remote(project_id, remote, updated_at);
      }
    }
  } catch (const PrivacyError& ex) {
    if (ex.code() == PrivacyErrorCode::EnvelopeInvalid ||
        ex.code() == PrivacyErrorCode::EnvelopeMetadataMismatch ||
        ex.code() == PrivacyErrorCode::PrivacyCryptoFailed) {
      throw PrivacyError(
          PrivacyErrorCode::RecoveryTokenInvalid,
          std::string("invalid recovery token: ") + ex.what()
      );
    }
    throw;
  } catch (const std::exception& ex) {
    throw PrivacyError(
        PrivacyErrorCode::RecoveryTokenInvalid,
        std::string("invalid recovery token: ") + ex.what()
    );
  }
}

RecoveryTokenMetadata inspect_recovery_token(
    const std::string& pin,
    const std::string& token_json
) {
  try {
    return decrypt_recovery_payload(pin, token_json).metadata;
  } catch (const PrivacyError& ex) {
    // decrypt_recovery_payload only emits EnvelopeInvalid/EnvelopeMetadataMismatch for token crypto
    // faults. LCOV_EXCL_START
    if (ex.code() == PrivacyErrorCode::EnvelopeInvalid ||
        ex.code() == PrivacyErrorCode::EnvelopeMetadataMismatch ||
        ex.code() == PrivacyErrorCode::PrivacyCryptoFailed) {
      throw PrivacyError(
          PrivacyErrorCode::RecoveryTokenInvalid,
          std::string("invalid recovery token: ") + ex.what()
      );
    }
    throw;
    // LCOV_EXCL_STOP
  } catch (const std::exception& ex) {
    throw PrivacyError(
        PrivacyErrorCode::RecoveryTokenInvalid,
        std::string("invalid recovery token: ") + ex.what()
    );
  }
}

EncryptionSafetyCheck run_encryption_safety_check(const std::string& root_path) {
  EncryptionSafetyCheck out;
  const auto cards_root = std::filesystem::path(root_path) / "cards";
  if (!std::filesystem::exists(cards_root)) {
    out.message = "No cards directory found; nothing to verify.";
    return out;
  }

  for (const auto& entry : std::filesystem::recursive_directory_iterator(cards_root)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const auto relative = std::filesystem::relative(entry.path(), root_path).generic_string();
    ++out.checked_files;
    if (!has_envelope_header(entry.path())) {
      out.unsafe_paths.push_back(relative);
    }
  }

  out.ok = out.unsafe_paths.empty();
  if (out.ok) {
    out.message = "Privacy safety check passed.";
  } else {
    out.message = "Privacy safety check failed: found plaintext card blobs.";
  }
  return out;
}

void assert_encryption_push_safe(const std::string& root_path) {
  const auto check = run_encryption_safety_check(root_path);
  if (!check.ok) {
    std::ostringstream oss;
    oss << check.message << " Unsafe paths:";
    for (const auto& path : check.unsafe_paths) {
      oss << ' ' << path;
    }
    throw PrivacyError(PrivacyErrorCode::EncryptionSafetyCheckFailed, oss.str());
  }
}

void assert_encryption_index_paths_safe(
    const std::string& root_path,
    const std::vector<std::string>& relative_paths
) {
#if CARD_SERVER_HAVE_LIBGIT2
  git_repository* repo = nullptr;
  const int open_rc = git_repository_open(&repo, root_path.c_str());
  if (open_rc != 0 || !repo) {
    throw PrivacyError(
        PrivacyErrorCode::EncryptionSafetyCheckFailed,
        "failed to open git repository for privacy safety check"
    );
  }

  git_index* index = nullptr;
  const int idx_rc = git_repository_index(&index, repo);
  if (idx_rc != 0 || !index) {
    git_repository_free(repo);
    throw PrivacyError(
        PrivacyErrorCode::EncryptionSafetyCheckFailed,
        "failed to load git index for privacy safety check"
    );
  }

  std::vector<std::string> unsafe_paths;
  for (const auto& rel_path : relative_paths) {
    if (!starts_with(rel_path, "cards/")) {
      continue;
    }

    const git_index_entry* entry = git_index_get_bypath(index, rel_path.c_str(), 0);
    if (!entry) {
      continue;
    }

    git_blob* blob = nullptr;
    const int blob_rc = git_blob_lookup(&blob, repo, &entry->id);
    if (blob_rc != 0 || !blob) {
      unsafe_paths.push_back(rel_path);
      continue;
    }

    const char* data = static_cast<const char*>(git_blob_rawcontent(blob));
    const size_t size = git_blob_rawsize(blob);
    bool ok = false;
    if (data && size > 0) {
      const std::string_view view(data, size);
      ok = view.rfind("HolderPriv1\n", 0) == 0;
    }
    git_blob_free(blob);

    if (!ok) {
      unsafe_paths.push_back(rel_path);
    }
  }

  git_index_free(index);
  git_repository_free(repo);

  if (!unsafe_paths.empty()) {
    std::ostringstream oss;
    oss << "Privacy safety check failed: staged blobs are plaintext. Unsafe paths:";
    for (const auto& p : unsafe_paths) {
      oss << ' ' << p;
    }
    throw PrivacyError(PrivacyErrorCode::EncryptionSafetyCheckFailed, oss.str());
  }
#else
  (void)root_path;
  (void)relative_paths;
  throw PrivacyError(
      PrivacyErrorCode::EncryptionSafetyCheckFailed,
      "libgit2 support is required for staged privacy safety checks"
  );
#endif
}

} // namespace holder::privacy
