#pragma once

#include "model/Milestone.h"
#include "platform/Db.h"

#include <string>
#include <vector>

namespace holder::card {

class MilestoneRepo {
 public:
  explicit MilestoneRepo(holder::platform::Db& db);

  // Replaces card_id's whole milestone set with `milestones` (delete-then-insert), matching
  // TagRepo::set_tags_for_card -- milestones are rebuilt from front matter each write, so a
  // milestone removed from the file must disappear from the index too, not just get upserted.
  void replace_for_card(
      const std::string& project_id,
      const std::string& card_id,
      const std::vector<holder::model::Milestone>& milestones
  );

  void delete_for_card(const std::string& project_id, const std::string& card_id);

  std::vector<holder::model::Milestone> list_for_card(
      const std::string& project_id,
      const std::string& card_id
  ) const;

  // Every milestone in the project whose start_at falls within [from, to] (inclusive), ordered
  // by start_at -- the Calendar's primary query. Never includes a trashed card's milestones,
  // same defensive join TagRepo's read paths use.
  std::vector<holder::model::Milestone> list_in_range(
      const std::string& project_id,
      long long from,
      long long to
  ) const;

 private:
  holder::platform::Db& db_;
};

} // namespace holder::card
