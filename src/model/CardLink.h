#pragma once

#include <optional>
#include <string>

namespace holder::model {

struct CardLink {
  std::string project_id;
  std::string from_card_id;
  std::string to_card_id;
  std::string to_type;
  std::string kind;
  std::optional<std::string> label;
  long long created_at = 0;
};

} // namespace holder::model
