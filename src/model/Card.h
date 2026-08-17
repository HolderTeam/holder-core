#pragma once

#include <optional>
#include <string>

namespace holder::model {

struct Card {
  std::string card_id;
  std::string project_id;
  std::string title;
  std::string rel_path;
  std::optional<std::string> parent_card_id;
  double sort_key = 0.0;
  long long created_at = 0;
  long long updated_at = 0;
  std::optional<long long> deleted_at;
};

} // namespace holder::model
