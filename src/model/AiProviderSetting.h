#pragma once

#include <string>

namespace holder::model {

struct AiProviderSetting {
  std::string provider;
  bool enabled = false;
  long long updated_at = 0;
};

} // namespace holder::model
