#pragma once

#include "privacy/SecretStore.h"

#include <map>
#include <optional>
#include <string>

namespace holder::resource {

struct LocationBinding {
  int version = 1;
  std::string provider;
  std::map<std::string, std::string> values;
};

class LocationBindingStore {
 public:
  explicit LocationBindingStore(holder::privacy::SecretStore& secrets);

  void bind(
      const std::string& project_id,
      const std::string& location_id,
      const LocationBinding& binding,
      const std::string& safe_preview,
      long long now
  );
  std::optional<LocationBinding> get(
      const std::string& project_id,
      const std::string& location_id
  ) const;
  std::optional<std::string> preview(
      const std::string& project_id,
      const std::string& location_id
  ) const;
  void unbind(const std::string& project_id, const std::string& location_id);

  void set_preferred(const std::string& project_id, const std::string& location_id, long long now);
  std::optional<std::string> preferred(const std::string& project_id) const;
  void clear_preferred(const std::string& project_id);

 private:
  holder::privacy::SecretStore& secrets_;
};

} // namespace holder::resource
