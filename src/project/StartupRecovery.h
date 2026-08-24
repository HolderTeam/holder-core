#pragma once

#include "index/FtsIndexer.h"
#include "model/Project.h"
#include "platform/Db.h"

#include <filesystem>
#include <functional>
#include <vector>

namespace holder::project {

std::vector<holder::model::Project> recover_projects_from_disk(
    holder::platform::Db& db,
    holder::index::FtsIndexer* fts,
    const std::filesystem::path& projects_root,
    const std::function<std::string()>& uuid_v4,
    bool require_durable_manifest = false
);

// Recovers an explicit set of project roots. This is used with the
// device-local registry so projects outside the managed projects directory do
// not disappear when SQLite is rebuilt.
std::vector<holder::model::Project> recover_project_roots(
    holder::platform::Db& db,
    holder::index::FtsIndexer* fts,
    std::vector<std::filesystem::path> roots,
    const std::function<std::string()>& uuid_v4,
    bool require_durable_manifest = false
);

} // namespace holder::project
