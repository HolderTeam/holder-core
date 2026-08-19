#pragma once

#include "model/Project.h"
#include "platform/Db.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace holder::git {
class GitOps;
} // namespace holder::git

namespace holder::index {
class FtsIndexer;
} // namespace holder::index

namespace holder::project {

// If any project already exists, does nothing and returns std::nullopt.
// Otherwise creates a project named `name` (with the given privacy_mode) and
// a single card (`welcome_title` / `welcome_content`) in it, returning the
// created project.
std::optional<holder::model::Project> ensure_default_project(
    holder::platform::Db& db,
    const std::string& name,
    const std::string& privacy_mode,
    const std::string& welcome_title,
    const std::string& welcome_content,
    const std::function<std::string()>& uuid_v4,
    const std::filesystem::path& projects_root,
    holder::index::FtsIndexer* fts = nullptr,
    holder::git::GitOps* git = nullptr
);

} // namespace holder::project
