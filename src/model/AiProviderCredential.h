#pragma once

#include <string>

namespace holder::model {

struct AiProviderCredential {
  std::string provider;
  std::string api_key_preview;
  long long created_at = 0;
  long long updated_at = 0;
};

} // namespace holder::model
