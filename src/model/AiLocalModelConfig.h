#pragma once

#include <optional>
#include <string>

namespace holder::model {

struct AiLocalModelConfig {
  std::optional<std::string> fast_model;
  std::optional<std::string> strong_model;
  std::optional<std::string> deep_model;
  long long updated_at = 0;
};

} // namespace holder::model
