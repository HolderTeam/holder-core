#include "resource/ResourceStore.h"

#include "card/CardFrontMatter.h"
#include "card/CardPaths.h"
#include "card/CardRepo.h"
#include "card/LinkRepo.h"
#include "platform/Tx.h"
#include "privacy/ProjectPrivacy.h"
#include "project/Rebuilder.h"
#include "resource/ResourceManifest.h"
#include "resource/ResourcePaths.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <unordered_set>
#include <vector>

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

const std::string& project_key_id(const holder::model::Project& project) {
  if (!project.project_key_id.has_value() || project.project_key_id->empty()) {
    throw std::runtime_error("encrypted project missing project_key_id");
  }
  return *project.project_key_id;
}

std::string encode_manifest(
    const holder::model::Project& project,
    const std::string& plaintext
) {
  if (project.privacy_mode != "encrypted_git") return plaintext;
  return holder::privacy::encrypt_project_blob(
      project.project_id, project_key_id(project), plaintext
  );
}

std::string decode_manifest(
    const holder::model::Project& project,
    const std::string& raw
) {
  if (project.privacy_mode != "encrypted_git") return raw;
  return holder::privacy::decrypt_project_blob(
      project.project_id, project_key_id(project), raw
  );
}

long long now_epoch_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch()
  ).count();
}

} // namespace

ResourceStore::ResourceStore(
    holder::platform::Db& db,
    holder::core::Fs* fs,
    holder::git::GitOps* git
)
    : db_(db),
      fs_(&resolve_fs(fs)),
      git_(&resolve_git(git)),
      project_repo_(db),
      resource_repo_(db) {}

holder::model::Project ResourceStore::require_project(const std::string& project_id) {
  const auto project = project_repo_.get(project_id);
  if (!project.has_value()) throw std::runtime_error("project not found: " + project_id);
  git_->open_or_init(project->root_path);
  if (project->git_remote_url.has_value()) git_->set_remote("origin", *project->git_remote_url);
  return *project;
}

void ResourceStore::put(const holder::model::ResourceBundle& bundle) {
  const auto project = require_project(bundle.resource.project_id);
  const auto path = resource_rel_path(bundle.resource.resource_id);
  const auto encoded = encode_manifest(project, render_resource_manifest(bundle));
  git_->write_file(path, encoded);
  git_->stage_path(path);
  if (project.privacy_mode == "encrypted_git") {
    holder::privacy::assert_encryption_index_paths_safe(project.root_path, {path});
  }
  git_->commit("Update resource " + bundle.resource.label);

  try {
    holder::platform::Tx tx(db_);
    resource_repo_.put_bundle(bundle);
    tx.commit();
  } catch (...) {
    // The Git commit is already authoritative. Reconstruct the projection before surfacing any
    // remaining rebuild error to the caller.
    holder::store::Rebuilder(db_, nullptr).rebuild_project(project);
  }
}

std::optional<holder::model::ResourceBundle> ResourceStore::get(
    const std::string& resource_id
) const {
  return resource_repo_.get_bundle(resource_id);
}

void ResourceStore::remove(const std::string& resource_id) {
  const auto bundle = resource_repo_.get_bundle(resource_id);
  if (!bundle.has_value()) throw std::runtime_error("resource not found: " + resource_id);
  const auto project = require_project(bundle->resource.project_id);
  const auto path = resource_rel_path(resource_id);
  holder::card::CardRepo cards(db_);
  holder::card::LinkRepo links(db_);
  const auto backlinks = links.list_backlinks_typed(project.project_id, resource_id, "resource");
  std::unordered_set<std::string> changed_cards;
  std::vector<std::string> staged_paths;
  const auto now = now_epoch_seconds();
  for (const auto& backlink : backlinks) {
    if (changed_cards.contains(backlink.from_card_id)) continue;
    const auto card = cards.get(backlink.from_card_id);
    if (!card.has_value() || card->deleted_at.has_value()) continue;
    changed_cards.insert(backlink.from_card_id);
    const auto card_path = holder::core::card_rel_path(card->card_id);
    if (card->rel_path != card_path) throw std::runtime_error("card rel_path does not match card_id");
    const auto parsed = holder::core::parse_card_file(
        decode_manifest(project, fs_->read_file(git_->repo_dir() / card_path))
    );
    auto kept = parsed.links;
    kept.erase(
        std::remove_if(kept.begin(), kept.end(), [&](const auto& link) {
          return link.to_type == "resource" && link.to_card_id == resource_id;
        }),
        kept.end()
    );
    auto updated = *card;
    updated.updated_at = now;
    git_->write_file(
        card_path,
        encode_manifest(
            project,
            holder::core::render_card_front_matter(updated, kept, parsed.milestones) + parsed.body
        )
    );
    git_->stage_path(card_path);
    staged_paths.push_back(card_path);
  }
  git_->remove_path(path);
  staged_paths.push_back(path);
  if (project.privacy_mode == "encrypted_git") {
    // The deleted path is no longer in the index; only rewritten Card blobs need checking.
    staged_paths.pop_back();
    holder::privacy::assert_encryption_index_paths_safe(project.root_path, staged_paths);
  }
  git_->commit("Remove resource " + bundle->resource.label);
  try {
    holder::platform::Tx tx(db_);
    links.delete_links_to_typed(project.project_id, resource_id, "resource");
    for (const auto& card_id : changed_cards) cards.touch_updated(card_id, now);
    resource_repo_.remove(resource_id);
    tx.commit();
  } catch (...) {
    holder::store::Rebuilder(db_, nullptr).rebuild_project(project);
  }
}

} // namespace holder::resource
