#pragma once

#include "card/CardRepo.h"
#include "model/Card.h"

#include <optional>
#include <string>
#include <vector>

namespace holder::card {

enum class CardReferenceStatus {
  Resolved,
  Ambiguous,
  NotFound,
};

enum class CardReferenceMatchKind {
  FullId,
  IdPrefix,
  ExactTitle,
};

// Result invariants:
// - Resolved: match_kind and card are set; candidates is empty.
// - Ambiguous: match_kind is set; card is empty; candidates contains two cards.
// - NotFound: match_kind and card are empty; candidates is empty.
struct CardReferenceResult {
  CardReferenceStatus status = CardReferenceStatus::NotFound;
  std::optional<CardReferenceMatchKind> match_kind;
  std::optional<holder::model::Card> card;
  std::vector<holder::model::Card> candidates;
};

class CardReferenceResolver {
 public:
  explicit CardReferenceResolver(CardRepo& cards)
      : cards_(cards) {}

  CardReferenceResult resolve(
      const std::string& project_id,
      const std::string& reference,
      holder::model::CardScope scope
  ) const;

 private:
  CardRepo& cards_;
};

} // namespace holder::card
