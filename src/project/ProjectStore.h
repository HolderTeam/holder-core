#pragma once

#include "git/GitOps.h"
#include "model/Project.h"
#include "platform/Db.h"
#include "project/ProjectRepo.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace holder::project {

// Owns the full "create a project" sequence: id/timestamp defaulting, root_path
// derivation, persistence, and git/privacy setup, with rollback on failure.
class ProjectStore {
 public:
  explicit ProjectStore(holder::platform::Db& db, holder::git::GitOps* git = nullptr);

  // project.project_id/created_at/updated_at are generated when unset. If
  // project.root_path is empty, projects_root must be provided and a unique
  // directory is derived from the project name under it (see ProjectPaths.h).
  // A git repo and durable project manifest are initialized for every project.
  // Encrypted projects also get key material via
  // ensure_encrypted_project_ready. The persisted row is removed if
  // git/privacy/manifest setup throws.
  holder::model::Project create(
      holder::model::Project project,
      const std::function<std::string()>& uuid_v4,
      const std::optional<std::filesystem::path>& projects_root = std::nullopt
  );

 private:
  ProjectRepo repo_;
  holder::git::GitOps* git_;
};

} // namespace holder::project
