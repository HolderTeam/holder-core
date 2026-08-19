#pragma once

#include "model/Project.h"

#include <filesystem>
#include <string>
#include <vector>

namespace holder::core {

std::string slugify(const std::string& input);

std::string unique_project_root(
    const std::filesystem::path& base_root,
    const std::string& slug,
    const std::vector<holder::model::Project>& existing
);

} // namespace holder::core
