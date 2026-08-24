#pragma once

#include "git/GitOps.h"
#include "model/AiThread.h"
#include "model/Project.h"

#include <filesystem>
#include <string>

namespace holder::ai {

std::string ai_thread_manifest_rel_path(const std::string& thread_id);
std::string render_ai_thread_manifest(
    const holder::model::Project& project,
    const holder::model::AiThread& thread
);
holder::model::AiThread parse_ai_thread_manifest(
    const holder::model::Project& project,
    const std::string& raw
);
void write_ai_thread_manifest(
    holder::git::GitOps& git,
    const holder::model::Project& project,
    const holder::model::AiThread& thread
);
holder::model::AiThread read_ai_thread_manifest(
    const holder::model::Project& project,
    const std::filesystem::path& path
);

} // namespace holder::ai
