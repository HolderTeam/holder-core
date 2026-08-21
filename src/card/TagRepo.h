#pragma once

#include "platform/Db.h"

#include <string>
#include <utility>
#include <vector>

namespace holder::card {

class TagRepo {
 public:
  explicit TagRepo(holder::platform::Db& db);

  // Replaces card_id's whole tag set with `tags` (delete-then-insert) -- tags can be removed by
  // editing them out of the body, not just added, so this isn't an upsert. Not wrapped in its
  // own transaction, matching LinkRepo::upsert_links, so it composes with a caller-level Tx
  // (e.g. Rebuilder's) instead of conflicting with one.
  void set_tags_for_card(
      const std::string& project_id,
      const std::string& card_id,
      const std::vector<std::string>& tags,
      long long now
  );

  void delete_tags_for_card(const std::string& project_id, const std::string& card_id);

  std::vector<std::string> list_tags_for_card(
      const std::string& project_id,
      const std::string& card_id
  ) const;

  // Never includes a trashed card's id, even if card_tags somehow still has a row for one.
  std::vector<std::string> list_card_ids_with_tag(
      const std::string& project_id,
      const std::string& tag
  ) const;

  // Every distinct tag in the project with how many (non-trashed) cards carry it, most-used
  // first.
  std::vector<std::pair<std::string, int>> list_project_tags(const std::string& project_id) const;

 private:
  holder::platform::Db& db_;
};

} // namespace holder::card
