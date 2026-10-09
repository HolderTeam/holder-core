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
std::vector<holder::model::Card> restored_card_placement(
    CardRepo& repo,
    holder::model::Card& restored,
    long long updated_at
);

} // namespace holder::card
