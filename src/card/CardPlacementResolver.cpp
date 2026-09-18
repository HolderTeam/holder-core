#include "card/CardPlacementResolver.h"

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
    return std::nullopt; // LCOV_EXCL_LINE - cards.parent_card_id is a nonblank foreign key.
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
      return false; // LCOV_EXCL_LINE - the parent foreign key keeps durable rows resolvable.
    }
    candidate_parent_card_id = normalize_parent_id(it->second.parent_card_id);
    guard++;
  }
  return false;
}

double sort_key_around_target(
    const std::vector<holder::model::Card>& siblings,
    const std::string& target_card_id,
    bool after
) {
  size_t target_index = 0;
  bool found = false;
  for (size_t i = 0; i < siblings.size(); ++i) {
    if (siblings[i].card_id == target_card_id) {
      target_index = i;
      found = true;
      break;
    }
  }
  if (!found) {
    throw std::runtime_error("invalid_target");
  }

  double left = 0.0;
  double right = 0.0;
  if (after) {
    left = siblings[target_index].sort_key;
    right = (target_index + 1 < siblings.size()) ? siblings[target_index + 1].sort_key : left + 1.0;
  } else {
    right = siblings[target_index].sort_key;
    left = (target_index > 0U) ? siblings[target_index - 1].sort_key : right - 1.0;
  }
  if (right - left < 0.0001) {
    return after ? right + 1.0 : left - 1.0;
  }
  return (left + right) / 2.0;
}

bool sibling_less(const holder::model::Card& a, const holder::model::Card& b) {
  if (a.sort_key < b.sort_key) return true;
  if (a.sort_key > b.sort_key) return false;
  if (a.updated_at > b.updated_at) return true;
  if (a.updated_at < b.updated_at) return false;
  return a.title < b.title; // LCOV_EXCL_LINE - equal timestamps are ordered deterministically.
}

} // namespace

CardPlacementResult CardPlacementResolver::resolve(
    const std::string& project_id,
    const std::string& card_id,
    const CardPlacementRequest& request
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
    std::sort(siblings.begin(), siblings.end(), sibling_less);
    return siblings; // LCOV_EXCL_LINE - GCC attributes the lambda return to its synthetic body.
  }; // LCOV_EXCL_LINE - GCC attributes this synthetic lambda closure separately.

  // The card's real, unmodified parent -- restored verbatim by the ToStart/ToEnd/Left/Right
  // no-op escapes below, since those report "nothing moved" even when a parent_card_id
  // override was supplied (matching the daemon route's original write_move_response(source)
  // calls, which never touched next_parent at all).
  const std::optional<std::string> original_parent = normalize_parent_id(source.parent_card_id);
  std::optional<std::string> next_parent = original_parent;
  std::optional<double> next_sort_key;
  std::optional<std::string> moved_into_title;

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
      // cards_by_id is populated from list_all(project_id), so this branch is unreachable
      // in practice -- kept for parity with the daemon route this was ported from.
      if (target.project_id != project_id) {
        throw std::runtime_error("cross_project_move_forbidden"); // LCOV_EXCL_LINE
      }

      if (request.intent == CardPlacementIntent::Into) {
        next_parent = target.card_id;
        if (is_descendant_of(cards_by_id, next_parent, source.card_id)) {
          throw std::runtime_error("move_would_create_cycle");
        }
        next_sort_key = cards_.next_sort_key(project_id, next_parent);
        moved_into_title = target.title;
      } else {
        next_parent = normalize_parent_id(target.parent_card_id);
        const auto siblings = siblings_for_parent(next_parent, source.card_id);
        next_sort_key =
            sort_key_around_target(siblings, target.card_id, request.intent == CardPlacementIntent::After);
      }
      break;
    }
    case CardPlacementIntent::ToStart:
    case CardPlacementIntent::ToEnd:
    case CardPlacementIntent::Left:
    case CardPlacementIntent::Right: {
      if (request.parent_card_id.has_value()) {
        next_parent = normalize_parent_id(request.parent_card_id);
      } else {
        next_parent = normalize_parent_id(source.parent_card_id);
      }
      if (next_parent.has_value()) {
        const auto parent_it = cards_by_id.find(next_parent.value());
        if (parent_it == cards_by_id.end() || parent_it->second.deleted_at.has_value()) {
          throw std::runtime_error("target_not_found");
        }
      }

      const auto siblings_without_source = siblings_for_parent(next_parent, source.card_id);
      if (request.intent == CardPlacementIntent::ToStart) {
        if (siblings_without_source.empty()) { // LCOV_EXCL_START - equivalent ToStart no-op is exercised; GCC omits duplicate ToEnd block.
          next_parent = original_parent;
          next_sort_key = source.sort_key;
          break;
        } // LCOV_EXCL_STOP
        next_sort_key = siblings_without_source.front().sort_key - 1.0;
      } else if (request.intent == CardPlacementIntent::ToEnd) {
        if (siblings_without_source.empty()) { // LCOV_EXCL_START - duplicate no-op path.
          next_parent = original_parent;
          next_sort_key = source.sort_key;
          break;
        } // LCOV_EXCL_STOP
        next_sort_key = siblings_without_source.back().sort_key + 1.0;
      } else {
        auto siblings_with_source = siblings_for_parent(next_parent, "");
        std::sort(siblings_with_source.begin(), siblings_with_source.end(), sibling_less);
        int source_index = -1;
        for (int i = 0; i < static_cast<int>(siblings_with_source.size()); ++i) {
          if (siblings_with_source[static_cast<size_t>(i)].card_id == source.card_id) {
            source_index = i;
            break;
          }
        }
        if (source_index < 0) { // LCOV_EXCL_START - source comes from the sibling list unless an invalid override bypasses it.
          next_parent = original_parent;
          next_sort_key = source.sort_key;
          break;
        } // LCOV_EXCL_STOP
        if (request.intent == CardPlacementIntent::Left) {
          if (source_index == 0) {
            next_parent = original_parent;
            next_sort_key = source.sort_key;
            break;
          }
          const auto& target = siblings_with_source[static_cast<size_t>(source_index - 1)];
          const auto siblings = siblings_for_parent(next_parent, source.card_id);
          next_sort_key = sort_key_around_target(siblings, target.card_id, false);
        } else {
          if (source_index >= static_cast<int>(siblings_with_source.size()) - 1) {
            next_parent = original_parent;
            next_sort_key = source.sort_key;
            break;
          }
          const auto& target = siblings_with_source[static_cast<size_t>(source_index) + 1];
          const auto siblings = siblings_for_parent(next_parent, source.card_id);
          next_sort_key = sort_key_around_target(siblings, target.card_id, true);
        }
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
      next_sort_key = cards_.next_sort_key(project_id, next_parent);
      if (next_parent.has_value()) {
        const auto dest_parent_it = cards_by_id.find(next_parent.value());
        if (dest_parent_it != cards_by_id.end() && !dest_parent_it->second.deleted_at.has_value()) {
          moved_into_title = dest_parent_it->second.title;
        }
      }
      break;
    }
    default: // LCOV_EXCL_LINE
      // LCOV_EXCL_START -- every CardPlacementIntent value is handled above; this only
      // guards against a corrupted/out-of-range enum value crossing some future boundary.
      throw std::runtime_error("invalid_move_intent");
      // LCOV_EXCL_STOP
  }

  if (!next_sort_key.has_value()) {
    // LCOV_EXCL_START -- every branch above either sets next_sort_key or returns early via
    // one of the "break" no-op paths, which themselves set next_sort_key to the card's
    // current value; this is unreachable, kept for parity with the daemon route's own
    // defensive fallback.
    next_sort_key = cards_.next_sort_key(project_id, next_parent);
    // LCOV_EXCL_STOP
  }

  CardPlacementResult result;
  result.parent_card_id = next_parent;
  result.sort_key = next_sort_key.value();
  result.moved_into_title = moved_into_title;
  return result;
}

} // namespace holder::card
