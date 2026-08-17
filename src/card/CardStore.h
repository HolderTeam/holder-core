#pragma once

#include "card/CardRepo.h"
#include "card/LinkRepo.h"
#include "git/GitOps.h"
#include "index/FtsIndexer.h"
#include "model/Card.h"
#include "platform/Db.h"
#include "platform/Fs.h"
#include "project/ProjectRepo.h"

#include <optional>
#include <string>

namespace holder::card {

class CardStore {
 public:
  CardStore(
      holder::platform::Db& db,
      holder::index::FtsIndexer* fts,
      holder::core::Fs* fs = nullptr,
      holder::git::GitOps* git = nullptr
  );

  void create(
      holder::model::Card card,
      const std::string& content,
      const std::optional<double>& explicit_sort_key = std::nullopt
  );
  void update_content(
      const std::string& card_id,
      const std::string& content,
      const std::optional<std::string>& title,
      long long updated_at
  );
  void move(
      const std::string& card_id,
      bool has_parent_card_id,
      const std::optional<std::string>& parent_card_id,
      const std::optional<double>& sort_key,
      long long updated_at
  );
  void update_links(const std::string& card_id, long long updated_at);
  void trash(const std::string& card_id, long long deleted_at);
  void restore(const std::string& card_id, long long updated_at);
  void hard_delete(const std::string& card_id);
  std::optional<holder::model::Card> get(const std::string& card_id) const;
  std::optional<std::string> get_content(const holder::model::Card& card);

 private:
  holder::model::Project require_project(const std::string& project_id);

  holder::core::Fs* fs_ = nullptr;
  holder::git::GitOps* git_ = nullptr;
  CardRepo card_repo_;
  LinkRepo link_repo_;
  holder::project::ProjectRepo project_repo_;
  holder::index::FtsIndexer* fts_ = nullptr;
};

} // namespace holder::card
