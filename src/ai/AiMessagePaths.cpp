#include "ai/AiMessagePaths.h"

#include <stdexcept>

namespace holder::core {

std::string ai_message_rel_path(const std::string& message_id) {
  if (message_id.size() < 4) {
    throw std::runtime_error("message_id too short for path sharding");
  }

  const std::string first = message_id.substr(0, 2);
  const std::string second = message_id.substr(2, 2);
  return "ai_messages/" + first + "/" + second + "/" + message_id + ".md";
}

std::string ai_message_trash_rel_path(const std::string& message_id) {
  return "trash/" + ai_message_rel_path(message_id);
}

} // namespace holder::core
