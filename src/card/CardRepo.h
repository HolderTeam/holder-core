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
