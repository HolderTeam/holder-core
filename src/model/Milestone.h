#pragma once

#include <optional>
#include <string>

namespace holder::model {

struct Milestone {
  std::string milestone_id;
  std::string project_id;
  std::string card_id;
  long long start_at = 0;
  std::optional<long long> end_at;
  bool all_day = false;
  std::optional<std::string> kind;
  std::optional<std::string> description;
  long long created_at = 0;
  long long updated_at = 0;
};

} // namespace holder::model
