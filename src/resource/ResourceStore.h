#pragma once

#include "git/GitOps.h"
#include "model/Resource.h"
#include "platform/Db.h"
#include "platform/Fs.h"
#include "project/ProjectRepo.h"
#include "resource/ResourceRepo.h"

#include <memory>
#include <optional>
#include <string>

namespace holder::resource {

class ResourceStore {
 public:
  ResourceStore(
      holder::platform::Db& db,
      holder::core::Fs* fs = nullptr,
      holder::git::GitOps* git = nullptr
  );

  void put(const holder::model::ResourceBundle& bundle);
  std::optional<holder::model::ResourceBundle> get(const std::string& resource_id) const;
  void remove(const std::string& resource_id);

 private:
  holder::model::Project require_project(const std::string& project_id);

  holder::platform::Db& db_;
  holder::core::Fs* fs_;
  // Owns the Git handle only when the caller injected none; see the constructor.
  std::unique_ptr<holder::git::RealGitOps> owned_git_;
  holder::git::GitOps* git_;
  holder::project::ProjectRepo project_repo_;
  ResourceRepo resource_repo_;
};

} // namespace holder::resource
