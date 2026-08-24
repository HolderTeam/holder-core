#pragma once

#include <map>
#include <string>
#include <vector>

namespace holder::model {

using ResourceMetadata = std::map<std::string, std::vector<std::string>>;

struct Placement {
  std::string placement_id;
  std::string asset_id;
  std::string location_id;
  std::string object_key;
  std::string encoding;
  long long stored_byte_size = 0;
  std::string stored_sha256;
  long long created_at = 0;
};

struct Asset {
  std::string asset_id;
  std::string resource_id;
  std::string original_filename;
  std::string media_type;
  long long byte_size = 0;
  std::string plaintext_sha256;
  long long created_at = 0;
  long long updated_at = 0;
  std::vector<Placement> placements;
};

struct Resource {
  std::string resource_id;
  std::string project_id;
  std::string type;
  std::string label;
  ResourceMetadata metadata;
  long long created_at = 0;
  long long updated_at = 0;
};

struct ResourceBundle {
  Resource resource;
  std::vector<Asset> assets;
};

} // namespace holder::model
