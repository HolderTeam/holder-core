#pragma once

#include <optional>
#include <string>

namespace holder::model {

struct AiRunner {
  std::string runner_id;
  std::string name;
  std::string kind;
  std::optional<std::string> base_url;
  std::string source;
  bool enabled = false;
  long long created_at = 0;
  long long updated_at = 0;
};

} // namespace holder::model
