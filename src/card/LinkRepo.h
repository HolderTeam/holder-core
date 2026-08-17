#pragma once

#include "model/CardLink.h"
#include "platform/Db.h"

#include <optional>
#include <string>
#include <vector>

namespace holder::card {

class LinkRepo {
 public:
  explicit LinkRepo(holder::platform::Db& db);

  void upsert_links(
      const std::string& project_id,
      const std::string& from_card_id,
      const std::vector<holder::model::CardLink>& links
  );

  std::vector<holder::model::CardLink> list_outgoing(
      const std::string& project_id,
      const std::string& from_card_id
  ) const;

  std::vector<holder::model::CardLink> list_backlinks(
      const std::string& project_id,
      const std::string& to_card_id
  ) const;
  std::vector<holder::model::CardLink> list_backlinks_typed(
      const std::string& project_id,
      const std::string& to_card_id,
      const std::string& to_type
  ) const;

  void delete_link(
      const std::string& project_id,
      const std::string& from_card_id,
      const std::string& to_card_id,
      const std::optional<std::string>& to_type,
      const std::optional<std::string>& kind
  );
  void delete_links_to_typed(
      const std::string& project_id,
      const std::string& to_card_id,
      const std::string& to_type
  );

  void delete_links_from(const std::string& project_id, const std::string& from_card_id);

 private:
  holder::platform::Db& db_;
};

} // namespace holder::card
