#include "resource/LocationStore.h"

#include "privacy/ProjectPrivacy.h"
#include "project/Rebuilder.h"
#include "resource/ResourceManifest.h"
#include "resource/ResourcePaths.h"

#include <stdexcept>

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

std::string encode_manifest(
    const holder::model::Project& project,
    const std::string& plaintext
) {
  if (project.privacy_mode != "encrypted_git") return plaintext;
  if (!project.project_key_id.has_value() || project.project_key_id->empty()) {
    throw std::runtime_error("encrypted project missing project_key_id");
  }
  return holder::privacy::encrypt_project_blob(
      project.project_id, *project.project_key_id, plaintext
  );
}

} // namespace

LocationStore::LocationStore(
    holder::platform::Db& db,
    holder::core::Fs* fs,
    holder::git::GitOps* git
)
    : db_(db),
      fs_(&resolve_fs(fs)),
      git_(&resolve_git(git)),
      project_repo_(db),
      location_repo_(db) {}

holder::model::Project LocationStore::require_project(const std::string& project_id) {
  const auto project = project_repo_.get(project_id);
  if (!project.has_value()) throw std::runtime_error("project not found: " + project_id);
  git_->open_or_init(project->root_path);
  if (project->git_remote_url.has_value()) git_->set_remote("origin", *project->git_remote_url);
  return *project;
}

void LocationStore::put(const holder::model::Location& location) {
  const auto project = require_project(location.project_id);
  const auto path = location_rel_path(location.location_id);
  git_->write_file(path, encode_manifest(project, render_location_manifest(location)));
  git_->stage_path(path);
  if (project.privacy_mode == "encrypted_git") {
    holder::privacy::assert_encryption_index_paths_safe(project.root_path, {path});
  }
  git_->commit("Update storage location " + location.name);
  try {
    location_repo_.put(location);
  } catch (...) {
    holder::store::Rebuilder(db_, nullptr).rebuild_project(project);
  }
}

std::optional<holder::model::Location> LocationStore::get(const std::string& location_id) const {
  return location_repo_.get(location_id);
}

void LocationStore::remove(const std::string& location_id) {
  const auto location = location_repo_.get(location_id);
  if (!location.has_value()) throw std::runtime_error("location not found: " + location_id);
  if (location_repo_.is_in_use(location_id)) {
    throw std::runtime_error("storage location is in use by an asset placement");
  }
  const auto project = require_project(location->project_id);
  const auto path = location_rel_path(location_id);
  git_->remove_path(path);
  git_->commit("Remove storage location " + location->name);
  try {
    location_repo_.remove(location_id);
  } catch (...) {
    holder::store::Rebuilder(db_, nullptr).rebuild_project(project);
  }
}

} // namespace holder::resource
