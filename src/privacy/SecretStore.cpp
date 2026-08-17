#include "privacy/SecretStore.h"

#include "privacy/CryptoService.h"
#include "privacy/PlatformKeyring.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace holder::privacy {
namespace {

constexpr const char* kMetadataFilename = "secret_store_index.json";
constexpr const char* kFallbackSecretsFilename = "secret_store_fallback.enc";
constexpr const char* kFallbackMasterKeyFilename = "secret_store_master.key";

std::string make_key(const std::string& service, const std::string& account) {
  return service + "\n" + account;
} // LCOV_EXCL_LINE

std::string sanitize_component(std::string value) {
  for (char& ch : value) {
    const unsigned char uch = static_cast<unsigned char>(ch);
    if (!std::isalnum(uch) && ch != '-' && ch != '_' && ch != '.') {
      ch = '_';
    }
  }
  return value;
} // LCOV_EXCL_LINE

std::filesystem::path test_store_root(const std::filesystem::path& server_dir) {
  if (const char* dir = std::getenv("HOLDER_TEST_KEYSTORE_DIR")) {
    return std::filesystem::path(dir);
  }
  return server_dir;
} // LCOV_EXCL_LINE

nlohmann::json read_json_or_empty(const std::filesystem::path& path) {
  if (!std::filesystem::exists(path)) {
    return nlohmann::json::object();
  }

  std::ifstream in(path, std::ios::binary);
  if (!in) {
    // LCOV_EXCL_START
    throw std::runtime_error("failed to open secret metadata file: " + path.string());
    // LCOV_EXCL_STOP
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  if (buffer.str().empty()) {
    return nlohmann::json::object();
  }
  return nlohmann::json::parse(buffer.str());
} // LCOV_EXCL_LINE

void write_json(const std::filesystem::path& path, const nlohmann::json& body) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    // LCOV_EXCL_START
    throw std::runtime_error("failed to write secret metadata file: " + path.string());
    // LCOV_EXCL_STOP
  }
  out << body.dump(2);
}

SecretMetadata metadata_from_json(const nlohmann::json& item) {
  SecretMetadata meta;
  meta.service = item.at("service").get<std::string>();
  meta.account = item.at("account").get<std::string>();
  meta.preview = item.value("preview", std::string());
  meta.created_at = item.value("created_at", 0LL);
  meta.updated_at = item.value("updated_at", 0LL);
  return meta;
} // LCOV_EXCL_LINE

class SecretMetadataIndex {
 public:
  explicit SecretMetadataIndex(std::filesystem::path path)
      : path_(std::move(path)) {}

  std::optional<SecretMetadata> get(const std::string& service, const std::string& account) const {
    const auto body = read_json_or_empty(path_);
    const auto key = make_key(service, account);
    if (!body.contains(key)) {
      return std::nullopt;
    }
    return metadata_from_json(body.at(key));
  }

  std::vector<SecretMetadata> list(const std::string& service) const {
    const auto body = read_json_or_empty(path_);
    std::vector<SecretMetadata> out;
    for (auto it = body.begin(); it != body.end(); ++it) {
      if (!it.value().is_object()) {
        continue;
      }
      auto meta = metadata_from_json(it.value());
      if (meta.service == service) {
        out.push_back(std::move(meta));
      }
    }
    return out;
  }

  void upsert(const SecretMetadata& meta) {
    auto body = read_json_or_empty(path_);
    body[make_key(meta.service, meta.account)] = {
        {"service", meta.service},
        {"account", meta.account},
        {"preview", meta.preview},
        {"created_at", meta.created_at},
        {"updated_at", meta.updated_at},
    };
    write_json(path_, body);
  }

  void remove(const std::string& service, const std::string& account) {
    auto body = read_json_or_empty(path_);
    body.erase(make_key(service, account));
    write_json(path_, body);
  }

 private:
  std::filesystem::path path_;
};

class RawSecretBackend {
 public:
  virtual ~RawSecretBackend() = default;
  virtual std::optional<std::string> get(const std::string& service, const std::string& account)
      const = 0;
  virtual void set(
      const std::string& service,
      const std::string& account,
      const std::string& secret
  ) = 0;
  virtual void remove(const std::string& service, const std::string& account) = 0;
};

class TestDirSecretBackend final : public RawSecretBackend {
 public:
  explicit TestDirSecretBackend(std::filesystem::path dir)
      : dir_(std::move(dir)) {}

  std::optional<std::string> get(const std::string& service, const std::string& account)
      const override {
    const auto path = secret_path(service, account);
    if (!std::filesystem::exists(path)) {
      return std::nullopt;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      // LCOV_EXCL_START
      throw std::runtime_error("failed to open test secret file: " + path.string());
      // LCOV_EXCL_STOP
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
  }

  void set(const std::string& service, const std::string& account, const std::string& secret)
      override {
    std::filesystem::create_directories(dir_);
    std::ofstream out(secret_path(service, account), std::ios::binary | std::ios::trunc);
    if (!out) {
      throw std::runtime_error("failed to write test secret file"); // LCOV_EXCL_LINE
    }
    out << secret;
  }

  void remove(const std::string& service, const std::string& account) override {
    std::error_code ec;
    std::filesystem::remove(secret_path(service, account), ec);
  }

 private:
  std::filesystem::path secret_path(const std::string& service, const std::string& account) const {
    return dir_ / (sanitize_component(service) + "__" + sanitize_component(account) + ".secret");
  }

  std::filesystem::path dir_;
};

class PlatformKeyringSecretBackend final : public RawSecretBackend {
 public:
  std::optional<std::string> get(const std::string& service, const std::string& account)
      const override {
    const auto result = platform_keyring_lookup_secret(PlatformKeyringSecretRef{
        .kind = PlatformKeyringSecretKind::GenericSecret,
        .service = service,
        .account = account,
        .project_id = std::nullopt,
    });
    if (result.error_message.has_value()) {
      throw PrivacyError(PrivacyErrorCode::KeyMaterialMissing, *result.error_message);
    }
    return result.secret;
  }

  void set(const std::string& service, const std::string& account, const std::string& secret)
      override {
    platform_keyring_store_secret(
        PlatformKeyringSecretRef{
            .kind = PlatformKeyringSecretKind::GenericSecret,
            .service = service,
            .account = account,
            .project_id = std::nullopt,
        },
        "Holder secret " + service + "/" + account,
        secret
    );
  }

  void remove(const std::string& service, const std::string& account) override {
    platform_keyring_remove_secret(PlatformKeyringSecretRef{
        .kind = PlatformKeyringSecretKind::GenericSecret,
        .service = service,
        .account = account,
        .project_id = std::nullopt,
    });
  }
};

std::array<unsigned char, kPrivacyKeyBytes> load_or_create_master_key(
    const std::filesystem::path& key_path
) {
  if (std::filesystem::exists(key_path)) {
    std::ifstream in(key_path, std::ios::binary);
    if (!in) {
      // LCOV_EXCL_START
      throw std::runtime_error("failed to open secret-store master key: " + key_path.string());
      // LCOV_EXCL_STOP
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return key_from_base64(buffer.str());
  }

  const auto key = generate_random_key();
  std::filesystem::create_directories(key_path.parent_path());
  std::ofstream out(key_path, std::ios::binary | std::ios::trunc);
  if (!out) {
    // LCOV_EXCL_START
    throw std::runtime_error("failed to write secret-store master key: " + key_path.string());
    // LCOV_EXCL_STOP
  }
  out << key_to_base64(key);
  out.close();
  std::filesystem::permissions(
      key_path,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace
  );
  return key;
}

class EncryptedFileSecretBackend final : public RawSecretBackend {
 public:
  EncryptedFileSecretBackend(std::filesystem::path store_path, std::filesystem::path key_path)
      : store_path_(std::move(store_path)),
        key_path_(std::move(key_path)) {}

  std::optional<std::string> get(const std::string& service, const std::string& account)
      const override {
    const auto map = load_map();
    const auto it = map.find(make_key(service, account));
    if (it == map.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  void set(const std::string& service, const std::string& account, const std::string& secret)
      override {
    auto map = load_map();
    map[make_key(service, account)] = secret;
    save_map(map);
  }

  void remove(const std::string& service, const std::string& account) override {
    auto map = load_map();
    map.erase(make_key(service, account));
    save_map(map);
  }

 private:
  std::unordered_map<std::string, std::string> load_map() const {
    if (!std::filesystem::exists(store_path_)) {
      return {};
    }

    std::ifstream in(store_path_, std::ios::binary);
    if (!in) {
      // LCOV_EXCL_START
      throw PrivacyError(
          PrivacyErrorCode::KeyringUnavailable,
          "failed to open fallback secret file: " + store_path_.string()
      );
      // LCOV_EXCL_STOP
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const auto plaintext = decrypt_envelope_v1(
        buffer.str(),
        load_or_create_master_key(key_path_),
        "holder-secret-store"
    );
    const auto body = nlohmann::json::parse(plaintext);
    std::unordered_map<std::string, std::string> out;
    for (auto it = body.begin(); it != body.end(); ++it) {
      out[it.key()] = it.value().get<std::string>();
    }
    return out;
  }

  void save_map(const std::unordered_map<std::string, std::string>& map) const {
    nlohmann::json body = nlohmann::json::object();
    for (const auto& [key, value] : map) {
      body[key] = value;
    }
    const auto ciphertext = encrypt_envelope_v1(
        body.dump(),
        load_or_create_master_key(key_path_),
        "holder-secret-store"
    );
    std::filesystem::create_directories(store_path_.parent_path());
    std::ofstream out(store_path_, std::ios::binary | std::ios::trunc);
    if (!out) {
      // LCOV_EXCL_START
      throw PrivacyError(
          PrivacyErrorCode::KeyringUnavailable,
          "failed to write fallback secret file: " + store_path_.string()
      );
      // LCOV_EXCL_STOP
    }
    out << ciphertext;
  }

  std::filesystem::path store_path_;
  std::filesystem::path key_path_;
};

class DefaultSecretStore final : public SecretStore {
 public:
  DefaultSecretStore(
      const std::filesystem::path& server_dir,
      std::unique_ptr<RawSecretBackend> backend
  )
      : index_(test_store_root(server_dir) / kMetadataFilename),
        backend_(std::move(backend)) {}

  explicit DefaultSecretStore(const std::filesystem::path& server_dir)
      : index_(test_store_root(server_dir) / kMetadataFilename) {
    if (const char* test_dir = std::getenv("HOLDER_TEST_KEYSTORE_DIR")) {
      backend_ = std::make_unique<TestDirSecretBackend>(std::filesystem::path(test_dir));
      return;
    }
    if (platform_keyring_supported()) {
      backend_ = std::make_unique<PlatformKeyringSecretBackend>();
    } else { // LCOV_EXCL_LINE: exercised by no-platform-keyring builds.
      // LCOV_EXCL_START
      backend_ = std::make_unique<EncryptedFileSecretBackend>(
          server_dir / kFallbackSecretsFilename,
          server_dir / kFallbackMasterKeyFilename
      );
      // LCOV_EXCL_STOP
    }
  } // LCOV_EXCL_LINE

  std::optional<StoredSecret> get(const std::string& service, const std::string& account)
      const override {
    const auto metadata = index_.get(service, account);
    if (!metadata.has_value()) {
      return std::nullopt;
    }
    const auto secret = backend_->get(service, account);
    if (!secret.has_value()) {
      return std::nullopt;
    }
    return StoredSecret{metadata.value(), secret.value()};
  }

  std::vector<SecretMetadata> list(const std::string& service) const override {
    return index_.list(service);
  }

  void set(
      const std::string& service,
      const std::string& account,
      const std::string& secret,
      const std::string& preview,
      long long created_at,
      long long updated_at
  ) override {
    backend_->set(service, account, secret);
    index_.upsert(SecretMetadata{service, account, preview, created_at, updated_at});
  }

  void remove(const std::string& service, const std::string& account) override {
    backend_->remove(service, account);
    index_.remove(service, account);
  }

 private:
  SecretMetadataIndex index_;
  std::unique_ptr<RawSecretBackend> backend_;
};

} // namespace

std::unique_ptr<SecretStore> make_default_secret_store(const std::filesystem::path& server_dir) {
  return std::make_unique<DefaultSecretStore>(server_dir);
}

std::unique_ptr<SecretStore> make_encrypted_file_secret_store_for_tests(
    const std::filesystem::path& server_dir
) {
  return std::make_unique<DefaultSecretStore>(
      server_dir,
      std::make_unique<EncryptedFileSecretBackend>(
          server_dir / kFallbackSecretsFilename,
          server_dir / kFallbackMasterKeyFilename
      )
  );
}

} // namespace holder::privacy
