#pragma once

#include "model/AiRunner.h"
#include "platform/Db.h"

#include <optional>
#include <string>
#include <vector>

namespace holder::ai {

class AiRunnerRepo {
 public:
  explicit AiRunnerRepo(holder::platform::Db& db);

  std::vector<holder::model::AiRunner> list() const;
  std::optional<holder::model::AiRunner> get(const std::string& runner_id) const;
  void upsert(const holder::model::AiRunner& runner);
  void remove(const std::string& runner_id);

 private:
  holder::platform::Db& db_;
};

} // namespace holder::ai
