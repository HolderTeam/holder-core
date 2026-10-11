#pragma once

#include "card/CardRepo.h"

namespace holder::card {

// The manual tree order: sort key, newest update, case-insensitive title, then ID.
bool card_tree_less(const holder::model::Card& a, const holder::model::Card& b);

// Walk the entire chain, rejecting cycles. Skip unreachable/deleted ancestors when
// restoring; strict validation is used by create/move under the project lock.
std::optional<std::string> reachable_card_parent(CardRepo& repo, const holder::model::Card& card);
void validate_card_parent(CardRepo& repo, const holder::model::Card& card);

// Return only changed placements. The caller persists them together with the
// lifecycle transition, rather than invoking independently committed moves.
std::vector<holder::model::Card> promoted_card_children(
    CardRepo& repo,
    const holder::model::Card& parent,
    long long updated_at
);
// Places `card` at index `at` of `destination` -- its new live siblings in tree order, without
// the card itself -- under `parent`. The card takes the midpoint between its neighbours when that
// fits; otherwise the smallest surrounding window of siblings is re-spaced. Returns every card
// whose placement changes, the placed card included, each with updated_at set.
std::vector<holder::model::Card> placed_card_changes(
    std::vector<holder::model::Card> destination,
    std::size_t at,
    holder::model::Card card,
    const std::optional<std::string>& parent,
    long long updated_at
);
// Live cards that browsing from the roots cannot reach, and how to make them reachable.
struct HierarchyRepairPlan {
  // Every card whose placement changes: the moved cards and any re-spaced siblings.
  std::vector<holder::model::Card> placements;
  // Hidden branch heads: live cards whose parent is missing or trashed. Each moves to its nearest
  // live, reachable ancestor (or the roots), in the place the hidden branch held there; its live
  // descendants stay attached and become reachable with it.
  std::vector<std::string> moved_card_ids;
  // Live cards whose parent chain loops. Repair leaves them alone; they need a decision.
  std::vector<std::string> cycle_card_ids;
};
HierarchyRepairPlan plan_hierarchy_repair(
    CardRepo& repo,
    const std::string& project_id,
    long long updated_at
);
std::vector<holder::model::Card> restored_card_placement(
    CardRepo& repo,
    holder::model::Card& restored,
    long long updated_at
);

} // namespace holder::card
