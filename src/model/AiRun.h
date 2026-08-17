#pragma once

#include <optional>
#include <string>

namespace holder::model {

struct AiRun {
  std::string run_id;
  std::optional<std::string> project_id;
  std::optional<std::string> thread_id;
  std::optional<std::string> message_id;
  std::string mode;
  std::string prompt;
  std::optional<std::string> context_json;
  std::optional<std::string> router_model;
  std::optional<std::string> ranked_json;
  std::optional<std::string> policy_trace_json;
  std::optional<std::string> chosen_model;
  std::string status;
  std::optional<std::string> error;
  long long created_at = 0;
  long long updated_at = 0;
};

} // namespace holder::model
