#pragma once

#include "git/RevisionReferenceResolver.h"
#include "model/Project.h"

#include <optional>
#include <string>
#include <vector>

namespace holder::history {

struct CardHistorySave {
  std::string oid;
  std::vector<std::string> parent_oids;
  long long authored_at = 0;
  long long committed_at = 0;
  std::string message;
};

struct CardHistoryEntry {
  std::string first_oid;
  std::string last_oid;
  std::vector<std::string> parent_oids;
  // Direct parents which are also represented by an entry in this page. This is
  // deliberately page-local so consumers never need to infer a connection
  // through filtered commits or across a pagination boundary.
  std::vector<std::string> visible_parent_oids;
  std::string author_name;
  std::string author_email;
  long long started_at = 0;
  long long ended_at = 0;
  std::string kind;
  std::string summary;
  std::size_t commit_count = 0;
  bool is_merge = false;
  // Ordered from the first save in an editing session to its final save.
  std::vector<CardHistorySave> saves;
};

struct CardHistoryPage {
  std::optional<std::string> head_oid;
  std::vector<CardHistoryEntry> entries;
  std::optional<std::string> next_cursor;
  bool scan_limited = false;
};

struct CardVersion {
  bool exists = false;
  std::string oid;
  std::string title;
  std::string body;
};

struct CardDiffLine {
  char origin = ' ';
  std::string text;
  long long old_line = -1;
  long long new_line = -1;
};

struct CardHistoryComparison {
  CardVersion from;
  CardVersion to;
  std::string summary;
  std::vector<CardDiffLine> lines;
  bool truncated = false;
};

struct CardHistoryChangeResult {
  holder::git::RevisionReferenceResult revision;
  std::optional<CardHistoryComparison> comparison;
};

class CardHistoryService {
 public:
  explicit CardHistoryService(std::size_t max_scanned_commits = 10'000)
      : max_scanned_commits_(max_scanned_commits) {}

  CardHistoryPage list(
      const holder::model::Project& project,
      const std::string& card_id,
      std::size_t limit = 50,
      const std::optional<std::string>& cursor = std::nullopt
  ) const;

  CardHistoryComparison compare(
      const holder::model::Project& project,
      const std::string& card_id,
      const std::optional<std::string>& from_oid,
      const std::optional<std::string>& to_oid = std::nullopt
  ) const;

  // Compare the state introduced by revision_reference against its first
  // parent. Merge commits deliberately use only their first parent; root
  // commits use the same previously-nonexistent state as compare(nullopt, to).
  // The revision result preserves typed not-found/ambiguous resolution and,
  // when resolved, contains the canonical full commit OID.
  CardHistoryChangeResult compare_change(
      const holder::model::Project& project,
      const std::string& card_id,
      const std::string& revision_reference
  ) const;

 private:
  std::size_t max_scanned_commits_;
};

} // namespace holder::history
