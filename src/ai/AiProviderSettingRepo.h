#pragma once

#include "model/AiProviderSetting.h"
#include "platform/Db.h"

#include <optional>
#include <string>
#include <vector>

namespace holder::ai {

class AiProviderSettingRepo {
 public:
  explicit AiProviderSettingRepo(holder::platform::Db& db);

  std::vector<holder::model::AiProviderSetting> list() const;
  std::optional<holder::model::AiProviderSetting> get(const std::string& provider) const;
  void upsert(const std::string& provider, bool enabled, long long updated_at);
  void remove(const std::string& provider);

 private:
  holder::platform::Db& db_;
};

} // namespace holder::ai
