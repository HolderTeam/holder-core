#pragma once

#include <optional>
#include <string>

namespace holder::model {

struct Resource {
  std::string resource_id;
  std::string project_id;
  std::string kind;
  std::string uri;
  std::string label;
  std::optional<std::string> desc;
  long long created_at = 0;
  long long updated_at = 0;
};

} // namespace holder::model
