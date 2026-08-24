#pragma once

#include "model/Location.h"
#include "platform/Db.h"

#include <optional>
#include <string>
#include <vector>

namespace holder::resource {

class LocationRepo {
 public:
  explicit LocationRepo(holder::platform::Db& db);

  void put(const holder::model::Location& location);
  std::optional<holder::model::Location> get(const std::string& location_id) const;
  std::vector<holder::model::Location> list(const std::string& project_id) const;
  bool is_in_use(const std::string& location_id) const;
  void remove(const std::string& location_id);
  void remove_project(const std::string& project_id);

 private:
  holder::platform::Db& db_;
};

} // namespace holder::resource
