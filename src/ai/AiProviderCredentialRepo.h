#pragma once

#include "model/AiProviderCredential.h"
#include "platform/Db.h"

#include <optional>
#include <string>
#include <vector>

namespace holder::ai {

class AiProviderCredentialRepo {
 public:
  explicit AiProviderCredentialRepo(holder::platform::Db& db);

  std::vector<holder::model::AiProviderCredential> list() const;
  std::optional<holder::model::AiProviderCredential> get(const std::string& provider) const;
  void upsert(
      const std::string& provider,
      const std::string& api_key_preview,
      long long created_at,
      long long updated_at
  );
  void remove(const std::string& provider);

 private:
  holder::platform::Db& db_;
};

} // namespace holder::ai
