#pragma once

#include <string>

namespace holder::resource {

std::string resource_rel_path(const std::string& resource_id);
std::string location_rel_path(const std::string& location_id);

} // namespace holder::resource
