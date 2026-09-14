#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace holder::git {

class GitRepo;

enum class RevisionReferenceStatus {
  Resolved,
  Ambiguous,
  NotFound,
};

// Result invariants:
// - Resolved: oid contains the canonical full lowercase commit OID.
// - Ambiguous or NotFound: oid is empty.
struct RevisionReferenceResult {
  RevisionReferenceStatus status = RevisionReferenceStatus::NotFound;
  std::optional<std::string> oid;
};

class RevisionReferenceResolver {
 public:
  // Deliberately independent of the card UUID prefix policy. Shorter Git
  // prefixes are not eligible even when libgit2 could resolve them uniquely.
  static constexpr std::size_t kMinimumPrefixLength = 8;

  explicit RevisionReferenceResolver(GitRepo& repo)
      : repo_(repo) {}

  RevisionReferenceResult resolve(std::string_view reference) const;

 private:
  GitRepo& repo_;
};

} // namespace holder::git
