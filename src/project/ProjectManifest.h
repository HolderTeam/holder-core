#pragma once

#include "git/GitOps.h"
#include "model/Project.h"

#include <filesystem>
#include <string>

namespace holder::project {

inline constexpr const char* kProjectBootstrapPath = ".holder/privacy.json";
inline constexpr const char* kProjectManifestPath = ".holder/project.json";

// The bootstrap is deliberately clear text and contains only what is needed to
// identify and, when necessary, decrypt a project after the local index is lost.
std::string render_project_bootstrap(const holder::model::Project& project);

// The project manifest is encrypted with the project key for encrypted_git
// projects. It owns user-facing metadata that must not be inferred from a
// directory name during database reconstruction.
std::string render_project_manifest(const holder::model::Project& project);

// Writes and stages both files. The caller owns the surrounding Git commit so
// project metadata can be committed atomically with the operation that changed
// it.
void write_project_manifest(
    holder::git::GitOps& git,
    const holder::model::Project& project
);

// Reads the bootstrap and manifest without consulting SQLite. root_path is
// supplied by device discovery and is never taken from portable project data.
holder::model::Project read_project_manifest(const std::filesystem::path& root_path);

bool has_project_manifest(const std::filesystem::path& root_path);

} // namespace holder::project
