#pragma once

#include "ai/AiThreadRepo.h"
#include "card/LinkRepo.h"
#include "git/GitOps.h"
#include "index/FtsIndexer.h"
#include "model/AiMessage.h"
#include "platform/Db.h"
#include "platform/Fs.h"
#include "project/ProjectRepo.h"

#include <optional>
#include <string>
#include <vector>

namespace holder::ai {

class AiMessageRepo {
 public:
  AiMessageRepo(
      holder::platform::Db& db,
      holder::index::FtsIndexer* fts,
      holder::core::Fs* fs = nullptr,
      holder::git::GitOps* git = nullptr
  );

  void append(const holder::model::AiMessage& message);
  std::optional<holder::model::AiMessage> get(const std::string& message_id) const;
  std::vector<holder::model::AiMessage> list_by_thread(const std::string& thread_id) const;
  void update_links(const std::string& message_id);
  void update(const holder::model::AiMessage& message);
  std::vector<holder::model::AiMessage> list_deleted_by_project(const std::string& project_id
  ) const;
  void trash(const std::string& message_id, long long deleted_at);
  void restore(const std::string& message_id);
  void remove(const std::string& message_id);

 private:
  holder::platform::Db& db_;
  holder::core::Fs* fs_ = nullptr;
  holder::git::GitOps* git_ = nullptr;
  holder::card::LinkRepo link_repo_;
  holder::ai::AiThreadRepo thread_repo_;
  holder::project::ProjectRepo project_repo_;
  holder::index::FtsIndexer* fts_ = nullptr;
};

} // namespace holder::ai
