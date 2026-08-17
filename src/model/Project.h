#pragma once

#include <optional>
#include <string>

namespace holder::model {

struct Project {
  std::string project_id;
  std::string name;
  std::string root_path;
  std::optional<std::string> git_remote_url;
  std::optional<std::string> git_provider;
  std::string privacy_mode = "encrypted_git";
  std::optional<std::string> project_key_id;
  long long created_at = 0;
  long long updated_at = 0;
};

} // namespace holder::model
