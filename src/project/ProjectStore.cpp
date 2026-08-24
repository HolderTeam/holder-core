#include "project/ProjectStore.h"

#include "privacy/ProjectPrivacy.h"
#include "project/ProjectManifest.h"
#include "project/ProjectPaths.h"

#include <chrono>
#include <stdexcept>

namespace holder::project {
namespace {

holder::git::GitOps& resolve_git(holder::git::GitOps* git) {
  static holder::git::RealGitOps real_git;
  return git ? *git : real_git;
}

long long now_epoch_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch()
  )
      .count();
}

} // namespace

ProjectStore::ProjectStore(holder::platform::Db& db, holder::git::GitOps* git)
    : repo_(db), git_(&resolve_git(git)) {}

holder::model::Project ProjectStore::create(
    holder::model::Project project,
    const std::function<std::string()>& uuid_v4,
    const std::optional<std::filesystem::path>& projects_root
) {
  if (project.project_id.empty()) {
    project.project_id = uuid_v4();
  }
  if (project.created_at <= 0) {
    project.created_at = now_epoch_seconds();
  }
  if (project.updated_at <= 0) {
    project.updated_at = project.created_at;
  }
  if (project.root_path.empty()) {
    if (!projects_root.has_value()) {
      throw std::invalid_argument("root_path or projects_root must be provided");
    }
    const auto slug = holder::core::slugify(project.name);
    project.root_path = holder::core::unique_project_root(*projects_root, slug, repo_.list());
  }

  repo_.create(project);

  try {
    git_->open_or_init(project.root_path);
    if (project.git_remote_url.has_value()) {
      git_->set_remote("origin", *project.git_remote_url);
    }
    if (project.privacy_mode == "encrypted_git") {
      holder::privacy::ensure_encrypted_project_ready(
          *git_,
          repo_,
          project.project_id,
          project.root_path,
          project.project_key_id,
          project.updated_at,
          uuid_v4
      );
    }
    project = repo_.get(project.project_id).value();
    write_project_manifest(*git_, project);
    git_->commit("Create project metadata");
  } catch (...) {
    repo_.remove(project.project_id);
    throw;
  }

  return repo_.get(project.project_id).value();
}

} // namespace holder::project
