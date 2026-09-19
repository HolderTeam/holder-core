#include "card/CardReferenceResolver.h"

#include "identity/Uuid.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace holder::card {
namespace {

constexpr int kUniquenessLimit = 2;

bool is_hex_digit(char value) {
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
         (value >= 'A' && value <= 'F');
}

char lowercase_hex(char value) {
  if (value >= 'A' && value <= 'F') {
    return static_cast<char>(value - 'A' + 'a');
  }
  return value;
}

bool is_uuid_hyphen_position(std::size_t index) {
  return index == 8 || index == 13 || index == 18 || index == 23;
}

bool has_uuid_version(char value) { return value == '4' || value == '7'; }

bool has_uuid_variant(char value) {
  const char normalized = lowercase_hex(value);
  return normalized == '8' || normalized == '9' || normalized == 'a' || normalized == 'b';
}

std::string lowercase_uuid(std::string_view value) {
  std::string normalized;
  normalized.reserve(value.size());
  for (const char character : value) {
    normalized.push_back(lowercase_hex(character));
  }
  return normalized;
} // LCOV_EXCL_LINE - gcov artefact: function exit is executed but never counted.

std::optional<std::string> normalize_uuid_prefix(std::string_view reference) {
  constexpr std::size_t kUuidLength = 36;
  constexpr std::size_t kMinimumHexDigits = 8;

  if (reference.size() >= kUuidLength) {
    return std::nullopt; // LCOV_EXCL_LINE - full UUIDs use the exact-reference path before this
                         // helper.
  }

  std::string normalized;
  normalized.reserve(reference.size());
  std::size_t hex_digits = 0;

  for (std::size_t index = 0; index < reference.size(); ++index) {
    const char character = reference[index];
    if (is_uuid_hyphen_position(index)) {
      if (character != '-') {
        return std::nullopt;
      }
      normalized.push_back(character);
      continue;
    }

    if (!is_hex_digit(character)) {
      return std::nullopt;
    }
    normalized.push_back(lowercase_hex(character));
    ++hex_digits;
  }

  if (hex_digits < kMinimumHexDigits) {
    return std::nullopt;
  }
  if (normalized.size() > 14 && !has_uuid_version(normalized[14])) {
    return std::nullopt;
  }
  if (normalized.size() > 19 && !has_uuid_variant(normalized[19])) {
    return std::nullopt;
  }
  return normalized;
}

CardReferenceResult resolved(holder::model::Card card, CardReferenceMatchKind match_kind) {
  CardReferenceResult result;
  result.status = CardReferenceStatus::Resolved;
  result.match_kind = match_kind;
  result.card = std::move(card);
  return result;
}

CardReferenceResult ambiguous(
    std::vector<holder::model::Card> candidates,
    CardReferenceMatchKind match_kind
) {
  CardReferenceResult result;
  result.status = CardReferenceStatus::Ambiguous;
  result.match_kind = match_kind;
  result.candidates = std::move(candidates);
  return result;
}

CardReferenceResult from_candidates(
    std::vector<holder::model::Card> candidates,
    CardReferenceMatchKind match_kind
) {
  if (candidates.size() == 1) {
    return resolved(std::move(candidates.front()), match_kind);
  }
  if (candidates.size() >= 2) {
    return ambiguous(std::move(candidates), match_kind);
  }
  return {};
}

} // namespace

CardReferenceResult CardReferenceResolver::resolve(
    const std::string& project_id,
    const std::string& reference,
    holder::model::CardScope scope
) const {
  if (holder::identity::is_valid_uuid(reference)) {
    const auto card = cards_.find_by_id(project_id, lowercase_uuid(reference), scope);
    if (card.has_value()) {
      return resolved(*card, CardReferenceMatchKind::FullId);
    }
    return {};
  }

  const auto prefix = normalize_uuid_prefix(reference);
  if (prefix.has_value()) {
    auto prefix_result = from_candidates(
        cards_.find_by_id_prefix(project_id, *prefix, scope, kUniquenessLimit),
        CardReferenceMatchKind::IdPrefix
    );
    if (prefix_result.status != CardReferenceStatus::NotFound) {
      return prefix_result;
    }
  }

  return from_candidates(
      cards_.find_by_exact_title(project_id, reference, scope, kUniquenessLimit),
      CardReferenceMatchKind::ExactTitle
  );
}

} // namespace holder::card
