#include "card/CardHierarchy.h"

#include <algorithm>
#include <cctype>
#include <cmath>
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

// Prefer fresh keys only for the inserted block. If floating-point precision or
// tied keys make that impossible, renumber the destination in its captured order.
std::vector<Card> insert_block(
    std::vector<Card> destination,
    std::size_t at,
    std::vector<Card> block,
    const std::optional<std::string>& parent,
    long long updated_at
) {
  if (block.empty()) return {};
  const double left = at ? destination[at - 1].sort_key
                         : (at < destination.size()
                                ? destination[at].sort_key - static_cast<double>(block.size()) - 1.0
                                : 0.0);
  const double right = at < destination.size() ? destination[at].sort_key
                                               : left + static_cast<double>(block.size()) + 1.0;
  double previous = left;
  bool fits = std::isfinite(left) && std::isfinite(right) && left < right;
  for (std::size_t i = 0; i < block.size(); ++i) {
    const double fraction = static_cast<double>(i + 1) / static_cast<double>(block.size() + 1);
    const double key = left * (1.0 - fraction) + right * fraction;
    fits = fits && std::isfinite(key) && key > previous && key < right;
    block[i].parent_card_id = parent;
    block[i].sort_key = key;
    block[i].updated_at = updated_at;
    previous = key;
  }
  if (fits) return block;

  destination
      .insert(destination.begin() + static_cast<std::ptrdiff_t>(at), block.begin(), block.end());
  std::vector<Card> changed;
  for (std::size_t i = 0; i < destination.size(); ++i) {
    auto& card = destination[i];
    const double key = static_cast<double>(i + 1);
    if ((i >= at && i < at + block.size()) || card.sort_key != key) {
      card.sort_key = key;
      card.updated_at = updated_at;
      changed.push_back(card);
    }
  }
  return changed;
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
