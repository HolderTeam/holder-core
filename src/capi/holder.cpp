#include "holder/holder.h"

#include "card/CardRepo.h"
#include "card/CardStore.h"
#include "git/EcdsaDerSigningCredentialProvider.h"
#include "git/GitOps.h"
#include "git/RepoSyncMetrics.h"
#include "index/FtsIndexer.h"
#include "index/Reindexer.h"
#include "model/ProjectSyncState.h"
#include "platform/Db.h"
#include "privacy/PlatformKeyring.h"
#include "privacy/ProjectPrivacy.h"
#include "project/DefaultProject.h"
#include "project/ProjectRepo.h"
#include "project/ProjectStore.h"
#include "project/ProjectSyncRepo.h"
#include "sync/ProjectSyncPolicy.h"

#include <git2.h>
#include <nlohmann/json.hpp>
#include <sodium.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

struct holder_context {
  std::filesystem::path data_dir;
  std::filesystem::path db_path;
  holder::platform::Db db;
  holder::index::FtsIndexer fts{db};
  std::shared_ptr<holder::git::GitCredentialProvider> credential_provider; // nullable
};

struct holder_error {
  std::string message;
};

namespace {

void clear_error(holder_error** out_error) {
  if (out_error != nullptr) {
    *out_error = nullptr;
  }
}

int set_error(holder_error** out_error, int code, std::string message) {
  if (out_error != nullptr) {
    try {
      *out_error = new holder_error{std::move(message)};
    } catch (...) {
      *out_error = nullptr;
      return HOLDER_ERROR_ALLOCATION;
    }
  }
  return code;
}

int set_exception(holder_error** out_error, const std::exception& e) {
  return set_error(out_error, HOLDER_ERROR_RUNTIME, e.what());
}

int set_unknown_exception(holder_error** out_error) {
  return set_error(out_error, HOLDER_ERROR_RUNTIME, "unknown holder error");
}

char* duplicate_string(const std::string& value) {
  auto* out = static_cast<char*>(std::malloc(value.size() + 1));
  if (out == nullptr) {
    return nullptr;
  }
  std::memcpy(out, value.c_str(), value.size() + 1);
  return out;
}

nlohmann::json project_to_json(const holder::model::Project& project) {
  nlohmann::json body = {
      {"project_id", project.project_id},
      {"name", project.name},
      {"root_path", project.root_path},
      {"privacy_mode", project.privacy_mode},
      {"created_at", project.created_at},
      {"updated_at", project.updated_at},
  };

  body["git_remote_url"] = project.git_remote_url.has_value()
                               ? nlohmann::json(*project.git_remote_url)
                               : nlohmann::json(nullptr);
  body["git_provider"] = project.git_provider.has_value()
                             ? nlohmann::json(*project.git_provider)
                             : nlohmann::json(nullptr);
  body["project_key_id"] = project.project_key_id.has_value()
                               ? nlohmann::json(*project.project_key_id)
                               : nlohmann::json(nullptr);
  return body;
}

nlohmann::json card_to_json(const holder::model::Card& card) {
  nlohmann::json body = {
      {"card_id", card.card_id},
      {"project_id", card.project_id},
      {"title", card.title},
      {"rel_path", card.rel_path},
      {"sort_key", card.sort_key},
      {"created_at", card.created_at},
      {"updated_at", card.updated_at},
  };

  body["parent_card_id"] = card.parent_card_id.has_value()
                               ? nlohmann::json(*card.parent_card_id)
                               : nlohmann::json(nullptr);
  body["deleted_at"] = card.deleted_at.has_value()
                           ? nlohmann::json(*card.deleted_at)
                           : nlohmann::json(nullptr);
  return body;
}

nlohmann::json search_row_to_json(const holder::index::FtsIndexer::SearchRow& row) {
  return {
      {"card_id", row.id},
      {"title", row.title},
      {"snippet", row.snippet},
      {"rank", row.rank},
      {"created_at", row.created_at},
      {"updated_at", row.updated_at},
  };
}

std::unique_ptr<holder::git::RealGitOps> open_project_git(
    holder_context* context,
    const holder::model::Project& project
) {
  auto git = std::make_unique<holder::git::RealGitOps>();
  if (context->credential_provider) {
    git->set_credential_provider(context->credential_provider);
  }
  git->open_or_init(project.root_path);
  return git;
}

nlohmann::json optional_json(const std::optional<std::string>& value) {
  return value.has_value() ? nlohmann::json(*value) : nlohmann::json(nullptr);
}

nlohmann::json optional_json(const std::optional<long long>& value) {
  return value.has_value() ? nlohmann::json(*value) : nlohmann::json(nullptr);
}

nlohmann::json project_sync_to_json(const std::optional<holder::model::ProjectSyncState>& sync) {
  if (!sync.has_value()) {
    return {
        {"last_commit_at", nullptr},
        {"last_push_at", nullptr},
        {"last_pull_at", nullptr},
        {"uncommitted_changes_count", 0},
        {"unpushed_commits_count", 0},
        {"last_push_status", nullptr},
        {"last_pull_status", nullptr},
        {"last_sync_error", nullptr},
        {"last_sync_error_at", nullptr},
        {"retry_count", 0},
        {"next_retry_at", nullptr},
        {"pull_retry_count", 0},
        {"next_pull_retry_at", nullptr},
        {"updated_at", nullptr},
    };
  }
  return {
      {"last_commit_at", optional_json(sync->last_commit_at)},
      {"last_push_at", optional_json(sync->last_push_at)},
      {"last_pull_at", optional_json(sync->last_pull_at)},
      {"uncommitted_changes_count", sync->uncommitted_changes_count},
      {"unpushed_commits_count", sync->unpushed_commits_count},
      {"last_push_status", optional_json(sync->last_push_status)},
      {"last_pull_status", optional_json(sync->last_pull_status)},
      {"last_sync_error", optional_json(sync->last_sync_error)},
      {"last_sync_error_at", optional_json(sync->last_sync_error_at)},
      {"retry_count", sync->retry_count},
      {"next_retry_at", optional_json(sync->next_retry_at)},
      {"pull_retry_count", sync->pull_retry_count},
      {"next_pull_retry_at", optional_json(sync->next_pull_retry_at)},
      {"updated_at", sync->updated_at > 0 ? nlohmann::json(sync->updated_at) : nlohmann::json(nullptr)},
  };
}

void refresh_sync_activity_counts(
    holder::platform::Db& db,
    const std::string& project_id,
    const std::filesystem::path& root_path,
    long long now
) {
  const auto metrics = holder::git::inspect_repo_sync_metrics(root_path, "origin");
  holder::project::ProjectSyncRepo(db).update_activity_counts(
      project_id,
      {.uncommitted_changes_count = metrics.uncommitted_changes_count,
       .unpushed_commits_count = metrics.unpushed_commits_count,
       .updated_at = now}
  );
}

// Owns the C-ABI signer's user_data/destroy_user_data pair for exactly as
// long as some EcdsaDerSigningCredentialProvider's captured lambda keeps it
// alive -- see holder_git_set_ssh_signer's ownership contract in holder.h.
class CApiSshSignerHandle {
 public:
  CApiSshSignerHandle(void* user_data, holder_destroy_fn destroy_user_data)
      : user_data_(user_data),
        destroy_user_data_(destroy_user_data) {}

  ~CApiSshSignerHandle() {
    if (destroy_user_data_ != nullptr) {
      destroy_user_data_(user_data_);
    }
  }

  CApiSshSignerHandle(const CApiSshSignerHandle&) = delete;
  CApiSshSignerHandle& operator=(const CApiSshSignerHandle&) = delete;

  std::vector<unsigned char> sign(
      holder_ssh_sign_fn sign_fn,
      const unsigned char* data,
      size_t data_len
  ) const {
    unsigned char* out_ptr = nullptr;
    size_t out_len = 0;
    const int rc = sign_fn(user_data_, data, data_len, &out_ptr, &out_len);
    if (rc != 0 || out_ptr == nullptr) return {};

    std::vector<unsigned char> result(out_ptr, out_ptr + out_len);
    std::free(out_ptr);
    return result;
  }

 private:
  void* user_data_;
  holder_destroy_fn destroy_user_data_;
};

int keyring_kind_to_int(holder::privacy::PlatformKeyringSecretKind kind) {
  return kind == holder::privacy::PlatformKeyringSecretKind::ProjectKey ? 1 : 0;
}

// Owns the C-ABI keyring provider's user_data/destroy_user_data pair for exactly
// as long as the external provider it's installed into (via
// platform_keyring_set_external_provider) is alive -- see
// holder_keyring_set_provider's ownership contract in holder.h.
class CApiKeyringProviderHandle {
 public:
  CApiKeyringProviderHandle(void* user_data, holder_destroy_fn destroy_user_data)
      : user_data_(user_data),
        destroy_user_data_(destroy_user_data) {}

  ~CApiKeyringProviderHandle() {
    if (destroy_user_data_ != nullptr) {
      destroy_user_data_(user_data_);
    }
  }

  CApiKeyringProviderHandle(const CApiKeyringProviderHandle&) = delete;
  CApiKeyringProviderHandle& operator=(const CApiKeyringProviderHandle&) = delete;

  holder::privacy::PlatformKeyringLookupResult lookup(
      holder_keyring_lookup_fn lookup_fn,
      const holder::privacy::PlatformKeyringSecretRef& ref
  ) const {
    int found = 0;
    char* secret_ptr = nullptr;
    char* error_ptr = nullptr;
    const int rc = lookup_fn(
        user_data_,
        keyring_kind_to_int(ref.kind),
        ref.service.c_str(),
        ref.account.c_str(),
        ref.project_id.has_value() ? ref.project_id->c_str() : nullptr,
        &found,
        &secret_ptr,
        &error_ptr
    );

    holder::privacy::PlatformKeyringLookupResult result;
    if (rc != 0) {
      result.error_message = error_ptr != nullptr ? std::string(error_ptr) : std::string("keyring lookup failed");
    } else if (found != 0 && secret_ptr != nullptr) {
      result.secret = std::string(secret_ptr);
    }
    if (secret_ptr != nullptr) std::free(secret_ptr);
    if (error_ptr != nullptr) std::free(error_ptr);
    return result;
  }

  std::optional<std::string> store(
      holder_keyring_store_fn store_fn,
      const holder::privacy::PlatformKeyringSecretRef& ref,
      const std::string& label,
      const std::string& secret
  ) const {
    char* error_ptr = nullptr;
    const int rc = store_fn(
        user_data_,
        keyring_kind_to_int(ref.kind),
        ref.service.c_str(),
        ref.account.c_str(),
        ref.project_id.has_value() ? ref.project_id->c_str() : nullptr,
        label.c_str(),
        secret.c_str(),
        &error_ptr
    );

    std::optional<std::string> result;
    if (rc != 0) {
      result = error_ptr != nullptr ? std::string(error_ptr) : std::string("keyring store failed");
    }
    if (error_ptr != nullptr) std::free(error_ptr);
    return result;
  }

  std::optional<std::string> remove(
      holder_keyring_remove_fn remove_fn,
      const holder::privacy::PlatformKeyringSecretRef& ref
  ) const {
    char* error_ptr = nullptr;
    const int rc = remove_fn(
        user_data_,
        keyring_kind_to_int(ref.kind),
        ref.service.c_str(),
        ref.account.c_str(),
        ref.project_id.has_value() ? ref.project_id->c_str() : nullptr,
        &error_ptr
    );

    std::optional<std::string> result;
    if (rc != 0) {
      result = error_ptr != nullptr ? std::string(error_ptr) : std::string("keyring remove failed");
    }
    if (error_ptr != nullptr) std::free(error_ptr);
    return result;
  }

 private:
  void* user_data_;
  holder_destroy_fn destroy_user_data_;
};

long long now_epoch_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch()
  )
      .count();
}

std::string uuid_v4() {
  if (sodium_init() < 0) {
    throw std::runtime_error("failed to initialize libsodium"); // LCOV_EXCL_LINE
  }
  unsigned char bytes[16];
  randombytes_buf(bytes, sizeof(bytes));
  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40); // version 4
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80); // variant 10xx

  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(36);
  for (int i = 0; i < 16; ++i) {
    out.push_back(kHex[bytes[i] >> 4]);
    out.push_back(kHex[bytes[i] & 0x0F]);
    if (i == 3 || i == 5 || i == 7 || i == 9) {
      out.push_back('-');
    }
  }
  return out;
}

} // namespace

int holder_context_open(
    const char* data_dir,
    const char* schema_sql,
    holder_context** out_context,
    holder_error** out_error
) {
  clear_error(out_error);
  if (out_context == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "out_context must not be null");
  }
  *out_context = nullptr;

  if (data_dir == nullptr || data_dir[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "data_dir must not be empty");
  }

  try {
    auto context = std::make_unique<holder_context>();
    context->data_dir = std::filesystem::path(data_dir);
    context->db_path = context->data_dir / "server" / "holder.db";
    std::filesystem::create_directories(context->db_path.parent_path());
    context->db.open(context->db_path);
    if (schema_sql != nullptr && schema_sql[0] != '\0') {
      context->db.exec(schema_sql);
    }

    *out_context = context.release();
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);
  }
}

void holder_context_destroy(holder_context* context) {
  delete context;
}

int holder_project_list(holder_context* context, char** out_json, holder_error** out_error) {
  clear_error(out_error);
  if (out_json == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "out_json must not be null");
  }
  *out_json = nullptr;

  if (context == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "context must not be null");
  }

  try {
    holder::project::ProjectRepo repo(context->db);
    nlohmann::json body = nlohmann::json::array();
    for (const auto& project : repo.list()) {
      body.push_back(project_to_json(project));
    }

    auto* out = duplicate_string(body.dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);
  }
}

int holder_card_list(
    holder_context* context,
    const char* project_id,
    char** out_json,
    holder_error** out_error
) {
  clear_error(out_error);
  if (out_json == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "out_json must not be null");
  }
  *out_json = nullptr;

  if (context == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "context must not be null");
  }
  if (project_id == nullptr || project_id[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "project_id must not be empty");
  }

  try {
    holder::card::CardRepo repo(context->db);
    nlohmann::json body = nlohmann::json::array();
    for (const auto& card : repo.list_all(project_id)) {
      if (card.deleted_at.has_value()) {
        continue;
      }
      body.push_back(card_to_json(card));
    }

    auto* out = duplicate_string(body.dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);
  }
}

int holder_card_get_content(
    holder_context* context,
    const char* card_id,
    char** out_content,
    holder_error** out_error
) {
  clear_error(out_error);
  if (out_content == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "out_content must not be null");
  }
  *out_content = nullptr;

  if (context == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "context must not be null");
  }
  if (card_id == nullptr || card_id[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "card_id must not be empty");
  }

  try {
    holder::card::CardRepo repo(context->db);
    const auto card = repo.get(card_id);
    if (!card.has_value()) {
      return set_error(out_error, HOLDER_ERROR_RUNTIME, "card not found: " + std::string(card_id));
    }

    holder::card::CardStore store(context->db, &context->fts);
    const auto content = store.get_content(*card);
    if (!content.has_value()) {
      return set_error(out_error, HOLDER_ERROR_RUNTIME, "card content missing");
    }

    auto* out = duplicate_string(*content);
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
    }

    *out_content = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);
  }
}

int holder_project_create(
    holder_context* context,
    const char* name,
    const char* root_path,
    const char* privacy_mode,
    char** out_json,
    holder_error** out_error
) {
  clear_error(out_error);
  if (out_json == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "out_json must not be null");
  }
  *out_json = nullptr;

  if (context == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "context must not be null");
  }
  if (name == nullptr || name[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "name must not be empty");
  }

  try {
    holder::model::Project project;
    project.name = name;
    if (root_path != nullptr && root_path[0] != '\0') {
      project.root_path = root_path;
    }
    project.privacy_mode =
        (privacy_mode != nullptr && privacy_mode[0] != '\0') ? std::string(privacy_mode) : "plain";

    holder::project::ProjectStore store(context->db);
    const auto created =
        store.create(std::move(project), uuid_v4, context->data_dir / "projects");

    auto* out = duplicate_string(project_to_json(created).dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);
  }
}

int holder_project_rename(
    holder_context* context,
    const char* project_id,
    const char* name,
    char** out_json,
    holder_error** out_error
) {
  clear_error(out_error);
  if (out_json == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "out_json must not be null");
  }
  *out_json = nullptr;

  if (context == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "context must not be null");
  }
  if (project_id == nullptr || project_id[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "project_id must not be empty");
  }
  if (name == nullptr || name[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "name must not be empty");
  }

  try {
    holder::project::ProjectRepo repo(context->db);
    if (!repo.get(project_id).has_value()) {
      return set_error(out_error, HOLDER_ERROR_RUNTIME, "project not found: " + std::string(project_id));
    }

    repo.update_name(project_id, name, now_epoch_seconds());

    auto* out = duplicate_string(project_to_json(repo.get(project_id).value()).dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);
  }
}

int holder_project_delete(holder_context* context, const char* project_id, holder_error** out_error) {
  clear_error(out_error);
  if (context == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "context must not be null");
  }
  if (project_id == nullptr || project_id[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "project_id must not be empty");
  }

  try {
    holder::project::ProjectRepo repo(context->db);
    if (!repo.get(project_id).has_value()) {
      return set_error(out_error, HOLDER_ERROR_RUNTIME, "project not found: " + std::string(project_id));
    }

    repo.remove(project_id);
    holder::project::ProjectSyncRepo(context->db).remove(project_id);
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);
  }
}

int holder_card_create(
    holder_context* context,
    const char* project_id,
    const char* title,
    const char* content,
    const char* parent_card_id,
    char** out_json,
    holder_error** out_error
) {
  clear_error(out_error);
  if (out_json == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "out_json must not be null");
  }
  *out_json = nullptr;

  if (context == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "context must not be null");
  }
  if (project_id == nullptr || project_id[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "project_id must not be empty");
  }
  if (title == nullptr || title[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "title must not be empty");
  }

  try {
    holder::model::Card card;
    card.card_id = uuid_v4();
    card.project_id = project_id;
    card.title = title;
    card.created_at = now_epoch_seconds();
    card.updated_at = card.created_at;
    if (parent_card_id != nullptr && parent_card_id[0] != '\0') {
      card.parent_card_id = std::string(parent_card_id);
    }

    holder::card::CardStore store(context->db, &context->fts);
    store.create(card, content != nullptr ? std::string(content) : std::string());

    holder::card::CardRepo repo(context->db);
    const auto created = repo.get(card.card_id);
    auto* out = duplicate_string(card_to_json(created.value()).dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);
  }
}

int holder_card_update_content(
    holder_context* context,
    const char* card_id,
    const char* content,
    const char* title,
    char** out_json,
    holder_error** out_error
) {
  clear_error(out_error);
  if (out_json == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "out_json must not be null");
  }
  *out_json = nullptr;

  if (context == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "context must not be null");
  }
  if (card_id == nullptr || card_id[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "card_id must not be empty");
  }
  if (content == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "content must not be null");
  }

  try {
    holder::card::CardStore store(context->db, &context->fts);
    const std::optional<std::string> title_opt =
        (title != nullptr && title[0] != '\0') ? std::optional<std::string>(title) : std::nullopt;
    store.update_content(card_id, content, title_opt, now_epoch_seconds());

    holder::card::CardRepo repo(context->db);
    const auto updated = repo.get(card_id);
    if (!updated.has_value()) {
      return set_error(out_error, HOLDER_ERROR_RUNTIME, "card not found: " + std::string(card_id));
    }

    auto* out = duplicate_string(card_to_json(updated.value()).dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);
  }
}

int holder_card_delete(holder_context* context, const char* card_id, holder_error** out_error) {
  clear_error(out_error);
  if (context == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "context must not be null");
  }
  if (card_id == nullptr || card_id[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "card_id must not be empty");
  }

  try {
    holder::card::CardStore store(context->db, &context->fts);
    store.trash(card_id, now_epoch_seconds());
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);
  }
}

int holder_card_search(
    holder_context* context,
    const char* project_id,
    const char* query,
    int limit,
    int offset,
    char** out_json,
    holder_error** out_error
) {
  clear_error(out_error);
  if (out_json == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "out_json must not be null");
  }
  *out_json = nullptr;

  if (context == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "context must not be null");
  }
  if (project_id == nullptr || project_id[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "project_id must not be empty");
  }
  if (query == nullptr || query[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "query must not be empty");
  }

  try {
    const auto rows = context->fts.search_cards(project_id, query, limit, offset);
    nlohmann::json body = nlohmann::json::array();
    for (const auto& row : rows) {
      body.push_back(search_row_to_json(row));
    }

    auto* out = duplicate_string(body.dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);
  }
}

int holder_reindex(holder_context* context, holder_error** out_error) {
  clear_error(out_error);
  if (context == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "context must not be null");
  }

  try {
    holder::index::Reindexer reindexer(context->db);
    reindexer.run();
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);
  }
}

int holder_ensure_default_project(
    holder_context* context,
    const char* name,
    const char* welcome_title,
    const char* welcome_content,
    char** out_json,
    holder_error** out_error
) {
  clear_error(out_error);
  if (out_json == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "out_json must not be null");
  }
  *out_json = nullptr;

  if (context == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "context must not be null");
  }
  if (name == nullptr || name[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "name must not be empty");
  }
  if (welcome_title == nullptr || welcome_title[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "welcome_title must not be empty");
  }

  try {
    const auto created = holder::project::ensure_default_project(
        context->db,
        name,
        "plain",
        welcome_title,
        welcome_content != nullptr ? std::string(welcome_content) : std::string(),
        uuid_v4,
        context->data_dir / "projects",
        &context->fts
    );

    auto* out = duplicate_string(
        created.has_value() ? project_to_json(created.value()).dump() : std::string("null")
    );
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);
  }
}

int holder_git_set_homedir(const char* path, holder_error** out_error) {
  clear_error(out_error);
  if (path == nullptr || path[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "path must not be empty");
  }

  // libgit2 only runs its real subsystem init (including deriving the default
  // homedir) on the process's first git_libgit2_init() 0->1 transition; later
  // calls (e.g. from each GitRepo constructed per git operation) just bump a
  // refcount. Force that first transition to happen here, before setting our
  // override, so it isn't silently clobbered the next time a GitRepo is
  // constructed. Deliberately never shut this reference back down: it must
  // outlive every GitRepo for the rest of the process, or the override would
  // be reset back to defaults whenever the refcount happened to hit zero.
  git_libgit2_init();

  const int rc = git_libgit2_opts(GIT_OPT_SET_HOMEDIR, path);
  if (rc != 0) {
    const git_error* e = git_error_last();
    return set_error(
        out_error,
        HOLDER_ERROR_RUNTIME,
        std::string("git_libgit2_opts(GIT_OPT_SET_HOMEDIR) failed: ") +
            (e && e->message ? e->message : "unknown error")
    );
  }
  return HOLDER_OK;
}

int holder_git_set_ssh_signer(
    holder_context* context,
    const char* username,
    const unsigned char* public_key_blob,
    size_t public_key_blob_len,
    holder_ssh_sign_fn sign_fn,
    void* user_data,
    holder_destroy_fn destroy_user_data,
    holder_error** out_error
) {
  clear_error(out_error);

  // Takes ownership of user_data unconditionally from this point on -- via
  // this shared_ptr, which either gets captured into the installed provider
  // below (success) or simply falls out of scope at any early return
  // (failure) -- so destroy_user_data runs exactly once either way: right
  // away if this call fails for any reason, or later (on replacement or
  // context destruction) if it succeeds. Callers never need to guess which
  // case applies or risk a double-release.
  std::shared_ptr<CApiSshSignerHandle> handle;
  try {
    handle = std::make_shared<CApiSshSignerHandle>(user_data, destroy_user_data);
  } catch (const std::bad_alloc&) {
    // user_data was never captured; nothing to release.
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  }

  if (context == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "context must not be null");
  }
  if (username == nullptr || username[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "username must not be empty");
  }
  if (public_key_blob == nullptr || public_key_blob_len == 0) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "public_key_blob must not be empty");
  }
  if (sign_fn == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "sign_fn must not be null");
  }

  try {
    std::vector<unsigned char> pubkey(public_key_blob, public_key_blob + public_key_blob_len);

    context->credential_provider = std::make_shared<holder::git::EcdsaDerSigningCredentialProvider>(
        std::string(username),
        std::move(pubkey),
        [handle, sign_fn](const unsigned char* data, size_t data_len) {
          return handle->sign(sign_fn, data, data_len);
        }
    );
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);
  }
}

int holder_project_update_git_remote(
    holder_context* context,
    const char* project_id,
    const char* remote_url,
    char** out_json,
    holder_error** out_error
) {
  clear_error(out_error);
  if (out_json == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "out_json must not be null");
  }
  *out_json = nullptr;

  if (context == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "context must not be null");
  }
  if (project_id == nullptr || project_id[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "project_id must not be empty");
  }

  try {
    holder::project::ProjectRepo repo(context->db);
    if (!repo.get(project_id).has_value()) {
      return set_error(out_error, HOLDER_ERROR_RUNTIME, "project not found: " + std::string(project_id));
    }

    const std::optional<std::string> url =
        (remote_url != nullptr && remote_url[0] != '\0') ? std::optional<std::string>(remote_url)
                                                          : std::nullopt;
    repo.update_git_remote(project_id, url, now_epoch_seconds());

    auto* out = duplicate_string(project_to_json(repo.get(project_id).value()).dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);
  }
}

int holder_git_test_remote(
    holder_context* context,
    const char* project_id,
    const char* branch,
    char** out_json,
    holder_error** out_error
) {
  clear_error(out_error);
  if (out_json == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "out_json must not be null");
  }
  *out_json = nullptr;

  if (context == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "context must not be null");
  }
  if (project_id == nullptr || project_id[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "project_id must not be empty");
  }

  try {
    holder::project::ProjectRepo repo(context->db);
    const auto project_opt = repo.get(project_id);
    if (!project_opt.has_value()) {
      return set_error(out_error, HOLDER_ERROR_RUNTIME, "project not found: " + std::string(project_id));
    }
    const auto& project = project_opt.value();
    const std::string resolved_branch =
        (branch != nullptr && branch[0] != '\0') ? std::string(branch) : "local_default";

    nlohmann::json body = {
        {"project_id", project_id},
        {"remote_url", optional_json(project.git_remote_url)},
        {"branch", resolved_branch},
    };

    if (!project.git_remote_url.has_value() || project.git_remote_url->empty()) {
      body["status"] = holder::git::remote_probe_status_name(holder::git::RemoteProbeStatus::RemoteUnset);
      body["remote_has_head"] = false;
      body["error_message"] = "Remote URL is not configured.";
    } else {
      auto git = open_project_git(context, project);
      git->set_remote("origin", project.git_remote_url.value());
      const auto probe = git->probe_remote("origin");
      body["status"] = holder::git::remote_probe_status_name(probe.status);
      body["remote_has_head"] = probe.remote_has_head;
      body["error_message"] = probe.error_message.empty() ? nlohmann::json(nullptr)
                                                           : nlohmann::json(probe.error_message);
    }

    auto* out = duplicate_string(body.dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
    }
    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);
  }
}

int holder_git_push(
    holder_context* context,
    const char* project_id,
    const char* branch,
    int set_upstream,
    char** out_json,
    holder_error** out_error
) {
  clear_error(out_error);
  if (out_json == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "out_json must not be null");
  }
  *out_json = nullptr;

  if (context == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "context must not be null");
  }
  if (project_id == nullptr || project_id[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "project_id must not be empty");
  }

  try {
    holder::project::ProjectRepo repo(context->db);
    holder::project::ProjectSyncRepo sync_repo(context->db);
    const auto project_opt = repo.get(project_id);
    if (!project_opt.has_value()) {
      return set_error(out_error, HOLDER_ERROR_RUNTIME, "project not found: " + std::string(project_id));
    }
    const auto& project = project_opt.value();
    const std::string requested_branch = (branch != nullptr) ? std::string(branch) : std::string();
    const std::string resolved_branch = requested_branch.empty() ? "local_default" : requested_branch;
    const auto now = now_epoch_seconds();

    nlohmann::json body = {
        {"project_id", project_id},
        {"remote_url", optional_json(project.git_remote_url)},
        {"branch", resolved_branch},
    };

    if (!project.git_remote_url.has_value() || project.git_remote_url->empty()) {
      sync_repo.record_push_result(
          project_id,
          holder::git::push_status_name(holder::git::PushStatus::RemoteUnset),
          false,
          std::optional<std::string>{"Remote URL is not configured."},
          now
      );
      body["status"] = holder::git::push_status_name(holder::git::PushStatus::RemoteUnset);
      body["ahead_count"] = 0;
      body["behind_count"] = 0;
      body["local_head_commit"] = nullptr;
      body["error_message"] = "Remote URL is not configured.";
    } else {
      auto git = open_project_git(context, project);
      git->set_remote("origin", project.git_remote_url.value());
      const auto push = git->push_branch("origin", requested_branch, set_upstream != 0);
      const bool push_ok = push.status == holder::git::PushStatus::Pushed ||
                           push.status == holder::git::PushStatus::UpToDate;
      sync_repo.record_push_result(
          project_id,
          holder::git::push_status_name(push.status),
          push_ok,
          push.error_message.empty() ? std::optional<std::string>() : std::optional<std::string>(push.error_message),
          now
      );
      try {
        refresh_sync_activity_counts(context->db, project_id, project.root_path, now);
      } catch (const std::exception&) {
        // Best-effort only; metrics refresh failure does not fail push response.
      }

      body["status"] = holder::git::push_status_name(push.status);
      body["ahead_count"] = push.ahead_count;
      body["behind_count"] = push.behind_count;
      body["local_head_commit"] =
          push.local_head_commit.empty() ? nlohmann::json(nullptr) : nlohmann::json(push.local_head_commit);
      body["error_message"] =
          push.error_message.empty() ? nlohmann::json(nullptr) : nlohmann::json(push.error_message);
    }

    auto* out = duplicate_string(body.dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
    }
    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);
  }
}

int holder_git_pull(
    holder_context* context,
    const char* project_id,
    char** out_json,
    holder_error** out_error
) {
  clear_error(out_error);
  if (out_json == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "out_json must not be null");
  }
  *out_json = nullptr;

  if (context == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "context must not be null");
  }
  if (project_id == nullptr || project_id[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "project_id must not be empty");
  }

  try {
    holder::project::ProjectRepo repo(context->db);
    holder::project::ProjectSyncRepo sync_repo(context->db);
    const auto project_opt = repo.get(project_id);
    if (!project_opt.has_value()) {
      return set_error(out_error, HOLDER_ERROR_RUNTIME, "project not found: " + std::string(project_id));
    }
    const auto& project = project_opt.value();
    const auto now = now_epoch_seconds();

    nlohmann::json body = {{"project_id", project_id}};

    if (!project.git_remote_url.has_value() || project.git_remote_url->empty()) {
      sync_repo.record_pull_result(
          project_id,
          "failed",
          false,
          std::optional<std::string>{"Remote URL is not configured."},
          now
      );
      body["status"] = "failed";
      body["error_message"] = "Remote URL is not configured.";
    } else {
      auto git = open_project_git(context, project);
      git->set_remote("origin", project.git_remote_url.value());
      try {
        git->pull_remote_ff_only("origin");
        sync_repo.record_pull_result(project_id, "succeeded", true, std::nullopt, now);
        body["status"] = "succeeded";
        body["error_message"] = nullptr;
      } catch (const std::exception& e) {
        sync_repo.record_pull_result(project_id, "failed", false, std::optional<std::string>(e.what()), now);
        body["status"] = "failed";
        body["error_message"] = e.what();
      }
      try {
        refresh_sync_activity_counts(context->db, project_id, project.root_path, now);
      } catch (const std::exception&) {
        // Best-effort only; metrics refresh failure does not fail pull response.
      }
    }

    auto* out = duplicate_string(body.dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
    }
    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);
  }
}

int holder_git_sync_status(
    holder_context* context,
    const char* project_id,
    char** out_json,
    holder_error** out_error
) {
  clear_error(out_error);
  if (out_json == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "out_json must not be null");
  }
  *out_json = nullptr;

  if (context == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "context must not be null");
  }
  if (project_id == nullptr || project_id[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "project_id must not be empty");
  }

  try {
    holder::project::ProjectRepo repo(context->db);
    if (!repo.get(project_id).has_value()) {
      return set_error(out_error, HOLDER_ERROR_RUNTIME, "project not found: " + std::string(project_id));
    }

    holder::project::ProjectSyncRepo sync_repo(context->db);
    nlohmann::json body = {
        {"project_id", project_id},
        {"sync", project_sync_to_json(sync_repo.get(project_id))},
    };

    auto* out = duplicate_string(body.dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
    }
    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);
  }
}

int holder_git_sync_if_due(
    holder_context* context,
    const char* project_id,
    int push_interval_seconds,
    int pull_interval_seconds,
    char** out_json,
    holder_error** out_error
) {
  clear_error(out_error);
  if (out_json == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "out_json must not be null");
  }
  *out_json = nullptr;

  if (context == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "context must not be null");
  }
  if (project_id == nullptr || project_id[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "project_id must not be empty");
  }

  try {
    holder::project::ProjectRepo repo(context->db);
    holder::project::ProjectSyncRepo sync_repo(context->db);
    const auto project_opt = repo.get(project_id);
    if (!project_opt.has_value()) {
      return set_error(out_error, HOLDER_ERROR_RUNTIME, "project not found: " + std::string(project_id));
    }
    const auto& project = project_opt.value();

    nlohmann::json body = {
        {"project_id", project_id},
        {"pull_attempted", false},
        {"pull_status", nullptr},
        {"pull_error", nullptr},
        {"push_attempted", false},
        {"push_status", nullptr},
        {"push_error", nullptr},
    };

    if (!project.git_remote_url.has_value() || project.git_remote_url->empty()) {
      auto* out = duplicate_string(body.dump());
      if (out == nullptr) {
        return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
      }
      *out_json = out;
      return HOLDER_OK;
    }

    const auto now = now_epoch_seconds();
    auto git = open_project_git(context, project);
    git->set_remote("origin", project.git_remote_url.value());

    const auto state = sync_repo.get(project_id);
    holder::sync::PullDecisionInput pull_input{
        .last_pull_at = state.has_value() ? state->last_pull_at : std::optional<long long>{},
        .next_pull_retry_at = state.has_value() ? state->next_pull_retry_at : std::optional<long long>{},
        .now = now,
    };
    if (pull_interval_seconds > 0) pull_input.pull_interval_seconds = pull_interval_seconds;

    if (holder::sync::should_attempt_pull(pull_input)) {
      body["pull_attempted"] = true;
      try {
        git->pull_remote_ff_only("origin");
        sync_repo.record_pull_result(project_id, "succeeded", true, std::nullopt, now);
        body["pull_status"] = "succeeded";
      } catch (const std::exception& e) {
        sync_repo.record_pull_result(
            project_id,
            "failed",
            false,
            std::optional<std::string>(e.what()),
            now
        );
        body["pull_status"] = "failed";
        body["pull_error"] = std::string(e.what());
      }
      try {
        refresh_sync_activity_counts(context->db, project_id, project.root_path, now);
      } catch (const std::exception&) {
        // Best-effort only; metrics refresh failure does not fail sync_if_due.
      }
    }

    // Re-read rather than reuse `state`: record_pull_result above may have
    // just written this row, and push's decision should see that write even
    // though it only reads push-specific fields.
    const auto state_for_push = sync_repo.get(project_id);
    holder::sync::PushDecisionInput push_input{
        .last_push_at = state_for_push.has_value() ? state_for_push->last_push_at
                                                    : std::optional<long long>{},
        .next_retry_at = state_for_push.has_value() ? state_for_push->next_retry_at
                                                     : std::optional<long long>{},
        .now = now,
    };
    if (push_interval_seconds > 0) push_input.push_interval_seconds = push_interval_seconds;

    if (holder::sync::should_attempt_push(push_input)) {
      body["push_attempted"] = true;
      try {
        if (project.privacy_mode == "encrypted_git") {
          holder::privacy::assert_encryption_push_safe(project.root_path);
        }
        const auto push = git->push_branch("origin", "", true);
        const bool push_ok = push.status == holder::git::PushStatus::Pushed ||
                             push.status == holder::git::PushStatus::UpToDate;
        sync_repo.record_push_result(
            project_id,
            holder::git::push_status_name(push.status),
            push_ok,
            push.error_message.empty() ? std::optional<std::string>()
                                       : std::optional<std::string>(push.error_message),
            now
        );
        body["push_status"] = holder::git::push_status_name(push.status);
        if (!push.error_message.empty()) body["push_error"] = push.error_message;
      } catch (const std::exception& e) {
        sync_repo.record_push_result(
            project_id,
            holder::git::push_status_name(holder::git::PushStatus::UnknownError),
            false,
            std::optional<std::string>(e.what()),
            now
        );
        body["push_status"] = holder::git::push_status_name(holder::git::PushStatus::UnknownError);
        body["push_error"] = std::string(e.what());
      }
      try {
        refresh_sync_activity_counts(context->db, project_id, project.root_path, now);
      } catch (const std::exception&) {
        // Best-effort only; metrics refresh failure does not fail sync_if_due.
      }
    }

    auto* out = duplicate_string(body.dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
    }
    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);
  }
}

int holder_keyring_set_provider(
    holder_keyring_lookup_fn lookup_fn,
    holder_keyring_store_fn store_fn,
    holder_keyring_remove_fn remove_fn,
    void* user_data,
    holder_destroy_fn destroy_user_data,
    holder_error** out_error
) {
  clear_error(out_error);

  // Takes ownership of user_data unconditionally from this point on, exactly
  // like holder_git_set_ssh_signer -- see that function's comment for why.
  std::shared_ptr<CApiKeyringProviderHandle> handle;
  try {
    handle = std::make_shared<CApiKeyringProviderHandle>(user_data, destroy_user_data);
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  }

  if (lookup_fn == nullptr || store_fn == nullptr || remove_fn == nullptr) {
    return set_error(
        out_error,
        HOLDER_ERROR_INVALID_ARGUMENT,
        "lookup_fn, store_fn, and remove_fn must not be null"
    );
  }

  try {
    holder::privacy::platform_keyring_set_external_provider(
        [handle, lookup_fn](const holder::privacy::PlatformKeyringSecretRef& ref) {
          return handle->lookup(lookup_fn, ref);
        },
        [handle, store_fn](
            const holder::privacy::PlatformKeyringSecretRef& ref,
            const std::string& label,
            const std::string& secret
        ) { return handle->store(store_fn, ref, label, secret); },
        [handle, remove_fn](const holder::privacy::PlatformKeyringSecretRef& ref) {
          return handle->remove(remove_fn, ref);
        }
    );
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);
  }
}

void holder_string_free(char* value) {
  std::free(value);
}

const char* holder_error_message(const holder_error* error) {
  if (error == nullptr) {
    return "";
  }
  return error->message.c_str();
}

void holder_error_destroy(holder_error* error) {
  delete error;
}
