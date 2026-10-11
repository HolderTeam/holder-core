#include "card/CardPlacementResolver.h"
#include "card/CardHierarchy.h"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace holder::card {
namespace {

std::optional<std::string> normalize_parent_id(const std::optional<std::string>& parent_card_id) {
  if (!parent_card_id.has_value()) {
    return std::nullopt;
  }
  const std::string& raw = parent_card_id.value();
  const auto start = raw.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return std::nullopt;
  }
  const auto end = raw.find_last_not_of(" \t\r\n");
  return raw.substr(start, end - start + 1);
}

bool is_descendant_of(
    const std::unordered_map<std::string, holder::model::Card>& cards_by_id,
    std::optional<std::string> candidate_parent_card_id,
    const std::string& card_id
) {
  int guard = 0;
  while (candidate_parent_card_id.has_value() && guard < 1024) {
    if (candidate_parent_card_id.value() == card_id) {
      return true;
    }
    const auto it = cards_by_id.find(candidate_parent_card_id.value());
    if (it == cards_by_id.end()) {
      return false;
    }
    candidate_parent_card_id = normalize_parent_id(it->second.parent_card_id);
    guard++;
  }
  return false;
}

} // namespace

CardPlacementResult CardPlacementResolver::resolve(
    const std::string& project_id,
    const std::string& card_id,
    const CardPlacementRequest& request,
    long long updated_at
) const {
  const auto source_opt = cards_.get(card_id);
  if (!source_opt.has_value() || source_opt->deleted_at.has_value()) {
    throw std::runtime_error("card_not_found");
  }
  const auto& source = source_opt.value();
  if (source.project_id != project_id) {
    throw std::runtime_error("cross_project_move_forbidden");
  }

  const auto cards = cards_.list_all(project_id);
  std::unordered_map<std::string, holder::model::Card> cards_by_id;
  cards_by_id.reserve(cards.size());
  for (const auto& c : cards) {
    cards_by_id[c.card_id] = c;
  }

  auto siblings_for_parent = [&](const std::optional<std::string>& parent,
                                 const std::string& exclude_card_id) {
    std::vector<holder::model::Card> siblings;
    for (const auto& c : cards) {
      if (c.deleted_at.has_value()) {
        continue;
      }
      if (c.card_id == exclude_card_id) {
        continue;
      }
      if (normalize_parent_id(c.parent_card_id) == normalize_parent_id(parent)) {
        siblings.push_back(c);
      }
    }
    std::sort(siblings.begin(), siblings.end(), card_tree_less);
    return siblings;
  }; // LCOV_EXCL_LINE - gcov artefact: lambda closer is executed but never counted.

  // The card's real, unmodified parent -- reported unchanged by the no-op escapes below, which
  // report "nothing moved" even when a parent_card_id override was supplied.
  const std::optional<std::string> original_parent = normalize_parent_id(source.parent_card_id);
  std::optional<std::string> next_parent = original_parent;
  std::optional<std::string> moved_into_title;
  // The card's new live siblings, without itself, and where among them it goes. No insertion
  // point means nothing moves.
  std::vector<holder::model::Card> destination;
  std::optional<std::size_t> at;

  switch (request.intent) {
  case CardPlacementIntent::Into:
  case CardPlacementIntent::Before:
  case CardPlacementIntent::After: {
    if (!request.target_card_id.has_value()) {
      throw std::runtime_error("missing_target_card_id");
    }
    const std::string& target_card_id = request.target_card_id.value();
    const auto it = cards_by_id.find(target_card_id);
    if (it == cards_by_id.end() || it->second.deleted_at.has_value()) {
      throw std::runtime_error("target_not_found");
    }
    const auto& target = it->second;
    if (request.intent == CardPlacementIntent::Into) {
      next_parent = target.card_id;
      if (is_descendant_of(cards_by_id, next_parent, source.card_id)) {
        throw std::runtime_error("move_would_create_cycle");
      }
      destination = siblings_for_parent(next_parent, source.card_id);
      at = destination.size();
      moved_into_title = target.title;
    } else {
      next_parent = normalize_parent_id(target.parent_card_id);
      destination = siblings_for_parent(next_parent, source.card_id);
      const auto target_it =
          std::find_if(destination.begin(), destination.end(), [&](const holder::model::Card& c) {
            return c.card_id == target.card_id;
          });
      if (target_it == destination.end()) {
        throw std::runtime_error("invalid_target");
      }
      at = static_cast<std::size_t>(target_it - destination.begin()) +
           (request.intent == CardPlacementIntent::After ? 1U : 0U);
    }
    break;
  }
  case CardPlacementIntent::ToStart:
  case CardPlacementIntent::ToEnd:
  case CardPlacementIntent::Left:
  case CardPlacementIntent::Right: {
    if (request.has_parent_card_id || request.parent_card_id.has_value()) {
      next_parent = normalize_parent_id(request.parent_card_id);
    } else {
      next_parent = original_parent;
    }
    if (next_parent.has_value()) {
      const auto parent_it = cards_by_id.find(next_parent.value());
      if (parent_it == cards_by_id.end() || parent_it->second.deleted_at.has_value()) {
        throw std::runtime_error("target_not_found");
      }
    }

    destination = siblings_for_parent(next_parent, source.card_id);
    if (request.intent == CardPlacementIntent::ToStart ||
        request.intent == CardPlacementIntent::ToEnd) {
      // Alone in its own parent scope there is nothing to reorder past: report "nothing
      // moved". An empty destination elsewhere is still a real move into that scope.
      if (destination.empty() && next_parent == original_parent) {
        break;
      }
      at = request.intent == CardPlacementIntent::ToStart ? 0U : destination.size();
      break;
    }
    const auto with_source = siblings_for_parent(next_parent, "");
    const auto source_it =
        std::find_if(with_source.begin(), with_source.end(), [&](const holder::model::Card& c) {
          return c.card_id == source.card_id;
        });
    // A parent_card_id override naming some other parent leaves the source out of that
    // parent's sibling list, so there is nothing to move left or right of.
    if (source_it == with_source.end()) {
      break;
    }
    const auto index = static_cast<std::size_t>(source_it - with_source.begin());
    if (request.intent == CardPlacementIntent::Left) {
      // Before the previous sibling, which sits at index - 1 once the source is left out.
      if (index > 0U) at = index - 1U;
    } else if (index + 1U < with_source.size()) {
      // After the next sibling, which sits at index once the source is left out.
      at = index + 1U;
    }
    break;
  }
  case CardPlacementIntent::UpLevel: {
    const auto current_parent = normalize_parent_id(source.parent_card_id);
    if (!current_parent.has_value()) {
      throw std::runtime_error("already_at_project_root");
    }
    const auto parent_it = cards_by_id.find(current_parent.value());
    if (parent_it == cards_by_id.end() || parent_it->second.deleted_at.has_value()) {
      next_parent = std::nullopt;
    } else {
      next_parent = normalize_parent_id(parent_it->second.parent_card_id);
    }
    if (next_parent.has_value()) {
      const auto dest_parent_it = cards_by_id.find(next_parent.value());
      if (dest_parent_it != cards_by_id.end() && !dest_parent_it->second.deleted_at.has_value()) {
        moved_into_title = dest_parent_it->second.title;
      }
    }
    destination = siblings_for_parent(next_parent, source.card_id);
    at = destination.size();
    break;
  }
  default:
    // Every CardPlacementIntent value is handled above; this guards against an out-of-range
    // enum value crossing some future boundary.
    throw std::runtime_error("invalid_move_intent");
  }

  CardPlacementResult result;
  result.moved_into_title = moved_into_title;
  if (!at.has_value()) {
    result.parent_card_id = original_parent;
    result.sort_key = source.sort_key;
    return result;
  }
  result.parent_card_id = next_parent;
  for (auto& card :
       placed_card_changes(std::move(destination), *at, source, next_parent, updated_at)) {
    if (card.card_id == source.card_id) {
      result.sort_key = card.sort_key;
    } else {
      result.sibling_changes.push_back(std::move(card));
    }
  }
  return result;
}

} // namespace holder::card
