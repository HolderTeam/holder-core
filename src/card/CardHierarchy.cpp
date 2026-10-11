#include "card/CardHierarchy.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

namespace holder::card {
namespace {
using Card = holder::model::Card;

std::vector<Card> siblings(CardRepo& repo, const Card& card) {
  auto result = card.parent_card_id ? repo.list_children(card.project_id, *card.parent_card_id)
                                    : repo.list_roots(card.project_id);
  result.erase(
      std::remove_if(
          result.begin(),
          result.end(),
          [](const Card& c) {
            return c.deleted_at.has_value();
          }
      ),
      result.end()
  );
  std::sort(result.begin(), result.end(), card_tree_less);
  return result;
}

// Evenly spaced keys for `count` cards strictly between two neighbours. An open end
// steps by 1.0 from the other bound; with both ends open, keys are 1..count.
std::optional<std::vector<double>> spaced_keys(
    std::optional<double> left,
    std::optional<double> right,
    std::size_t count
) {
  const auto n = static_cast<double>(count);
  if (!left && !right) left = 0.0;
  const double low = left ? *left : *right - n - 1.0;
  const double high = right ? *right : *left + n + 1.0;
  std::vector<double> keys;
  double previous = low;
  for (std::size_t i = 0; i < count; ++i) {
    const double fraction = static_cast<double>(i + 1) / (n + 1.0);
    const double key = low * (1.0 - fraction) + high * fraction;
    if (!std::isfinite(key) || key <= previous || key >= high) return std::nullopt;
    keys.push_back(key);
    previous = key;
  }
  return keys;
}

// Give the inserted block fresh keys between its neighbours. If floating-point
// precision or tied keys leave no room, re-space the smallest surrounding window of
// siblings, widening it until it fits; keys need not be contiguous, only ordered.
// As insert_block, but leaves `destination` holding the block and every new key, so several
// blocks can be inserted into one sibling list in turn.
std::vector<Card> insert_block_into(
    std::vector<Card>& destination,
    std::size_t at,
    std::vector<Card> block,
    const std::optional<std::string>& parent,
    long long updated_at
) {
  if (block.empty()) return {};
  for (auto& card : block)
    card.parent_card_id = parent;
  destination
      .insert(destination.begin() + static_cast<std::ptrdiff_t>(at), block.begin(), block.end());
  const std::size_t block_end = at + block.size();
  // Re-space destination[low, high), which always contains the block.
  for (std::size_t radius = 0;; radius = radius ? radius * 2 : 1) {
    const std::size_t low = at > radius ? at - radius : 0;
    const std::size_t high = std::min(destination.size(), block_end + radius);
    const auto left = low ? std::optional<double>(destination[low - 1].sort_key) : std::nullopt;
    const auto right = high < destination.size() ? std::optional<double>(destination[high].sort_key)
                                                 : std::nullopt;
    const auto keys = spaced_keys(left, right, high - low);
    if (!keys) {
      if (low == 0 && high == destination.size()) {
        throw std::runtime_error("no sort keys available for card placement");
      }
      continue;
    }
    std::vector<Card> changed;
    for (std::size_t i = low; i < high; ++i) {
      auto& card = destination[i];
      const double key = (*keys)[i - low];
      if ((i >= at && i < block_end) || card.sort_key != key) {
        card.sort_key = key;
        card.updated_at = updated_at;
        changed.push_back(card);
      }
    }
    return changed;
  }
}

std::vector<Card> insert_block(
    std::vector<Card> destination,
    std::size_t at,
    std::vector<Card> block,
    const std::optional<std::string>& parent,
    long long updated_at
) {
  return insert_block_into(destination, at, std::move(block), parent, updated_at);
}
} // namespace

bool card_tree_less(const Card& a, const Card& b) {
  if (a.sort_key < b.sort_key) return true;
  if (a.sort_key > b.sort_key) return false;
  if (a.updated_at != b.updated_at) return a.updated_at > b.updated_at;
  auto lower = [](std::string title) {
    std::transform(title.begin(), title.end(), title.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return title;
  };
  const auto lhs = lower(a.title);
  const auto rhs = lower(b.title);
  return lhs != rhs ? lhs < rhs : a.card_id < b.card_id;
}

std::optional<std::string> reachable_card_parent(CardRepo& repo, const Card& card) {
  std::set<std::string> visited{card.card_id};
  auto next = card.parent_card_id;
  std::optional<std::string> candidate;
  while (next) {
    if (!visited.insert(*next).second) throw std::runtime_error("move_would_create_cycle");
    const auto ancestor = repo.get(*next);
    if (!ancestor || ancestor->project_id != card.project_id) return std::nullopt;
    if (ancestor->deleted_at) {
      candidate.reset();
    } else if (!candidate) {
      candidate = ancestor->card_id;
    }
    next = ancestor->parent_card_id;
  }
  return candidate;
}

void validate_card_parent(CardRepo& repo, const Card& card) {
  if (reachable_card_parent(repo, card) != card.parent_card_id) {
    throw std::runtime_error("target_not_found");
  }
}

std::vector<Card> promoted_card_children(CardRepo& repo, const Card& parent, long long updated_at) {
  auto destination_parent = reachable_card_parent(repo, parent);
  auto source = parent;
  source.parent_card_id = parent.card_id;
  auto children = siblings(repo, source);
  if (children.empty()) return {};
  auto anchor = parent;
  anchor.parent_card_id = destination_parent;
  auto destination = siblings(repo, anchor);
  destination.erase(
      std::remove_if(
          destination.begin(),
          destination.end(),
          [&](const Card& c) {
            return c.card_id == parent.card_id;
          }
      ),
      destination.end()
  );
  const auto pos = std::lower_bound(destination.begin(), destination.end(), anchor, card_tree_less);
  const auto at = static_cast<std::size_t>(pos - destination.begin());
  return insert_block(
      std::move(destination),
      at,
      std::move(children),
      destination_parent,
      updated_at
  );
}

std::vector<Card> placed_card_changes(
    std::vector<Card> destination,
    std::size_t at,
    Card card,
    const std::optional<std::string>& parent,
    long long updated_at
) {
  return insert_block(std::move(destination), at, {std::move(card)}, parent, updated_at);
}

HierarchyRepairPlan plan_hierarchy_repair(
    CardRepo& repo,
    const std::string& project_id,
    long long updated_at
) {
  std::map<std::string, Card> by_id;
  for (auto& card : repo.list_all(project_id))
    by_id.emplace(card.card_id, std::move(card));
  const auto find = [&](const std::optional<std::string>& id) -> const Card* {
    if (!id) return nullptr;
    const auto it = by_id.find(*id);
    return it == by_id.end() ? nullptr : &it->second;
  };

  HierarchyRepairPlan plan;
  struct Head {
    Card card;
    std::optional<std::string> destination;
    std::optional<Card> anchor;
  };
  std::vector<Head> heads;
  for (const auto& [id, card] : by_id) {
    if (card.deleted_at || !card.parent_card_id) continue;
    // Walk the ancestors as reachable_card_parent does, without a query per step.
    std::set<std::string> visited{id};
    std::optional<std::string> destination;
    bool cycle = false;
    for (auto next = card.parent_card_id; next;) {
      if (!visited.insert(*next).second) {
        cycle = true;
        break;
      }
      const auto* ancestor = find(next);
      if (!ancestor) {
        destination.reset();
        break;
      }
      if (ancestor->deleted_at) {
        destination.reset();
      } else if (!destination) {
        destination = ancestor->card_id;
      }
      next = ancestor->parent_card_id;
    }
    if (cycle) {
      plan.cycle_card_ids.push_back(id);
      continue;
    }
    const auto* parent = find(card.parent_card_id);
    // A card under a live parent moves, if at all, with that parent's branch.
    if (parent && !parent->deleted_at) continue;
    // Anchor on the hidden ancestor that sits among the destination's children, so the branch
    // returns to the place it held there.
    std::optional<Card> anchor;
    for (const auto* a = parent; a; a = find(a->parent_card_id)) {
      if (a->parent_card_id == destination) {
        anchor = *a;
        break;
      }
    }
    heads.push_back({card, destination, anchor});
  }

  // Group heads by destination, then by anchor; insert anchored groups before unanchored ones.
  std::map<std::optional<std::string>, std::vector<Head>> by_destination;
  for (auto& head : heads)
    by_destination[head.destination].push_back(std::move(head));
  std::map<std::string, Card> changed;
  for (auto& [destination, group] : by_destination) {
    std::vector<Card> siblings;
    for (const auto& [id, card] : by_id)
      if (!card.deleted_at && card.parent_card_id == destination) siblings.push_back(card);
    std::sort(siblings.begin(), siblings.end(), card_tree_less);
    std::map<std::string, std::vector<Card>> anchored;
    std::vector<Card> unanchored;
    std::map<std::string, Card> anchors;
    for (auto& head : group) {
      plan.moved_card_ids.push_back(head.card.card_id);
      if (head.anchor) {
        anchors.emplace(head.anchor->card_id, *head.anchor);
        anchored[head.anchor->card_id].push_back(std::move(head.card));
      } else {
        unanchored.push_back(std::move(head.card));
      }
    }
    const auto place = [&](std::vector<Card> block, std::size_t at) {
      std::sort(block.begin(), block.end(), card_tree_less);
      for (auto& card : insert_block_into(siblings, at, std::move(block), destination, updated_at))
        changed[card.card_id] = std::move(card);
    };
    for (auto& [anchor_id, block] : anchored) {
      const auto pos =
          std::lower_bound(siblings.begin(), siblings.end(), anchors.at(anchor_id), card_tree_less);
      place(std::move(block), static_cast<std::size_t>(pos - siblings.begin()));
    }
    if (!unanchored.empty()) place(std::move(unanchored), siblings.size());
  }
  for (auto& [id, card] : changed)
    plan.placements.push_back(std::move(card));
  std::sort(plan.moved_card_ids.begin(), plan.moved_card_ids.end());
  return plan;
}

std::vector<Card> restored_card_placement(CardRepo& repo, Card& restored, long long updated_at) {
  restored.parent_card_id = reachable_card_parent(repo, restored);
  auto destination = siblings(repo, restored);
  destination.erase(
      std::remove_if(
          destination.begin(),
          destination.end(),
          [&](const Card& c) {
            return c.card_id == restored.card_id;
          }
      ),
      destination.end()
  );
  const bool collision = !std::isfinite(restored.sort_key) ||
                         std::any_of(destination.begin(), destination.end(), [&](const Card& c) {
                           return c.sort_key == restored.sort_key;
                         });
  if (!collision) return {};
  const auto pos =
      std::lower_bound(destination.begin(), destination.end(), restored, card_tree_less);
  const auto at = static_cast<std::size_t>(pos - destination.begin());
  auto changes =
      insert_block(std::move(destination), at, {restored}, restored.parent_card_id, updated_at);
  changes.erase(
      std::remove_if(
          changes.begin(),
          changes.end(),
          [&](const Card& c) {
            if (c.card_id != restored.card_id) return false;
            restored.sort_key = c.sort_key;
            return true;
          }
      ),
      changes.end()
  );
  return changes;
}
} // namespace holder::card
