#pragma once

#include <map>
#include <string>

namespace holder::model {

struct Location {
  std::string location_id;
  std::string project_id;
  std::string name;
  std::string provider;
  std::map<std::string, std::string> configuration;
  long long created_at = 0;
  long long updated_at = 0;
};

} // namespace holder::model
