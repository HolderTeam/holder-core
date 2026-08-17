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
    const std::function<std::string()>& uuid_v4
);

} // namespace holder::project
