#pragma once

#include "model/Card.h"
#include "platform/Db.h"

#include <optional>
#include <string>
#include <vector>

namespace holder::card {

class CardRepo {
 public:
  explicit CardRepo(holder::platform::Db& db);

  void create(const holder::model::Card& card);
  std::optional<holder::model::Card> get(const std::string& card_id) const;

  std::vector<holder::model::Card> list_roots(const std::string& project_id) const;
  std::vector<holder::model::Card> list_children(
      const std::string& project_id,
      const std::string& parent_card_id
  ) const;
  std::vector<holder::model::Card> list_all(const std::string& project_id) const;

  // Cursor-paginated, most-recently-updated first (ties broken by card_id descending, for
  // stable pagination across calls) -- distinct from list_all's sort_key-then-updated_at
  // ordering, which is for normal card-listing UI, not this. Pass std::nullopt for both
  // cursor fields to get the first page; for subsequent pages, pass the last row's own
  // updated_at/card_id back in. Never includes a soft-deleted card. See
  // BACKUP_RESTORE_IMPLEMENTATION_PLAN.md step 1 -- the Android backup snapshot's query.
  std::vector<holder::model::Card> list_recent_page(
      const std::string& project_id,
      const std::optional<long long>& before_updated_at,
      const std::optional<std::string>& before_card_id,
      int limit
  ) const;
  int count_all_not_deleted(const std::string& project_id) const;
  int count_roots_not_deleted(const std::string& project_id) const;
  int count_children_not_deleted(const std::string& project_id, const std::string& parent_card_id)
      const;
  double next_sort_key(
      const std::string& project_id,
      const std::optional<std::string>& parent_card_id
  ) const;

  void update_title(const std::string& card_id, const std::string& title, long long updated_at);
  void touch_updated(const std::string& card_id, long long updated_at);
  void soft_delete(const std::string& card_id, long long deleted_at, long long updated_at);
  void restore(const std::string& card_id, long long updated_at);
  void remove(const std::string& card_id);
  void move(
      const std::string& card_id,
      const std::optional<std::string>& parent_card_id,
      double sort_key,
      long long updated_at
  );

 private:
  holder::platform::Db& db_;
};

} // namespace holder::card
