#pragma once

#include <optional>
#include <string>

namespace holder::resource {

std::optional<std::string> dublin_core_term_for(const std::string& holder_property);
std::string holder_property_for_dublin_core(const std::string& external_term);

} // namespace holder::resource
