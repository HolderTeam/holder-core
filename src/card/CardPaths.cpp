#include "card/CardPaths.h"

#include <stdexcept>

namespace holder::core {

std::string card_rel_path(const std::string& card_id) {
  if (card_id.size() < 4) {
    throw std::runtime_error("card_id too short for path sharding");
  }

  const std::string first = card_id.substr(0, 2);
  const std::string second = card_id.substr(2, 2);
  return "cards/" + first + "/" + second + "/" + card_id + ".md";
}

std::string card_trash_rel_path(const std::string& card_id) {
  return "trash/" + card_rel_path(card_id);
}

} // namespace holder::core
