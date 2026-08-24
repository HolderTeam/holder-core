#include "resource/ResourcePaths.h"

#include <stdexcept>

namespace holder::resource {
namespace {

std::string sharded_path(
    const std::string& root,
    const std::string& id,
    const std::string& extension
) {
  if (id.size() < 4) {
    throw std::runtime_error("id too short for path sharding");
  }
  return root + "/" + id.substr(0, 2) + "/" + id.substr(2, 2) + "/" + id + extension;
}

} // namespace

std::string resource_rel_path(const std::string& resource_id) {
  return sharded_path("resources", resource_id, ".json");
}

std::string location_rel_path(const std::string& location_id) {
  return sharded_path("locations", location_id, ".json");
}

} // namespace holder::resource
