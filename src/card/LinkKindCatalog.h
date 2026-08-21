#pragma once

#include <string>
#include <vector>

namespace holder::core {

// One entry in the built-in vocabulary of card_links.kind values Holder ships
// curated English labels for. forward_label is how the relationship reads
// from the linking (outgoing) card's side, reverse_label from the linked-to
// (backlink) card's side -- e.g. id "depends_on" reads "Depends on" outgoing,
// "Required by" as a backlink. kind on a link row stays plain text, not
// constrained to this list; a kind outside it has no known reverse, so
// callers fall back to a humanized (snake_case -> "Snake case") label shown
// the same both directions.
struct LinkKindInfo {
  std::string id;
  std::string forward_label;
  std::string reverse_label;
};

// The full catalog, in a stable but otherwise unspecified order. Every id is
// unique and every label is non-empty.
const std::vector<LinkKindInfo>& link_kind_catalog();

}  // namespace holder::core
