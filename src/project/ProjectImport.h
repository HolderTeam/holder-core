#pragma once

#include "index/FtsIndexer.h"
#include "model/Project.h"
#include "platform/Db.h"

#include <filesystem>

namespace holder::project {

// Imports durable plain-project files already copied beneath data_dir/projects
// into an empty database. Does not clone, copy files or alter durable source data.
holder::model::Project import_project_into_empty_database(
    holder::platform::Db& db,
    holder::index::FtsIndexer& fts,
    const std::filesystem::path& data_dir,
    const std::filesystem::path& project_root
);

} // namespace holder::project
