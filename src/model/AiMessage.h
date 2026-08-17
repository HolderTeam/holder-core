#pragma once

#include <optional>
#include <string>

namespace holder::model {

struct AiMessage {
  std::string message_id;
  std::string thread_id;
  std::string role;
  std::string source;
  std::optional<std::string> provider;
  std::optional<std::string> model;
  std::string content;
  long long created_at = 0;
  std::optional<long long> deleted_at;
  std::optional<std::string> prompt_hash;
  std::optional<std::string> meta_json;
};

} // namespace holder::model
