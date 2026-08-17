#pragma once

#include <optional>
#include <string>

namespace holder::model {

struct AiThread {
  std::string thread_id;
  std::string project_id;
  std::optional<std::string> card_id;
  std::string title;
  long long created_at = 0;
  long long updated_at = 0;
};

} // namespace holder::model
