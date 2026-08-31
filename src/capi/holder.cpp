#include "holder/holder.h"

#include "card/CardRepo.h"
#include "card/CardStore.h"
#include "card/LinkKindCatalog.h"
#include "card/LinkRepo.h"
#include "card/MilestoneRepo.h"
#include "card/TagRepo.h"
#include "git/EcdsaDerSigningCredentialProvider.h"
#include "git/GitOps.h"
#include "git/RepoSyncMetrics.h"
#include "index/FtsIndexer.h"
#include "index/Reindexer.h"
#include "model/ProjectSyncState.h"
#include "platform/Db.h"
#include "platform/DatabaseRebuild.h"
#include "platform/Migrations.h"
#include "privacy/PlatformKeyring.h"
#include "privacy/ProjectPrivacy.h"
#include "project/DefaultProject.h"
#include "project/ProjectManifest.h"
#include "project/ProjectPaths.h"
#include "project/ProjectRepo.h"
#include "project/ProjectStore.h"
#include "project/ProjectSyncRepo.h"
#include "resource/AssetImportService.h"
#include "resource/LocalDirectoryProvider.h"
#include "resource/LocationRepo.h"
#include "resource/LocationStore.h"
#include "resource/ResourceRepo.h"
#include "resource/ResourceStore.h"
#include "resource/StorageProvider.h"
#include "sync/ProjectSyncPolicy.h"
#include "sync/PullConflictResolution.h"

#include <git2.h>
#include <nlohmann/json.hpp>
#include <sodium.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
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

std::filesystem::path rebuild_readiness_path(const std::filesystem::path& data_dir) {
  return data_dir / "server" / "database-rebuild-ready.json";
}

bool looks_like_project(const std::filesystem::path& root) {
  return std::filesystem::is_directory(root) &&
         std::filesystem::is_regular_file(root / ".holder" / "project.json");
}

std::vector<std::filesystem::path> discover_managed_project_roots(
    const std::filesystem::path& data_dir
) {
  std::vector<std::filesystem::path> roots;
  const auto projects_dir = data_dir / "projects";
  if (!std::filesystem::is_directory(projects_dir)) return roots;
  for (const auto& entry : std::filesystem::directory_iterator(projects_dir)) {
    if (looks_like_project(entry.path())) roots.push_back(entry.path());
  }
  return roots;
}

holder::platform::DatabaseRebuildReport rebuild_managed_database(
    const std::filesystem::path& data_dir,
    const std::string& schema_sql,
    bool dry_run
) {
  holder::platform::DatabaseRebuildRequest request;
  request.database_path = data_dir / "server" / "holder.db";
  request.backup_root = data_dir / "server" / "database-backups";
  request.schema_sql = schema_sql;
  request.expected_schema_version = holder::platform::Migrations::latest_schema_version;
  request.project_roots = discover_managed_project_roots(data_dir);
  request.durable_ownership_ready = holder::platform::database_rebuild_is_ready(
      rebuild_readiness_path(data_dir)
  );
  request.dry_run = dry_run;
  auto report = holder::platform::rebuild_database_projection(request);
  if (!dry_run) {
    holder::platform::mark_database_rebuild_ready(rebuild_readiness_path(data_dir));
  }
  return report;
}

void clear_error(holder_error** out_error) {
  if (out_error != nullptr) {
    *out_error = nullptr;
  }
}

int set_error(holder_error** out_error, int code, std::string message) {
  if (out_error != nullptr) {
    try {
      *out_error = new holder_error{std::move(message)};
    // LCOV_EXCL_START
    } catch (...) {
      *out_error = nullptr;
      return HOLDER_ERROR_ALLOCATION;
    }
    // LCOV_EXCL_STOP
  }
  return code;
}

int set_exception(holder_error** out_error, const std::exception& e) {
  return set_error(out_error, HOLDER_ERROR_RUNTIME, e.what());
}

// LCOV_EXCL_START -- all production throw sites use std::exception-derived types; this is the
// C ABI's final defense against a foreign/non-standard exception crossing the boundary.
int set_unknown_exception(holder_error** out_error) {
  return set_error(out_error, HOLDER_ERROR_RUNTIME, "unknown holder error");
}
// LCOV_EXCL_STOP

char* duplicate_string(const std::string& value) {
  auto* out = static_cast<char*>(std::malloc(value.size() + 1));
  if (out == nullptr) {
    return nullptr; // LCOV_EXCL_LINE
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

nlohmann::json placement_to_json(const holder::model::Placement& placement) {
  return {
      {"placement_id", placement.placement_id},
      {"asset_id", placement.asset_id},
      {"location_id", placement.location_id},
      {"object_key", placement.object_key},
      {"encoding", placement.encoding},
      {"stored_byte_size", placement.stored_byte_size},
      {"stored_sha256", placement.stored_sha256},
      {"created_at", placement.created_at},
  };
}

nlohmann::json asset_to_json(const holder::model::Asset& asset) {
  nlohmann::json placements = nlohmann::json::array();
  for (const auto& placement : asset.placements) placements.push_back(placement_to_json(placement));
  return {
      {"asset_id", asset.asset_id},
      {"resource_id", asset.resource_id},
      {"original_filename", asset.original_filename},
      {"media_type", asset.media_type},
      {"byte_size", asset.byte_size},
      {"plaintext_sha256", asset.plaintext_sha256},
      {"created_at", asset.created_at},
      {"updated_at", asset.updated_at},
      {"placements", std::move(placements)},
  };
}

nlohmann::json resource_bundle_to_json(const holder::model::ResourceBundle& bundle) {
  nlohmann::json assets = nlohmann::json::array();
  for (const auto& asset : bundle.assets) assets.push_back(asset_to_json(asset));
  return {
      {"resource",
       {
           {"resource_id", bundle.resource.resource_id},
           {"project_id", bundle.resource.project_id},
           {"type", bundle.resource.type},
           {"label", bundle.resource.label},
           {"metadata", bundle.resource.metadata},
           {"created_at", bundle.resource.created_at},
           {"updated_at", bundle.resource.updated_at},
       }},
      {"assets", std::move(assets)},
  };
}

nlohmann::json location_to_json(const holder::model::Location& location) {
  return {
      {"location_id", location.location_id},
      {"project_id", location.project_id},
      {"name", location.name},
      {"provider", location.provider},
      {"configuration", location.configuration},
      {"created_at", location.created_at},
      {"updated_at", location.updated_at},
  };
}

holder::model::Placement placement_from_json(const nlohmann::json& body, const std::string& asset_id) {
  holder::model::Placement placement;
  placement.placement_id = body.at("placement_id").get<std::string>();
  placement.asset_id = asset_id;
  placement.location_id = body.at("location_id").get<std::string>();
  placement.object_key = body.at("object_key").get<std::string>();
  placement.encoding = body.at("encoding").get<std::string>();
  placement.stored_byte_size = body.at("stored_byte_size").get<long long>();
  placement.stored_sha256 = body.at("stored_sha256").get<std::string>();
  placement.created_at = body.at("created_at").get<long long>();
  return placement;
}

holder::model::Asset asset_from_json(const nlohmann::json& body, const std::string& resource_id) {
  holder::model::Asset asset;
  asset.asset_id = body.at("asset_id").get<std::string>();
  asset.resource_id = resource_id;
  asset.original_filename = body.at("original_filename").get<std::string>();
  asset.media_type = body.at("media_type").get<std::string>();
  asset.byte_size = body.at("byte_size").get<long long>();
  asset.plaintext_sha256 = body.at("plaintext_sha256").get<std::string>();
  asset.created_at = body.at("created_at").get<long long>();
  asset.updated_at = body.at("updated_at").get<long long>();
  for (const auto& item : body.value("placements", nlohmann::json::array())) {
    asset.placements.push_back(placement_from_json(item, asset.asset_id));
  }
  return asset;
}

holder::model::ResourceBundle resource_bundle_from_json(const nlohmann::json& body) {
  const auto& resource_body = body.at("resource");
  holder::model::ResourceBundle bundle;
  bundle.resource.resource_id = resource_body.at("resource_id").get<std::string>();
  bundle.resource.project_id = resource_body.at("project_id").get<std::string>();
  bundle.resource.type = resource_body.at("type").get<std::string>();
  bundle.resource.label = resource_body.at("label").get<std::string>();
  bundle.resource.metadata = resource_body.value(
      "metadata", holder::model::ResourceMetadata{}
  );
  bundle.resource.created_at = resource_body.at("created_at").get<long long>();
  bundle.resource.updated_at = resource_body.at("updated_at").get<long long>();
  for (const auto& item : body.value("assets", nlohmann::json::array())) {
    bundle.assets.push_back(asset_from_json(item, bundle.resource.resource_id));
  }
  return bundle;
}

holder::model::Location location_from_json(const nlohmann::json& body) {
  holder::model::Location location;
  location.location_id = body.at("location_id").get<std::string>();
  location.project_id = body.at("project_id").get<std::string>();
  location.name = body.at("name").get<std::string>();
  location.provider = body.at("provider").get<std::string>();
  location.configuration = body.value("configuration", std::map<std::string, std::string>{});
  location.created_at = body.at("created_at").get<long long>();
  location.updated_at = body.at("updated_at").get<long long>();
  return location;
}

// -- Storage provider registry (see holder_storage_provider_register in holder.h) --

holder::resource::StorageErrorCode storage_error_code_from_int(int value) {
  switch (value) {
    case HOLDER_STORAGE_ERROR_AUTHENTICATION: return holder::resource::StorageErrorCode::Authentication;
    case HOLDER_STORAGE_ERROR_PERMISSION: return holder::resource::StorageErrorCode::Permission;
    case HOLDER_STORAGE_ERROR_CAPACITY: return holder::resource::StorageErrorCode::Capacity;
    case HOLDER_STORAGE_ERROR_INTEGRITY: return holder::resource::StorageErrorCode::Integrity;
    case HOLDER_STORAGE_ERROR_CONFLICT: return holder::resource::StorageErrorCode::Conflict;
    case HOLDER_STORAGE_ERROR_INVALID_CONFIGURATION: return holder::resource::StorageErrorCode::InvalidConfiguration;
    case HOLDER_STORAGE_ERROR_TRANSIENT: return holder::resource::StorageErrorCode::Transient;
    default: return holder::resource::StorageErrorCode::Unavailable;
  }
}

// Owns a C-ABI storage provider's user_data/destroy_user_data pair for exactly as long as
// it's installed in the registry below -- see holder_storage_provider_register's ownership
// contract in holder.h. Implements holder::resource::StorageProvider by calling the C
// callbacks and translating a nonzero return into a typed StorageError, the same shape
// CApiKeyringProviderHandle uses for the keyring seam.
class CApiStorageProviderHandle final : public holder::resource::StorageProvider {
 public:
  CApiStorageProviderHandle(
      holder_storage_put_fn put_fn,
      holder_storage_get_fn get_fn,
      holder_storage_exists_fn exists_fn,
      holder_storage_remove_fn remove_fn,
      void* user_data,
      holder_destroy_fn destroy_user_data
  )
      : put_fn_(put_fn),
        get_fn_(get_fn),
        exists_fn_(exists_fn),
        remove_fn_(remove_fn),
        user_data_(user_data),
        destroy_user_data_(destroy_user_data) {}

  ~CApiStorageProviderHandle() override {
    if (destroy_user_data_ != nullptr) destroy_user_data_(user_data_);
  }

  CApiStorageProviderHandle(const CApiStorageProviderHandle&) = delete;
  CApiStorageProviderHandle& operator=(const CApiStorageProviderHandle&) = delete;

  void put(
      const std::string& object_key,
      const std::filesystem::path& staged_file,
      long long stored_size,
      const std::string& stored_sha256
  ) override {
    int error_code = HOLDER_STORAGE_ERROR_UNAVAILABLE;
    char* error_ptr = nullptr;
    const int rc = put_fn_(
        user_data_,
        object_key.c_str(),
        staged_file.string().c_str(),
        stored_size,
        stored_sha256.c_str(),
        &error_code,
        &error_ptr
    );
    throw_if_failed(rc, error_code, error_ptr, "put");
  }

  void get(const std::string& object_key, const std::filesystem::path& destination_file) override {
    int error_code = HOLDER_STORAGE_ERROR_UNAVAILABLE;
    char* error_ptr = nullptr;
    const int rc =
        get_fn_(user_data_, object_key.c_str(), destination_file.string().c_str(), &error_code, &error_ptr);
    throw_if_failed(rc, error_code, error_ptr, "get");
  }

  bool exists(const std::string& object_key) override {
    int found = 0;
    int error_code = HOLDER_STORAGE_ERROR_UNAVAILABLE;
    char* error_ptr = nullptr;
    const int rc = exists_fn_(user_data_, object_key.c_str(), &found, &error_code, &error_ptr);
    throw_if_failed(rc, error_code, error_ptr, "exists");
    return found != 0;
  }

  void remove(const std::string& object_key) override {
    int error_code = HOLDER_STORAGE_ERROR_UNAVAILABLE;
    char* error_ptr = nullptr;
    const int rc = remove_fn_(user_data_, object_key.c_str(), &error_code, &error_ptr);
    throw_if_failed(rc, error_code, error_ptr, "remove");
  }

 private:
  static void throw_if_failed(int rc, int error_code, char* error_ptr, const char* op) {
    if (rc == 0) {
      if (error_ptr != nullptr) std::free(error_ptr); // LCOV_EXCL_LINE
      return;
    }
    std::string message = error_ptr != nullptr ? std::string(error_ptr)
                                                : (std::string("storage provider ") + op + " failed");
    if (error_ptr != nullptr) std::free(error_ptr);
    throw holder::resource::StorageError(storage_error_code_from_int(error_code), message);
  }

  holder_storage_put_fn put_fn_;
  holder_storage_get_fn get_fn_;
  holder_storage_exists_fn exists_fn_;
  holder_storage_remove_fn remove_fn_;
  void* user_data_;
  holder_destroy_fn destroy_user_data_;
};

std::mutex& storage_provider_registry_mutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, std::shared_ptr<CApiStorageProviderHandle>>& storage_provider_registry() {
  static std::map<std::string, std::shared_ptr<CApiStorageProviderHandle>> registry;
  return registry;
}

// "local_directory" is always available with no registration required, rooted per
// data_dir; every other provider name (e.g. "google-drive", "s3_compatible") must have
// been registered first via holder_storage_provider_register. Named to match
// holder-desktop's own local-storage Location provider string (see
// resources_tool_view.vala) -- Locations are portable/git-synced, so a client-specific
// name here would silently fail to resolve a Location another client created.
holder::resource::StorageProvider& resolve_storage_provider(
    holder_context* context,
    const std::string& provider_name
) {
  if (provider_name == "local_directory") {
    static std::mutex local_mutex;
    static std::map<std::filesystem::path, std::unique_ptr<holder::resource::LocalDirectoryProvider>>
        local_by_root;
    const auto root = context->data_dir / "resource-store";
    std::lock_guard<std::mutex> lock(local_mutex);
    auto it = local_by_root.find(root);
    if (it == local_by_root.end()) {
      it = local_by_root
               .emplace(root, std::make_unique<holder::resource::LocalDirectoryProvider>(root))
               .first;
    }
    return *it->second;
  }
  std::lock_guard<std::mutex> lock(storage_provider_registry_mutex());
  auto it = storage_provider_registry().find(provider_name);
  if (it == storage_provider_registry().end()) {
    throw std::runtime_error("no storage provider registered for: " + provider_name);
  }
  return *it->second;
}

int return_json(const nlohmann::json& body, char** out_json, holder_error** out_error) {
  auto* out = duplicate_string(body.dump());
  if (out == nullptr) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed"); // LCOV_EXCL_LINE
  }
  *out_json = out;
  return HOLDER_OK;
}

template <typename Fn>
int with_json_output(
    holder_context* context,
    char** out_json,
    holder_error** out_error,
    Fn&& fn
) {
  clear_error(out_error);
  if (out_json == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "out_json must not be null");
  }
  *out_json = nullptr;
  if (context == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "context must not be null");
  }
  try {
    return return_json(fn(), out_json, out_error);
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed"); // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error); // LCOV_EXCL_LINE
  }
}

template <typename Fn>
int with_void_output(holder_context* context, holder_error** out_error, Fn&& fn) {
  clear_error(out_error);
  if (context == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "context must not be null");
  }
  try {
    fn();
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed"); // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error); // LCOV_EXCL_LINE
  }
}

std::optional<holder::model::ResourceBundle> find_bundle_for_asset(
    holder_context* context,
    const std::string& asset_id
) {
  holder::project::ProjectRepo projects(context->db);
  holder::resource::ResourceRepo resources(context->db);
  for (const auto& project : projects.list()) {
    for (const auto& resource : resources.list(project.project_id)) {
      auto bundle = resources.get_bundle(resource.resource_id);
      if (!bundle.has_value()) continue;
      const auto found = std::find_if(
          bundle->assets.begin(), bundle->assets.end(), [&](const auto& asset) {
            return asset.asset_id == asset_id;
          }
      );
      if (found != bundle->assets.end()) return bundle;
    }
  }
  return std::nullopt;
}

std::string to_lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

// from_card_id is a generic "front-matter owner" id shared with ai_messages, so a backlink's
// from side isn't necessarily a card -- to_title/from_title come back null when the id doesn't
// resolve via CardRepo rather than throwing, since that's an expected, not exceptional, case.
nlohmann::json outgoing_link_to_json(holder::platform::Db& db, const holder::model::CardLink& link) {
  const auto to_card = holder::card::CardRepo(db).get(link.to_card_id);
  nlohmann::json body = {
      {"to_card_id", link.to_card_id},
      {"to_type", link.to_type},
      {"kind", link.kind},
      {"created_at", link.created_at},
  };
  body["label"] = link.label.has_value() ? nlohmann::json(*link.label) : nlohmann::json(nullptr);
  body["to_title"] = to_card.has_value() ? nlohmann::json(to_card->title) : nlohmann::json(nullptr);
  return body;
}

nlohmann::json backlink_to_json(holder::platform::Db& db, const holder::model::CardLink& link) {
  const auto from_card = holder::card::CardRepo(db).get(link.from_card_id);
  nlohmann::json body = {
      {"from_card_id", link.from_card_id},
      {"kind", link.kind},
      {"created_at", link.created_at},
  };
  body["label"] = link.label.has_value() ? nlohmann::json(*link.label) : nlohmann::json(nullptr);
  body["from_title"] = from_card.has_value() ? nlohmann::json(from_card->title) : nlohmann::json(nullptr);
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
}  // LCOV_EXCL_LINE

void persist_project_metadata(
    holder_context* context,
    const holder::model::Project& project,
    const std::string& commit_message
) {
  auto git = open_project_git(context, project);
  holder::project::write_project_manifest(*git, project);
  git->commit(commit_message);
}

nlohmann::json optional_json(const std::optional<std::string>& value) {
  return value.has_value() ? nlohmann::json(*value) : nlohmann::json(nullptr);
}

nlohmann::json optional_json(const std::optional<long long>& value) {
  return value.has_value() ? nlohmann::json(*value) : nlohmann::json(nullptr);
}

nlohmann::json milestone_to_json(const holder::model::Milestone& milestone) {
  nlohmann::json body = {
      {"milestone_id", milestone.milestone_id},
      {"card_id", milestone.card_id},
      {"start_at", milestone.start_at},
      {"all_day", milestone.all_day},
      {"created_at", milestone.created_at},
      {"updated_at", milestone.updated_at},
  };
  body["end_at"] = optional_json(milestone.end_at);
  body["kind"] = optional_json(milestone.kind);
  body["description"] = optional_json(milestone.description);
  return body;
}

// Adds card_title (null if unresolvable) for the project-wide range listing, where the caller
// doesn't already know which card each milestone belongs to -- unlike the card-scoped list.
nlohmann::json milestone_with_card_title_to_json(
    holder::platform::Db& db,
    const holder::model::Milestone& milestone
) {
  auto body = milestone_to_json(milestone);
  const auto card = holder::card::CardRepo(db).get(milestone.card_id);
  body["card_title"] = card.has_value() ? nlohmann::json(card->title) : nlohmann::json(nullptr);
  return body;
}

nlohmann::json project_sync_to_json(const std::optional<holder::model::ProjectSyncState>& sync) {
  if (!sync.has_value()) {
    return {
        {"last_commit_at", nullptr},
        {"last_push_at", nullptr},
        {"last_pull_at", nullptr},
        {"uncommitted_changes_count", 0},  // LCOV_EXCL_LINE
        {"unpushed_commits_count", 0},  // LCOV_EXCL_LINE
        {"last_push_status", nullptr},
        {"last_pull_status", nullptr},
        {"last_sync_error", nullptr},
        {"last_sync_error_at", nullptr},
        {"retry_count", 0},  // LCOV_EXCL_LINE
        {"next_retry_at", nullptr},
        {"pull_retry_count", 0},  // LCOV_EXCL_LINE
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

  // Only invoked from EcdsaDerSigningCredentialProvider's captured sign callback during a real
  // SSH auth handshake with a remote -- every test in this suite pushes/pulls over local
  // file:// or bare-repo remotes, which libgit2 never authenticates, so this never runs. Not
  // worth standing up a live SSH server just to exercise a thin buffer-ownership adapter.
  // LCOV_EXCL_START
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
  // LCOV_EXCL_STOP

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
  }  // LCOV_EXCL_LINE

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
  }  // LCOV_EXCL_LINE

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
  }  // LCOV_EXCL_LINE

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
}  // LCOV_EXCL_LINE

// Thin wrappers binding the C ABI's holder_context to the shared (holder::sync) pull
// reconciliation/conflict-resolution logic -- shared because holder-daemon's own native sync
// worker needs the exact same behavior and doesn't go through this C ABI at all, so the logic
// itself can't live here as a context-bound implementation detail.
void rebuild_project_index(holder_context* context, const holder::model::Project& project) {
  holder::sync::reconcile_index_after_pull(context->db, &context->fts, project);
}

int resolve_pull_conflicts(
    holder_context* context,
    const holder::model::Project& project,
    holder::git::RealGitOps& git,
    const holder::git::NonFastForwardPullError& diverged,
    long long now
) {
  return holder::sync::resolve_pull_conflicts(context->db, &context->fts, project, git, diverged, now, uuid_v4);
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

    const auto health = holder::platform::inspect_database_health(context->db_path);
    const auto roots = discover_managed_project_roots(context->data_dir);
    if (health.health == holder::platform::DatabaseHealth::IoError) {
      throw std::runtime_error("database I/O failure: " + health.detail);
    }
    bool needs_rebuild = health.health == holder::platform::DatabaseHealth::Corrupt ||
                         (health.health == holder::platform::DatabaseHealth::Missing && !roots.empty());
    // quick_check (what `health` reflects) only catches corruption, not staleness: a
    // structurally intact database left behind by an older build can still be on an
    // older schema_version than this one expects -- the one case this whole codebase's
    // callers actually hit is an in-place app update on Android, which has no other
    // migration path wired up (unlike the desktop daemon, which calls
    // Migrations::migrate_to_latest() directly from its own startup). Reuse the same
    // rebuild-from-durable-git-source-of-truth path already used for Corrupt/Missing
    // rather than running incremental migrations in place here: every future schema
    // bump gets this for free with no migration script of its own to write or forget,
    // and rebuild_database_projection's durable-object-count check means it fails loudly
    // instead of silently if anything doesn't come back the same.
    if (!needs_rebuild && health.health == holder::platform::DatabaseHealth::Healthy) {
      holder::platform::Db probe;
      probe.open(context->db_path);
      const bool stale =
          holder::platform::Migrations::read_schema_version(probe) <
          holder::platform::Migrations::latest_schema_version;
      probe.close();
      needs_rebuild = stale;
    }
    if (needs_rebuild) {
      if (schema_sql == nullptr || schema_sql[0] == '\0') {
        throw std::runtime_error("schema SQL is required to reconstruct the database");
      }
      rebuild_managed_database(context->data_dir, schema_sql, false);
    }

    context->db.open(context->db_path);
    if (schema_sql != nullptr && schema_sql[0] != '\0') {
      context->db.exec(schema_sql);
    }
    try {
      if (holder::project::ProjectRepo(context->db).list().size() != roots.size()) {
        throw std::runtime_error(
            "not every project is under the managed project directory"
        );
      }
      holder::platform::audit_core_durable_ownership(context->db);
      holder::platform::mark_database_rebuild_ready(rebuild_readiness_path(context->data_dir));
    } catch (const std::exception&) {
      // A healthy legacy profile remains usable, but is deliberately not marked
      // reconstructible until its durable owners have been backfilled and audited.
      if (health.health != holder::platform::DatabaseHealth::Healthy) throw;
    }

    *out_context = context.release();
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
}

void holder_context_destroy(holder_context* context) {
  delete context;
}

int holder_database_rebuild(
    const char* data_dir,
    const char* schema_sql,
    int dry_run,
    char** out_json,
    holder_error** out_error
) {
  clear_error(out_error);
  if (out_json == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "out_json must not be null");
  }
  *out_json = nullptr;
  if (data_dir == nullptr || data_dir[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "data_dir must not be empty");
  }
  if (schema_sql == nullptr || schema_sql[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "schema_sql must not be empty");
  }
  try {
    const auto report = rebuild_managed_database(
        std::filesystem::path(data_dir), schema_sql, dry_run != 0
    );
    return return_json(nlohmann::json::parse(report.to_json()), out_json, out_error);
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed"); // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error); // LCOV_EXCL_LINE
  }
}

int holder_resource_list(
    holder_context* context,
    const char* project_id,
    char** out_json,
    holder_error** out_error
) {
  if (project_id == nullptr || project_id[0] == '\0') {
    clear_error(out_error);
    if (out_json != nullptr) *out_json = nullptr;
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "project_id must not be empty");
  }
  return with_json_output(context, out_json, out_error, [&]() {
    nlohmann::json body = nlohmann::json::array();
    holder::resource::ResourceRepo repo(context->db);
    for (const auto& resource : repo.list(project_id)) {
      const auto bundle = repo.get_bundle(resource.resource_id);
      if (bundle.has_value()) body.push_back(resource_bundle_to_json(*bundle));
    }
    return body;
  });
}

int holder_resource_get(
    holder_context* context,
    const char* resource_id,
    char** out_json,
    holder_error** out_error
) {
  if (resource_id == nullptr || resource_id[0] == '\0') {
    clear_error(out_error);
    if (out_json != nullptr) *out_json = nullptr;
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "resource_id must not be empty");
  }
  return with_json_output(context, out_json, out_error, [&]() {
    const auto bundle = holder::resource::ResourceRepo(context->db).get_bundle(resource_id);
    if (!bundle.has_value()) throw std::runtime_error("resource not found: " + std::string(resource_id));
    return resource_bundle_to_json(*bundle);
  });
}

int holder_resource_put_json(
    holder_context* context,
    const char* resource_bundle_json,
    char** out_json,
    holder_error** out_error
) {
  if (resource_bundle_json == nullptr || resource_bundle_json[0] == '\0') {
    clear_error(out_error);
    if (out_json != nullptr) *out_json = nullptr;
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "resource_bundle_json must not be empty");
  }
  return with_json_output(context, out_json, out_error, [&]() {
    const auto bundle = resource_bundle_from_json(nlohmann::json::parse(resource_bundle_json));
    holder::resource::ResourceStore(context->db).put(bundle);
    return resource_bundle_to_json(bundle);
  });
}

int holder_resource_delete(
    holder_context* context,
    const char* resource_id,
    holder_error** out_error
) {
  if (resource_id == nullptr || resource_id[0] == '\0') {
    clear_error(out_error);
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "resource_id must not be empty");
  }
  return with_void_output(context, out_error, [&]() {
    holder::resource::ResourceStore(context->db).remove(resource_id);
  });
}

int holder_asset_get(
    holder_context* context,
    const char* asset_id,
    char** out_json,
    holder_error** out_error
) {
  if (asset_id == nullptr || asset_id[0] == '\0') {
    clear_error(out_error);
    if (out_json != nullptr) *out_json = nullptr;
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "asset_id must not be empty");
  }
  return with_json_output(context, out_json, out_error, [&]() {
    const auto bundle = find_bundle_for_asset(context, asset_id);
    if (!bundle.has_value()) throw std::runtime_error("asset not found: " + std::string(asset_id));
    const auto found = std::find_if(bundle->assets.begin(), bundle->assets.end(), [&](const auto& asset) {
      return asset.asset_id == asset_id;
    });
    return asset_to_json(*found);
  });
}

int holder_asset_put_json(
    holder_context* context,
    const char* resource_id,
    const char* asset_json,
    char** out_json,
    holder_error** out_error
) {
  if (resource_id == nullptr || resource_id[0] == '\0' || asset_json == nullptr || asset_json[0] == '\0') {
    clear_error(out_error);
    if (out_json != nullptr) *out_json = nullptr;
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "resource_id and asset_json must not be empty");
  }
  return with_json_output(context, out_json, out_error, [&]() {
    holder::resource::ResourceRepo repo(context->db);
    auto bundle = repo.get_bundle(resource_id);
    if (!bundle.has_value()) throw std::runtime_error("resource not found: " + std::string(resource_id));
    auto asset = asset_from_json(nlohmann::json::parse(asset_json), resource_id);
    const auto found = std::find_if(bundle->assets.begin(), bundle->assets.end(), [&](const auto& current) {
      return current.asset_id == asset.asset_id;
    });
    if (found == bundle->assets.end()) bundle->assets.push_back(asset);
    else *found = asset;
    bundle->resource.updated_at = std::max(bundle->resource.updated_at, asset.updated_at);
    holder::resource::ResourceStore(context->db).put(*bundle);
    return asset_to_json(asset);
  });
}

int holder_asset_delete(
    holder_context* context,
    const char* asset_id,
    char** out_json,
    holder_error** out_error
) {
  if (asset_id == nullptr || asset_id[0] == '\0') {
    clear_error(out_error);
    if (out_json != nullptr) *out_json = nullptr;
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "asset_id must not be empty");
  }
  return with_json_output(context, out_json, out_error, [&]() {
    auto bundle = find_bundle_for_asset(context, asset_id);
    if (!bundle.has_value()) throw std::runtime_error("asset not found: " + std::string(asset_id));
    bundle->assets.erase(
        std::remove_if(bundle->assets.begin(), bundle->assets.end(), [&](const auto& asset) {
          return asset.asset_id == asset_id;
        }),
        bundle->assets.end()
    );
    holder::resource::ResourceStore(context->db).put(*bundle);
    return resource_bundle_to_json(*bundle);
  });
}

int holder_location_list(
    holder_context* context,
    const char* project_id,
    char** out_json,
    holder_error** out_error
) {
  if (project_id == nullptr || project_id[0] == '\0') {
    clear_error(out_error);
    if (out_json != nullptr) *out_json = nullptr;
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "project_id must not be empty");
  }
  return with_json_output(context, out_json, out_error, [&]() {
    nlohmann::json body = nlohmann::json::array();
    for (const auto& location : holder::resource::LocationRepo(context->db).list(project_id)) {
      body.push_back(location_to_json(location));
    }
    return body;
  });
}

int holder_location_get(
    holder_context* context,
    const char* location_id,
    char** out_json,
    holder_error** out_error
) {
  if (location_id == nullptr || location_id[0] == '\0') {
    clear_error(out_error);
    if (out_json != nullptr) *out_json = nullptr;
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "location_id must not be empty");
  }
  return with_json_output(context, out_json, out_error, [&]() {
    const auto location = holder::resource::LocationRepo(context->db).get(location_id);
    if (!location.has_value()) throw std::runtime_error("location not found: " + std::string(location_id));
    return location_to_json(*location);
  });
}

int holder_location_put_json(
    holder_context* context,
    const char* location_json,
    char** out_json,
    holder_error** out_error
) {
  if (location_json == nullptr || location_json[0] == '\0') {
    clear_error(out_error);
    if (out_json != nullptr) *out_json = nullptr;
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "location_json must not be empty");
  }
  return with_json_output(context, out_json, out_error, [&]() {
    const auto location = location_from_json(nlohmann::json::parse(location_json));
    holder::resource::LocationStore(context->db).put(location);
    return location_to_json(location);
  });
}

int holder_location_delete(
    holder_context* context,
    const char* location_id,
    holder_error** out_error
) {
  if (location_id == nullptr || location_id[0] == '\0') {
    clear_error(out_error);
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "location_id must not be empty");
  }
  return with_void_output(context, out_error, [&]() {
    holder::resource::LocationStore(context->db).remove(location_id);
  });
}

int holder_storage_provider_register(
    const char* provider_name,
    holder_storage_put_fn put_fn,
    holder_storage_get_fn get_fn,
    holder_storage_exists_fn exists_fn,
    holder_storage_remove_fn remove_fn,
    void* user_data,
    holder_destroy_fn destroy_user_data,
    holder_error** out_error
) {
  clear_error(out_error);

  // Takes ownership of user_data unconditionally from this point on, exactly like
  // holder_keyring_set_provider -- constructing the RAII handle before any validation is
  // what makes every early return below (including a validation failure) still run
  // destroy_user_data exactly once, via normal scope-exit destruction.
  std::shared_ptr<CApiStorageProviderHandle> handle;
  try {
    handle = std::make_shared<CApiStorageProviderHandle>(
        put_fn, get_fn, exists_fn, remove_fn, user_data, destroy_user_data
    );
  // LCOV_EXCL_START
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  }
  // LCOV_EXCL_STOP

  if (provider_name == nullptr || provider_name[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "provider_name must not be empty");
  }
  if (std::string(provider_name) == "local_directory") {
    return set_error(
        out_error,
        HOLDER_ERROR_INVALID_ARGUMENT,
        "'local_directory' is a built-in provider name and cannot be overridden"
    );
  }
  if (put_fn == nullptr || get_fn == nullptr || exists_fn == nullptr || remove_fn == nullptr) {
    return set_error(
        out_error,
        HOLDER_ERROR_INVALID_ARGUMENT,
        "put_fn, get_fn, exists_fn, and remove_fn must not be null"
    );
  }

  try {
    std::lock_guard<std::mutex> lock(storage_provider_registry_mutex());
    storage_provider_registry()[provider_name] = std::move(handle);
    return HOLDER_OK;
  // LCOV_EXCL_START
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);
  }
  // LCOV_EXCL_STOP
}

int holder_asset_import_file(
    holder_context* context,
    const char* project_id,
    const char* card_id,
    const char* location_id,
    const char* source_file_path,
    char** out_json,
    holder_error** out_error
) {
  if (project_id == nullptr || project_id[0] == '\0' || card_id == nullptr || card_id[0] == '\0' ||
      location_id == nullptr || location_id[0] == '\0' || source_file_path == nullptr ||
      source_file_path[0] == '\0') {
    clear_error(out_error);
    if (out_json != nullptr) *out_json = nullptr;
    return set_error(
        out_error,
        HOLDER_ERROR_INVALID_ARGUMENT,
        "project_id, card_id, location_id, and source_file_path must not be empty"
    );
  }
  return with_json_output(context, out_json, out_error, [&]() {
    const auto location = holder::resource::LocationRepo(context->db).get(location_id);
    if (!location.has_value()) throw std::runtime_error("location not found: " + std::string(location_id));

    holder::resource::AssetImportRequest request;
    request.project_id = project_id;
    request.card_id = card_id;
    request.location_id = location_id;
    request.source_file = std::filesystem::path(source_file_path);
    request.now = now_epoch_seconds();

    holder::resource::AssetImportService service(
        context->db,
        context->data_dir / "server" / "asset-staging",
        uuid_v4
    );
    auto& provider = resolve_storage_provider(context, location->provider);
    const auto result = service.import_file(request, provider);
    return nlohmann::json{
        {"resource_id", result.resource_id},
        {"asset_id", result.asset_id},
        {"duplicate_reused", result.duplicate_reused},
        {"link_created", result.link_created},
    };
  });
}

int holder_asset_retrieve(
    holder_context* context,
    const char* resource_id,
    const char* asset_id,
    const char* placement_id,
    const char* destination_file_path,
    holder_error** out_error
) {
  if (resource_id == nullptr || resource_id[0] == '\0' || asset_id == nullptr || asset_id[0] == '\0' ||
      placement_id == nullptr || placement_id[0] == '\0' || destination_file_path == nullptr ||
      destination_file_path[0] == '\0') {
    clear_error(out_error);
    return set_error(
        out_error,
        HOLDER_ERROR_INVALID_ARGUMENT,
        "resource_id, asset_id, placement_id, and destination_file_path must not be empty"
    );
  }
  return with_void_output(context, out_error, [&]() {
    const auto bundle = holder::resource::ResourceRepo(context->db).get_bundle(resource_id);
    if (!bundle.has_value()) throw std::runtime_error("resource not found: " + std::string(resource_id));
    const auto asset = std::find_if(bundle->assets.begin(), bundle->assets.end(), [&](const auto& item) {
      return item.asset_id == asset_id;
    });
    if (asset == bundle->assets.end()) {
      throw std::runtime_error("asset not found in resource: " + std::string(asset_id));
    }
    const auto placement =
        std::find_if(asset->placements.begin(), asset->placements.end(), [&](const auto& item) {
          return item.placement_id == placement_id;
        });
    if (placement == asset->placements.end()) {
      throw std::runtime_error("placement not found in asset: " + std::string(placement_id));
    }
    const auto location = holder::resource::LocationRepo(context->db).get(placement->location_id);
    if (!location.has_value()) throw std::runtime_error("location not found: " + placement->location_id);

    holder::resource::AssetImportService service(
        context->db,
        context->data_dir / "server" / "asset-staging",
        uuid_v4
    );
    auto& provider = resolve_storage_provider(context, location->provider);
    service.retrieve(
        resource_id, asset_id, placement_id, provider, std::filesystem::path(destination_file_path)
    );
  });
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
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
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
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
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
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
    }

    *out_content = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
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

    if (root_path != nullptr && root_path[0] != '\0') {
      // This generic C API does not yet persist a registry for caller-selected
      // roots. Fail closed rather than later rebuilding only managed projects.
      std::error_code ignored;
      std::filesystem::remove(rebuild_readiness_path(context->data_dir), ignored);
    }

    auto* out = duplicate_string(project_to_json(created).dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
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
    const auto updated = repo.get(project_id).value();
    persist_project_metadata(context, updated, "Update project metadata");

    auto* out = duplicate_string(project_to_json(updated).dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
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
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
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
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
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
      // CardStore::update_content above already throws "card not found" for this exact
      // condition, so this branch is unreachable given the current implementation -- kept as
      // defense-in-depth in case that invariant ever changes.
      return set_error(out_error, HOLDER_ERROR_RUNTIME, "card not found: " + std::string(card_id)); // LCOV_EXCL_LINE
    }

    auto* out = duplicate_string(card_to_json(updated.value()).dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
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
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
}

int holder_card_list_trashed(
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
      if (!card.deleted_at.has_value()) {
        continue;
      }
      body.push_back(card_to_json(card));
    }

    auto* out = duplicate_string(body.dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
}

int holder_card_restore(
    holder_context* context,
    const char* card_id,
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

  try {
    holder::card::CardStore store(context->db, &context->fts);
    store.restore(card_id, now_epoch_seconds());

    holder::card::CardRepo repo(context->db);
    const auto restored = repo.get(card_id);
    auto* out = duplicate_string(card_to_json(restored.value()).dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
}

int holder_card_purge(holder_context* context, const char* card_id, holder_error** out_error) {
  clear_error(out_error);
  if (context == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "context must not be null");
  }
  if (card_id == nullptr || card_id[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "card_id must not be empty");
  }

  try {
    holder::card::CardStore store(context->db, &context->fts);
    store.hard_delete(card_id);
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
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
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
}

int holder_card_list_links(
    holder_context* context,
    const char* card_id,
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

  try {
    holder::card::CardRepo cards(context->db);
    const auto card = cards.get(card_id);
    if (!card.has_value()) {
      return set_error(out_error, HOLDER_ERROR_RUNTIME, "card not found: " + std::string(card_id));
    }

    holder::card::LinkRepo links(context->db);
    nlohmann::json outgoing = nlohmann::json::array();
    for (const auto& link : links.list_outgoing(card->project_id, card_id)) {
      outgoing.push_back(outgoing_link_to_json(context->db, link));
    }
    nlohmann::json backlinks = nlohmann::json::array();
    for (const auto& link : links.list_backlinks(card->project_id, card_id)) {
      backlinks.push_back(backlink_to_json(context->db, link));
    }

    nlohmann::json parent = nullptr;
    if (card->parent_card_id.has_value()) {
      const auto parent_card = cards.get(*card->parent_card_id);
      if (parent_card.has_value()) {
        parent = {{"card_id", parent_card->card_id}, {"title", parent_card->title}};
      }
    }
    nlohmann::json children = nlohmann::json::array();
    for (const auto& child : cards.list_children(card->project_id, card_id)) {
      if (child.deleted_at.has_value()) {
        continue;
      }
      children.push_back({{"card_id", child.card_id}, {"title", child.title}});
    }

    nlohmann::json body = {
        {"outgoing", outgoing}, {"backlinks", backlinks}, {"parent", parent}, {"children", children}
    };
    auto* out = duplicate_string(body.dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
}

int holder_card_link_add(
    holder_context* context,
    const char* from_card_id,
    const char* to_card_id,
    const char* kind,
    const char* label,
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
  if (from_card_id == nullptr || from_card_id[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "from_card_id must not be empty");
  }
  if (to_card_id == nullptr || to_card_id[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "to_card_id must not be empty");
  }
  if (kind == nullptr || kind[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "kind must not be empty");
  }

  try {
    holder::card::CardRepo cards(context->db);
    const auto from_card = cards.get(from_card_id);
    if (!from_card.has_value()) {
      return set_error(
          out_error, HOLDER_ERROR_RUNTIME, "card not found: " + std::string(from_card_id)
      );
    }
    if (!cards.get(to_card_id).has_value()) {
      return set_error(out_error, HOLDER_ERROR_RUNTIME, "card not found: " + std::string(to_card_id));
    }

    holder::model::CardLink link;
    link.project_id = from_card->project_id;
    link.from_card_id = from_card_id;
    link.to_card_id = to_card_id;
    link.to_type = "card";
    link.kind = kind;
    if (label != nullptr && label[0] != '\0') {
      link.label = std::string(label);
    }
    link.created_at = now_epoch_seconds();

    holder::card::LinkRepo(context->db).upsert_links(from_card->project_id, from_card_id, {link});
    holder::card::CardStore(context->db, &context->fts).update_links(from_card_id, now_epoch_seconds());

    nlohmann::json outgoing = nlohmann::json::array();
    for (const auto& l : holder::card::LinkRepo(context->db).list_outgoing(from_card->project_id, from_card_id)) {
      outgoing.push_back(outgoing_link_to_json(context->db, l));
    }
    auto* out = duplicate_string(outgoing.dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
}

int holder_card_link_remove(
    holder_context* context,
    const char* from_card_id,
    const char* to_card_id,
    const char* kind,
    holder_error** out_error
) {
  clear_error(out_error);
  if (context == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "context must not be null");
  }
  if (from_card_id == nullptr || from_card_id[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "from_card_id must not be empty");
  }
  if (to_card_id == nullptr || to_card_id[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "to_card_id must not be empty");
  }
  if (kind == nullptr || kind[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "kind must not be empty");
  }

  try {
    holder::card::CardRepo cards(context->db);
    const auto from_card = cards.get(from_card_id);
    if (!from_card.has_value()) {
      return set_error(
          out_error, HOLDER_ERROR_RUNTIME, "card not found: " + std::string(from_card_id)
      );
    }

    holder::card::LinkRepo(context->db).delete_link(
        from_card->project_id, from_card_id, to_card_id, std::string("card"), std::string(kind)
    );
    holder::card::CardStore(context->db, &context->fts).update_links(from_card_id, now_epoch_seconds());
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
}

int holder_card_list_tags(
    holder_context* context,
    const char* card_id,
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

  try {
    holder::card::CardRepo cards(context->db);
    const auto card = cards.get(card_id);
    if (!card.has_value()) {
      return set_error(out_error, HOLDER_ERROR_RUNTIME, "card not found: " + std::string(card_id));
    }

    const auto tags = holder::card::TagRepo(context->db).list_tags_for_card(card->project_id, card_id);
    auto* out = duplicate_string(nlohmann::json(tags).dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
}

int holder_cards_with_tag(
    holder_context* context,
    const char* project_id,
    const char* tag,
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
  if (tag == nullptr || tag[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "tag must not be empty");
  }

  try {
    holder::card::CardRepo cards(context->db);
    holder::card::TagRepo tags(context->db);

    nlohmann::json body = nlohmann::json::array();
    for (const auto& id : tags.list_card_ids_with_tag(project_id, to_lower(tag))) {
      const auto card = cards.get(id);
      if (!card.has_value()) {
        continue;  // LCOV_EXCL_LINE
      }
      body.push_back({{"card_id", card->card_id}, {"title", card->title}});
    }

    auto* out = duplicate_string(body.dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
}

int holder_project_list_tags(
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
    nlohmann::json body = nlohmann::json::array();
    for (const auto& [tag, count] : holder::card::TagRepo(context->db).list_project_tags(project_id)) {
      body.push_back({{"tag", tag}, {"count", count}});
    }

    auto* out = duplicate_string(body.dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
}

int holder_card_list_milestones(
    holder_context* context,
    const char* card_id,
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

  try {
    holder::card::CardRepo cards(context->db);
    const auto card = cards.get(card_id);
    if (!card.has_value()) {
      return set_error(out_error, HOLDER_ERROR_RUNTIME, "card not found: " + std::string(card_id));
    }

    nlohmann::json body = nlohmann::json::array();
    for (const auto& milestone :
         holder::card::MilestoneRepo(context->db).list_for_card(card->project_id, card_id)) {
      body.push_back(milestone_to_json(milestone));
    }

    auto* out = duplicate_string(body.dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
}

int holder_card_milestone_add(
    holder_context* context,
    const char* card_id,
    long long start_at,
    int has_end_at,
    long long end_at,
    int all_day,
    const char* kind,
    const char* description,
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

  try {
    holder::card::CardRepo cards(context->db);
    const auto card = cards.get(card_id);
    if (!card.has_value()) {
      return set_error(out_error, HOLDER_ERROR_RUNTIME, "card not found: " + std::string(card_id));
    }

    holder::card::MilestoneRepo milestone_repo(context->db);
    auto milestones = milestone_repo.list_for_card(card->project_id, card_id);

    holder::model::Milestone milestone;
    milestone.milestone_id = uuid_v4();
    milestone.project_id = card->project_id;
    milestone.card_id = card_id;
    milestone.start_at = start_at;
    if (has_end_at != 0) {
      milestone.end_at = end_at;
    }
    milestone.all_day = (all_day != 0);
    if (kind != nullptr && kind[0] != '\0') {
      milestone.kind = std::string(kind);
    }
    if (description != nullptr && description[0] != '\0') {
      milestone.description = std::string(description);
    }
    milestone.created_at = now_epoch_seconds();
    milestone.updated_at = milestone.created_at;
    milestones.push_back(milestone);

    milestone_repo.replace_for_card(card->project_id, card_id, milestones);
    holder::card::CardStore(context->db, &context->fts)
        .update_milestones(card_id, now_epoch_seconds());

    nlohmann::json body = nlohmann::json::array();
    for (const auto& m : milestone_repo.list_for_card(card->project_id, card_id)) {
      body.push_back(milestone_to_json(m));
    }
    auto* out = duplicate_string(body.dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
}

int holder_card_milestone_remove(
    holder_context* context,
    const char* card_id,
    const char* milestone_id,
    holder_error** out_error
) {
  clear_error(out_error);
  if (context == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "context must not be null");
  }
  if (card_id == nullptr || card_id[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "card_id must not be empty");
  }
  if (milestone_id == nullptr || milestone_id[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "milestone_id must not be empty");
  }

  try {
    holder::card::CardRepo cards(context->db);
    const auto card = cards.get(card_id);
    if (!card.has_value()) {
      return set_error(out_error, HOLDER_ERROR_RUNTIME, "card not found: " + std::string(card_id));
    }

    holder::card::MilestoneRepo milestone_repo(context->db);
    auto milestones = milestone_repo.list_for_card(card->project_id, card_id);
    const auto before = milestones.size();
    milestones.erase(
        std::remove_if(
            milestones.begin(),
            milestones.end(),
            [&](const holder::model::Milestone& m) { return m.milestone_id == milestone_id; }
        ),
        milestones.end()
    );
    if (milestones.size() == before) {
      return HOLDER_OK;
    }

    milestone_repo.replace_for_card(card->project_id, card_id, milestones);
    holder::card::CardStore(context->db, &context->fts)
        .update_milestones(card_id, now_epoch_seconds());
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
}

int holder_project_list_milestones_in_range(
    holder_context* context,
    const char* project_id,
    long long from,
    long long to,
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
    nlohmann::json body = nlohmann::json::array();
    for (const auto& milestone :
         holder::card::MilestoneRepo(context->db).list_in_range(project_id, from, to)) {
      body.push_back(milestone_with_card_title_to_json(context->db, milestone));
    }

    auto* out = duplicate_string(body.dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
}

int holder_link_kind_list(char** out_json, holder_error** out_error) {
  clear_error(out_error);
  if (out_json == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "out_json must not be null");
  }
  *out_json = nullptr;

  try {
    nlohmann::json body = nlohmann::json::array();
    for (const auto& entry : holder::core::link_kind_catalog()) {
      body.push_back({{"id", entry.id}, {"forward", entry.forward_label}, {"reverse", entry.reverse_label}});
    }

    auto* out = duplicate_string(body.dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {  // LCOV_EXCL_LINE
    return set_exception(out_error, e);  // LCOV_EXCL_LINE
  } catch (...) {  // LCOV_EXCL_LINE
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
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
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
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
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
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
    // Only fails on an internal libgit2 allocation failure; not practically triggerable from a
    // test.
    // LCOV_EXCL_START
    const git_error* e = git_error_last();
    return set_error(
        out_error,
        HOLDER_ERROR_RUNTIME,
        std::string("git_libgit2_opts(GIT_OPT_SET_HOMEDIR) failed: ") +
            (e && e->message ? e->message : "unknown error")
    );
    // LCOV_EXCL_STOP
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
  // LCOV_EXCL_START
  } catch (const std::bad_alloc&) {
    // user_data was never captured; nothing to release.
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  }
  // LCOV_EXCL_STOP

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
        [handle, sign_fn](const unsigned char* data, size_t data_len) { // LCOV_EXCL_START
          // Invoked only by a real SSH server's authentication challenge; local-remotes tests
          // verify provider wiring without requiring network credentials or an SSH daemon.
          return handle->sign(sign_fn, data, data_len);
        } // LCOV_EXCL_STOP
    );
    return HOLDER_OK;
  // EcdsaDerSigningCredentialProvider's constructor never throws anything but bad_alloc, so
  // beyond that this whole catch chain is unreachable given the current implementation --
  // kept only as the same defense-in-depth boilerplate every other C ABI function uses.
  // LCOV_EXCL_START
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);
  }
  // LCOV_EXCL_STOP
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
    const auto updated = repo.get(project_id).value();
    auto git = open_project_git(context, updated);
    if (url.has_value()) {
      git->set_remote("origin", *url);
    } else {
      git->remove_remote("origin");
    }
    holder::project::write_project_manifest(*git, updated);
    git->commit("Update project metadata");

    auto* out = duplicate_string(project_to_json(updated).dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
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
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
    }
    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
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
      // LCOV_EXCL_START -- refresh_sync_activity_counts only throws if the repo dir is an
      // unreadable/corrupted git repo at this exact moment, not practically triggerable
      // right after a successful push/pull/import against it.
      } catch (const std::exception&) {
        // Best-effort only; metrics refresh failure does not fail push response.
      }
      // LCOV_EXCL_STOP

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
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
    }
    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
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
        rebuild_project_index(context, project);
        sync_repo.record_pull_result(project_id, "succeeded", true, std::nullopt, now);
        body["status"] = "succeeded";
        body["error_message"] = nullptr;
        body["conflicts_resolved"] = 0;
      } catch (const holder::git::NonFastForwardPullError& diverged) {
        const int resolved = resolve_pull_conflicts(context, project, *git, diverged, now);
        rebuild_project_index(context, project);
        sync_repo.record_pull_result(project_id, "succeeded", true, std::nullopt, now);
        body["status"] = "succeeded";
        body["error_message"] = nullptr;
        body["conflicts_resolved"] = resolved;
      } catch (const std::exception& e) {
        sync_repo.record_pull_result(project_id, "failed", false, std::optional<std::string>(e.what()), now);
        body["status"] = "failed";
        body["error_message"] = e.what();
      }
      try {
        refresh_sync_activity_counts(context->db, project_id, project.root_path, now);
      // LCOV_EXCL_START -- refresh_sync_activity_counts only throws if the repo dir is an
      // unreadable/corrupted git repo at this exact moment, not practically triggerable
      // right after a successful push/pull/import against it.
      } catch (const std::exception&) {
        // Best-effort only; metrics refresh failure does not fail pull response.
      }
      // LCOV_EXCL_STOP
    }

    auto* out = duplicate_string(body.dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
    }
    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
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
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
    }
    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
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
        {"pull_conflicts_resolved", 0},  // LCOV_EXCL_LINE
        {"push_attempted", false},
        {"push_status", nullptr},
        {"push_error", nullptr},
    };

    if (!project.git_remote_url.has_value() || project.git_remote_url->empty()) {
      auto* out = duplicate_string(body.dump());
      if (out == nullptr) {
        return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
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
        rebuild_project_index(context, project);
        sync_repo.record_pull_result(project_id, "succeeded", true, std::nullopt, now);
        body["pull_status"] = "succeeded";
      } catch (const holder::git::NonFastForwardPullError& diverged) {
        body["pull_conflicts_resolved"] = resolve_pull_conflicts(context, project, *git, diverged, now);
        rebuild_project_index(context, project);
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
      // LCOV_EXCL_START -- refresh_sync_activity_counts only throws if the repo dir is an
      // unreadable/corrupted git repo at this exact moment, not practically triggerable
      // right after a successful push/pull/import against it.
      } catch (const std::exception&) {
        // Best-effort only; metrics refresh failure does not fail sync_if_due.
      }
      // LCOV_EXCL_STOP
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
    }; // LCOV_EXCL_LINE -- GCC attributes the already-executed aggregate initialization here.
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
      // LCOV_EXCL_START -- refresh_sync_activity_counts only throws if the repo dir is an
      // unreadable/corrupted git repo at this exact moment, not practically triggerable
      // right after a successful push/pull/import against it.
      } catch (const std::exception&) {
        // Best-effort only; metrics refresh failure does not fail sync_if_due.
      }
      // LCOV_EXCL_STOP
    }

    auto* out = duplicate_string(body.dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
    }
    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
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
  // LCOV_EXCL_START
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  }
  // LCOV_EXCL_STOP

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
  // platform_keyring_set_external_provider only moves std::functions into storage; it never
  // throws anything but bad_alloc, so beyond that this whole catch chain is unreachable given
  // the current implementation -- kept only as the same defense-in-depth boilerplate as
  // elsewhere.
  // LCOV_EXCL_START
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);
  }
  // LCOV_EXCL_STOP
}

int holder_encryption_check(
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
    const auto project_opt = repo.get(project_id);
    if (!project_opt.has_value()) {
      return set_error(out_error, HOLDER_ERROR_RUNTIME, "project not found: " + std::string(project_id));
    }
    const auto& project = project_opt.value();

    nlohmann::json body = {
        {"project_id", project_id},
        {"privacy_mode", project.privacy_mode},
    };

    if (project.privacy_mode != "encrypted_git") {
      body["check"] = {
          {"ok", true},
          {"checked_files", 0},  // LCOV_EXCL_LINE
          {"unsafe_files", 0},  // LCOV_EXCL_LINE
          {"unsafe_paths", nlohmann::json::array()},
          {"message", "Project is plain mode; privacy check not required."},
      };
    } else {
      const auto check = holder::privacy::run_encryption_safety_check(project.root_path);
      body["check"] = {
          {"ok", check.ok},
          {"checked_files", check.checked_files},
          {"unsafe_files", check.unsafe_paths.size()},  // LCOV_EXCL_LINE
          {"unsafe_paths", check.unsafe_paths},
          {"message", check.message},
      };
    }  // LCOV_EXCL_LINE

    auto* out = duplicate_string(body.dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
    }
    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
}

int holder_recovery_token_export(
    holder_context* context,
    const char* project_id,
    const char* pin,
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
  if (pin == nullptr || pin[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "pin must not be empty");
  }

  try {
    holder::project::ProjectRepo repo(context->db);
    const auto project_opt = repo.get(project_id);
    if (!project_opt.has_value()) {
      return set_error(out_error, HOLDER_ERROR_RUNTIME, "project not found: " + std::string(project_id));
    }
    const auto& project = project_opt.value();
    if (!project.project_key_id.has_value()) {
      return set_error(out_error, HOLDER_ERROR_RUNTIME, "project has no key material configured");
    }

    const std::string token = holder::privacy::export_recovery_token(
        project_id,
        project.project_key_id.value(),
        pin,
        project.name,
        project.git_remote_url
    );

    nlohmann::json body = {
        {"project_id", project_id},
        {"key_id", project.project_key_id.value()},
        {"recovery_token", token},
    };

    auto* out = duplicate_string(body.dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
    }
    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
}

int holder_recovery_token_import(
    holder_context* context,
    const char* project_id,
    const char* pin,
    const char* recovery_token,
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
  if (pin == nullptr || pin[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "pin must not be empty");
  }
  if (recovery_token == nullptr || recovery_token[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "recovery_token must not be empty");
  }

  try {
    holder::project::ProjectRepo repo(context->db);
    if (!repo.get(project_id).has_value()) {
      return set_error(out_error, HOLDER_ERROR_RUNTIME, "project not found: " + std::string(project_id));
    }

    holder::privacy::import_recovery_token(repo, project_id, pin, recovery_token, now_epoch_seconds());
    persist_project_metadata(
        context,
        repo.get(project_id).value(),
        "Restore encrypted project metadata"
    );

    nlohmann::json body = {{"project_id", project_id}};
    auto* out = duplicate_string(body.dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
    }
    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
}

int holder_recovery_token_inspect(
    const char* pin,
    const char* recovery_token,
    char** out_json,
    holder_error** out_error
) {
  clear_error(out_error);
  if (out_json == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "out_json must not be null");
  }
  *out_json = nullptr;

  if (pin == nullptr || pin[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "pin must not be empty");
  }
  if (recovery_token == nullptr || recovery_token[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "recovery_token must not be empty");
  }

  try {
    const auto metadata = holder::privacy::inspect_recovery_token(pin, recovery_token);
    nlohmann::json body = {
        {"project_id", metadata.project_id},
        {"project_key_id", metadata.project_key_id},
        {"project_name", optional_json(metadata.project_name)},
        {"git_remote_url", optional_json(metadata.git_remote_url)},
    };

    auto* out = duplicate_string(body.dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
    }
    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
}

int holder_recovery_token_import_global(
    holder_context* context,
    const char* pin,
    const char* recovery_token,
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
  if (pin == nullptr || pin[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "pin must not be empty");
  }
  if (recovery_token == nullptr || recovery_token[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "recovery_token must not be empty");
  }

  try {
    const auto metadata = holder::privacy::inspect_recovery_token(pin, recovery_token);

    holder::project::ProjectRepo repo(context->db);
    holder::project::ProjectSyncRepo sync_repo(context->db);
    const auto now = now_epoch_seconds();

    bool project_created = false;
    auto project_opt = repo.get(metadata.project_id);
    if (!project_opt.has_value()) {
      holder::model::Project project;
      project.project_id = metadata.project_id;
      project.name = metadata.project_name.has_value() && !metadata.project_name->empty()
                         ? metadata.project_name.value()
                         : "Recovered Project";
      project.privacy_mode = "encrypted_git";
      project.created_at = now;
      project.updated_at = now;
      const auto slug = holder::core::slugify(project.name);
      project.root_path = holder::core::unique_project_root(context->data_dir / "projects", slug, repo.list());
      repo.create(project);
      project_created = true;
      project_opt = repo.get(metadata.project_id);
    }

    holder::privacy::import_recovery_token(repo, metadata.project_id, pin, recovery_token, now);

    const bool remote_hint_present =
        metadata.git_remote_url.has_value() && !metadata.git_remote_url->empty();
    bool remote_configured = false;
    std::string pull_status = "not_attempted";
    std::string remote_error;
    std::string pull_error;

    if (remote_hint_present) {
      const auto refreshed = repo.get(metadata.project_id);
      if (refreshed.has_value()) {
        auto git = open_project_git(context, refreshed.value());
        try {
          git->set_remote("origin", metadata.git_remote_url.value());
          remote_configured = true;
          // LCOV_EXCL_START -- libgit2 is lenient about remote URL content and "origin" is
          // always a valid remote name here, so this realistically only fails on an internal
          // libgit2 error, not on any input a test could construct.
        } catch (const std::exception& ex) {
          remote_error = ex.what();
        }
        // LCOV_EXCL_STOP

        if (remote_configured) {
          try {
            git->pull_remote_ff_only("origin");
            rebuild_project_index(context, refreshed.value());
            pull_status = "succeeded";
            sync_repo.record_pull_result(metadata.project_id, pull_status, true, std::nullopt, now);
          } catch (const holder::git::NonFastForwardPullError& diverged) {
            resolve_pull_conflicts(context, refreshed.value(), *git, diverged, now);
            rebuild_project_index(context, refreshed.value());
            pull_status = "succeeded";
            sync_repo.record_pull_result(metadata.project_id, pull_status, true, std::nullopt, now);
          } catch (const std::exception& ex) {
            pull_status = "failed";
            pull_error = ex.what();
            sync_repo.record_pull_result(
                metadata.project_id,
                pull_status,
                false,
                std::optional<std::string>(pull_error),
                now
            );
          }
        }
        try {
          refresh_sync_activity_counts(context->db, metadata.project_id, refreshed->root_path, now);
        // LCOV_EXCL_START -- refresh_sync_activity_counts only throws if the repo dir is an
        // unreadable/corrupted git repo at this exact moment, not practically triggerable
        // right after a successful push/pull/import against it.
        } catch (const std::exception&) {
          // Best-effort only; metrics refresh failure does not fail import.
        }
        // LCOV_EXCL_STOP
      }
    }

    // A newly recovered repository must pull before its first local metadata
    // commit; otherwise that commit makes an otherwise empty checkout diverge
    // from the hinted remote before recovery has even begun.
    if (const auto imported = repo.get(metadata.project_id); imported.has_value()) {
      persist_project_metadata(context, *imported, "Restore encrypted project metadata");
    }

    nlohmann::json body = {
        {"project_id", metadata.project_id},
        {"project_created", project_created},
        {"remote_hint_present", remote_hint_present},
        {"remote_configured", remote_configured},
        {"remote_error", remote_error.empty() ? nlohmann::json(nullptr) : nlohmann::json(remote_error)},
        {"pull_status", pull_status},
        {"pull_error", pull_error.empty() ? nlohmann::json(nullptr) : nlohmann::json(pull_error)},
    };

    auto* out = duplicate_string(body.dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
    }
    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");  // LCOV_EXCL_LINE
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);  // LCOV_EXCL_LINE
  }  // LCOV_EXCL_LINE
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
