#pragma once

#include "card/CardRepo.h"
#include "model/Card.h"

#include <optional>
#include <string>

namespace holder::card {

enum class CardPlacementIntent {
  Into,
  Before,
  After,
  ToStart,
  ToEnd,
  Left,
  Right,
  UpLevel,
};

// intent determines which of target_card_id/parent_card_id apply:
// - Into/Before/After: target_card_id is required; parent_card_id is ignored.
// - ToStart/ToEnd/Left/Right: target_card_id is ignored. parent_card_id is an optional
//   override -- when absent, the card's own current parent is used. Unlike the daemon route
//   this was ported from, there is no way to distinguish "not provided" from "explicitly
//   null" once the request reaches this struct, so both fold to "use the current parent";
//   only a caller-supplied, non-empty parent_card_id actually overrides it.
// - UpLevel: both are ignored.
struct CardPlacementRequest {
  CardPlacementIntent intent;
  std::optional<std::string> target_card_id;
  std::optional<std::string> parent_card_id;
};

struct CardPlacementResult {
  std::optional<std::string> parent_card_id;
  double sort_key = 0.0;
  std::optional<std::string> moved_into_title;
};

// Resolves a card-move placement intent -- the algorithm behind holder-daemon's /move route --
// without applying it: given a live sibling tree, works out the exact {parent, sort_key} a
// card should land at for one of eight relative-placement intents, but leaves the actual
// write (holder::card::CardStore::move) to the caller. Ported verbatim from holder-daemon's
// CardRoutes.cpp so both the daemon route and this library's own C API
// (holder_card_move_json) share one implementation.
class CardPlacementResolver {
 public:
  explicit CardPlacementResolver(CardRepo& cards)
      : cards_(cards) {}

  // Resolves request against project_id's live cards, given card_id is the card being moved.
  // Throws std::runtime_error with one of these exact messages on failure, matching the
  // daemon route's existing error vocabulary:
  //   "card_not_found" -- card_id doesn't exist, or is soft-deleted.
  //   "cross_project_move_forbidden" -- card_id belongs to a different project.
  //   "target_not_found" -- target_card_id (Into/Before/After) or an explicit/inferred
  //     parent_card_id (ToStart/ToEnd/Left/Right/UpLevel's destination) doesn't exist, is
  //     soft-deleted, or isn't in project_id.
  //   "move_would_create_cycle" -- an Into move would make card_id its own ancestor.
  //   "invalid_target" -- target_card_id (Before/After) isn't actually among the target's
  //     siblings once card_id itself is excluded (e.g. target_card_id == card_id).
  //   "invalid_move_intent" -- defensive only; every CardPlacementIntent value is otherwise
  //     handled.
  //   "already_at_project_root" -- UpLevel on a card that has no parent.
  //   "missing_target_card_id" -- Into/Before/After without target_card_id.
  CardPlacementResult resolve(
      const std::string& project_id,
      const std::string& card_id,
      const CardPlacementRequest& request
  ) const;

 private:
  CardRepo& cards_;
};

} // namespace holder::card
