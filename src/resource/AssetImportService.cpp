#include "resource/AssetImportService.h"

#include "card/CardFrontMatter.h"
#include "card/CardPaths.h"
#include "card/CardRepo.h"
#include "card/LinkRepo.h"
#include "platform/Tx.h"
#include "privacy/ProjectPrivacy.h"
#include "project/ProjectRepo.h"
#include "project/Rebuilder.h"
#include "resource/AssetEnvelope.h"
#include "resource/LocationRepo.h"
#include "resource/ResourceManifest.h"
#include "resource/ResourcePaths.h"
#include "resource/ResourceRepo.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace holder::resource {
namespace {

holder::core::Fs& resolve_fs(holder::core::Fs* fs) {
  static holder::core::RealFs real_fs;
  return fs ? *fs : real_fs;
}

holder::git::GitOps& resolve_git(holder::git::GitOps* git) {
  static holder::git::RealGitOps real_git;
  return git ? *git : real_git;
}

std::string lower_extension(const std::filesystem::path& path) {
  auto extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return extension;
}

std::pair<std::string, std::string> classify(const std::filesystem::path& path) {
  const auto extension = lower_extension(path);
  if (extension == ".jpg" || extension == ".jpeg") return {"image", "image/jpeg"};
  if (extension == ".png") return {"image", "image/png"};
  if (extension == ".gif") return {"image", "image/gif"};
  if (extension == ".webp") return {"image", "image/webp"};
  if (extension == ".svg") return {"image", "image/svg+xml"};
  if (extension == ".pdf") return {"document", "application/pdf"};
  if (extension == ".txt" || extension == ".md") return {"document", "text/plain"};
  return {"thing", "application/octet-stream"};
}

std::string encode_project_blob(
    const holder::model::Project& project,
    const std::string& plain
) {
  if (project.privacy_mode != "encrypted_git") return plain;
  if (!project.project_key_id.has_value() || project.project_key_id->empty()) {
    throw std::runtime_error("encrypted project missing project_key_id");
  }
  return holder::privacy::encrypt_project_blob(project.project_id, *project.project_key_id, plain);
}

std::string decode_project_blob(
    const holder::model::Project& project,
    const std::string& raw
) {
  if (project.privacy_mode != "encrypted_git") return raw;
  if (!project.project_key_id.has_value() || project.project_key_id->empty()) {
    throw std::runtime_error("encrypted project missing project_key_id");
  }
  return holder::privacy::decrypt_project_blob(project.project_id, *project.project_key_id, raw);
}

std::string object_key_for(
    const holder::model::Location& location,
    const std::string& project_id,
    const std::string& asset_id
) {
  auto prefix = location.configuration.contains("prefix")
                    ? location.configuration.at("prefix")
                    : std::string();
  while (!prefix.empty() && prefix.front() == '/') prefix.erase(prefix.begin());
  while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
  if (!prefix.empty()) prefix += "/";
  return prefix + project_id + "/" + asset_id + ".holderasset";
}

bool has_resource_link(
    const std::vector<holder::model::CardLink>& links,
    const std::string& resource_id
) {
  return std::any_of(links.begin(), links.end(), [&](const auto& link) {
    return link.to_type == "resource" && link.kind == "attachment" &&
           link.to_card_id == resource_id;
  });
}

void remove_quietly(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}

} // namespace

AssetImportService::AssetImportService(
    holder::platform::Db& db,
    std::filesystem::path staging_root,
    std::function<std::string()> uuid_v4,
    holder::core::Fs* fs,
    holder::git::GitOps* git,
    std::function<void(AssetImportStage)> on_stage
)
    : db_(db),
      staging_root_(std::move(staging_root)),
      uuid_v4_(std::move(uuid_v4)),
      fs_(&resolve_fs(fs)),
      git_(&resolve_git(git)),
      on_stage_(std::move(on_stage)) {
  if (!uuid_v4_) throw std::invalid_argument("uuid generator is required");
}

AssetImportResult AssetImportService::import_file(
    const AssetImportRequest& request,
    StorageProvider& provider
) {
  if (!std::filesystem::is_regular_file(request.source_file)) {
    throw std::invalid_argument("asset source must be a readable regular file");
  }
  holder::project::ProjectRepo projects(db_);
  const auto project = projects.get(request.project_id);
  if (!project.has_value()) throw std::runtime_error("project not found: " + request.project_id);
  holder::card::CardRepo cards(db_);
  const auto card = cards.get(request.card_id);
  if (!card.has_value() || card->project_id != request.project_id || card->deleted_at.has_value()) {
    throw std::runtime_error("live card not found in project");
  }
  holder::resource::LocationRepo locations(db_);
  const auto location = locations.get(request.location_id);
  if (!location.has_value() || location->project_id != request.project_id) {
    throw std::runtime_error("storage location not found in project");
  }

  git_->open_or_init(project->root_path);
  if (project->git_remote_url.has_value()) git_->set_remote("origin", *project->git_remote_url);
  std::filesystem::create_directories(staging_root_);
  const auto provisional_resource_id = uuid_v4_();
  const auto provisional_asset_id = uuid_v4_();
  const auto staging_file = staging_root_ / (provisional_asset_id + ".staged");
  if (on_stage_) on_stage_(AssetImportStage::Staging);
  const auto staged = stage_asset_file(
      request.source_file,
      staging_file,
      *project,
      provisional_resource_id,
      provisional_asset_id
  );

  holder::resource::ResourceRepo resources(db_);
  const auto duplicate = resources.find_by_asset_hash(request.project_id, staged.plaintext.sha256);
  const std::string resource_id =
      duplicate.has_value() ? duplicate->resource.resource_id : provisional_resource_id;
  const std::string asset_id = duplicate.has_value() ? duplicate->assets.front().asset_id
                                                      : provisional_asset_id;

  holder::card::LinkRepo link_repo(db_);
  auto links = link_repo.list_outgoing(request.project_id, request.card_id);
  const bool link_exists = has_resource_link(links, resource_id);
  if (!link_exists) {
    holder::model::CardLink link;
    link.project_id = request.project_id;
    link.from_card_id = request.card_id;
    link.to_card_id = resource_id;
    link.to_type = "resource";
    link.kind = "attachment";
    link.label = request.source_file.filename().string();
    link.created_at = request.now;
    links.push_back(std::move(link));
  }

  const auto card_path = holder::core::card_rel_path(card->card_id);
  if (card->rel_path != card_path) {
    remove_quietly(staging_file);
    throw std::runtime_error("card rel_path does not match card_id");
  }
  const auto raw_card = fs_->read_file(git_->repo_dir() / card_path);
  const auto parsed_card = holder::core::parse_card_file(decode_project_blob(*project, raw_card));
  auto updated_card = *card;
  updated_card.updated_at = request.now;
  const auto updated_card_blob = encode_project_blob(
      *project,
      holder::core::render_card_front_matter(updated_card, links, parsed_card.milestones) +
          parsed_card.body
  );

  holder::model::ResourceBundle bundle;
  std::string object_key;
  bool stored_new_object = false;
  bool git_committed = false;
  try {
    if (!duplicate.has_value()) {
      const auto [resource_type, media_type] = classify(request.source_file);
      bundle.resource.resource_id = resource_id;
      bundle.resource.project_id = request.project_id;
      bundle.resource.type = resource_type;
      bundle.resource.label = request.source_file.filename().string();
      bundle.resource.created_at = request.now;
      bundle.resource.updated_at = request.now;

      holder::model::Asset asset;
      asset.asset_id = asset_id;
      asset.resource_id = resource_id;
      asset.original_filename = request.source_file.filename().string();
      asset.media_type = media_type;
      asset.byte_size = staged.plaintext.byte_size;
      asset.plaintext_sha256 = staged.plaintext.sha256;
      asset.created_at = request.now;
      asset.updated_at = request.now;

      holder::model::Placement placement;
      placement.placement_id = uuid_v4_();
      placement.asset_id = asset_id;
      placement.location_id = request.location_id;
      placement.object_key = object_key_for(*location, request.project_id, asset_id);
      placement.encoding = staged.encoding;
      placement.stored_byte_size = staged.stored.byte_size;
      placement.stored_sha256 = staged.stored.sha256;
      placement.created_at = request.now;
      object_key = placement.object_key;
      asset.placements.push_back(placement);
      bundle.assets.push_back(asset);

      if (on_stage_) on_stage_(AssetImportStage::Storing);
      provider.put(object_key, staging_file, staged.stored.byte_size, staged.stored.sha256);
      if (!provider.exists(object_key)) {
        throw StorageError(StorageErrorCode::Integrity, "stored object did not become available");
      }
      stored_new_object = true;
      const auto resource_path = holder::resource::resource_rel_path(resource_id);
      git_->write_file(
          resource_path,
          encode_project_blob(*project, holder::resource::render_resource_manifest(bundle))
      );
      git_->stage_path(resource_path);
    }

    if (on_stage_) on_stage_(AssetImportStage::Committing);
    if (!link_exists) {
      git_->write_file(card_path, updated_card_blob);
      git_->stage_path(card_path);
    }
    if (project->privacy_mode == "encrypted_git") {
      std::vector<std::string> paths;
      if (!duplicate.has_value()) paths.push_back(holder::resource::resource_rel_path(resource_id));
      if (!link_exists) paths.push_back(card_path);
      holder::privacy::assert_encryption_index_paths_safe(project->root_path, paths);
    }
    if (!duplicate.has_value() || !link_exists) {
      git_->commit("Attach " + request.source_file.filename().string());
      git_committed = true;
    }

    try {
      holder::platform::Tx tx(db_);
      if (!duplicate.has_value()) resources.put_bundle(bundle);
      if (!link_exists) {
        link_repo.upsert_links(request.project_id, request.card_id, links);
        cards.touch_updated(request.card_id, request.now);
      }
      tx.commit();
    } catch (...) {
      if (!git_committed) throw;
      holder::store::Rebuilder(db_, nullptr).rebuild_project(*project);
    }
  } catch (...) {
    remove_quietly(staging_file);
    if (stored_new_object && !git_committed) {
      try {
        provider.remove(object_key);
      } catch (...) {
      }
    }
    throw;
  }
  remove_quietly(staging_file);
  return {
      .resource_id = resource_id,
      .asset_id = asset_id,
      .duplicate_reused = duplicate.has_value(),
      .link_created = !link_exists,
  };
}

void AssetImportService::retrieve(
    const std::string& resource_id,
    const std::string& asset_id,
    const std::string& placement_id,
    StorageProvider& provider,
    const std::filesystem::path& destination
) {
  holder::resource::ResourceRepo resources(db_);
  const auto bundle = resources.get_bundle(resource_id);
  if (!bundle.has_value()) throw std::runtime_error("resource not found");
  const auto asset = std::find_if(bundle->assets.begin(), bundle->assets.end(), [&](const auto& item) {
    return item.asset_id == asset_id;
  });
  if (asset == bundle->assets.end()) throw std::runtime_error("asset not found in resource");
  const auto placement = std::find_if(
      asset->placements.begin(), asset->placements.end(), [&](const auto& item) {
        return item.placement_id == placement_id;
      }
  );
  if (placement == asset->placements.end()) throw std::runtime_error("placement not found in asset");
  const auto project = holder::project::ProjectRepo(db_).get(bundle->resource.project_id);
  if (!project.has_value()) throw std::runtime_error("project not found");
  std::filesystem::create_directories(staging_root_);
  const auto download = staging_root_ / (placement->placement_id + ".download");
  try {
    provider.get(placement->object_key, download);
    recover_asset_file(
        download,
        destination,
        *project,
        resource_id,
        asset_id,
        placement->encoding,
        {placement->stored_byte_size, placement->stored_sha256},
        {asset->byte_size, asset->plaintext_sha256}
    );
    remove_quietly(download);
  } catch (...) {
    remove_quietly(download);
    remove_quietly(destination);
    throw;
  }
}

} // namespace holder::resource
