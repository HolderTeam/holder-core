#pragma once

#include "model/Project.h"

#include <optional>
#include <string>
#include <vector>

namespace holder::history {

struct CardHistoryEntry {
  std::string first_oid;
  std::string last_oid;
  std::vector<std::string> parent_oids;
  std::string author_name;
  std::string author_email;
  long long started_at = 0;
  long long ended_at = 0;
  std::string kind;
  std::string summary;
  std::size_t commit_count = 0;
  bool is_merge = false;
};

struct CardHistoryPage {
  std::optional<std::string> head_oid;
  std::vector<CardHistoryEntry> entries;
  std::optional<std::string> next_cursor;
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

class CardHistoryService {
 public:
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
};

} // namespace holder::history
