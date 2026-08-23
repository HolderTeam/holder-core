#pragma once

#include "model/Card.h"
#include "model/CardLink.h"
#include "model/Milestone.h"

#include <string>
#include <vector>

namespace holder::core {

struct ParsedCardFile {
  holder::model::Card card;
  std::vector<holder::model::CardLink> links;
  std::vector<holder::model::Milestone> milestones;
  std::string body;
  bool has_front_matter = false;
};

ParsedCardFile parse_card_file(const std::string& raw);
std::string render_card_front_matter(
    const holder::model::Card& card,
    const std::vector<holder::model::CardLink>& links,
    const std::vector<holder::model::Milestone>& milestones
);

} // namespace holder::core
