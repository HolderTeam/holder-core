#pragma once

#include <string>

namespace holder::core {

// Compute cards/<first2>/<next2>/<uuid>.md
std::string card_rel_path(const std::string& card_id);
// Compute trash/cards/<first2>/<next2>/<uuid>.md
std::string card_trash_rel_path(const std::string& card_id);

} // namespace holder::core
