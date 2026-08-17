#pragma once

#include "model/Resource.h"
#include "platform/Db.h"

#include <optional>
#include <string>
#include <vector>

namespace holder::resource {

class ResourceRepo {
 public:
  explicit ResourceRepo(holder::platform::Db& db);

  void add(const holder::model::Resource& resource);
  std::optional<holder::model::Resource> get(const std::string& resource_id) const;
  void update(const holder::model::Resource& resource);
  std::vector<holder::model::Resource> list(const std::string& project_id) const;
  void remove(const std::string& resource_id);

 private:
  holder::platform::Db& db_;
};

} // namespace holder::resource
