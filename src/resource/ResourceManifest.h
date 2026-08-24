#pragma once

#include "model/Location.h"
#include "model/Resource.h"

#include <string>

namespace holder::resource {

std::string render_resource_manifest(const holder::model::ResourceBundle& bundle);
holder::model::ResourceBundle parse_resource_manifest(const std::string& text);

std::string render_location_manifest(const holder::model::Location& location);
holder::model::Location parse_location_manifest(const std::string& text);

} // namespace holder::resource
