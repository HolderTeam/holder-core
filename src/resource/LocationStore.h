#pragma once

#include "git/GitOps.h"
#include "model/Location.h"
#include "platform/Db.h"
#include "platform/Fs.h"
#include "project/ProjectRepo.h"
#include "resource/LocationRepo.h"

#include <optional>
#include <string>

namespace holder::resource {

class LocationStore {
 public:
  LocationStore(
      holder::platform::Db& db,
      holder::core::Fs* fs = nullptr,
      holder::git::GitOps* git = nullptr
  );

  void put(const holder::model::Location& location);
  std::optional<holder::model::Location> get(const std::string& location_id) const;
  void remove(const std::string& location_id);

 private:
  holder::model::Project require_project(const std::string& project_id);

  holder::platform::Db& db_;
  holder::core::Fs* fs_;
  holder::git::GitOps* git_;
  holder::project::ProjectRepo project_repo_;
  LocationRepo location_repo_;
};

} // namespace holder::resource
