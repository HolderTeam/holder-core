#pragma once

#include "platform/Fs.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace holder::card {

// Used under the project operation lock. The journal holds each path's original and
// intended bytes (still encrypted for encrypted cards), the index and HEAD. A caught
// failure rolls the operation back. After an interruption, recovery keeps the
// operation if its commit landed and otherwise undoes its uncommitted file writes;
// it never moves Git refs. It never resets the working tree.
class CardMutation {
 public:
  CardMutation(
      holder::core::Fs& fs,
      const std::filesystem::path& root,
      const std::vector<std::string>& paths,
      // Intended bytes for each changed path; nullopt removes it. Other paths are unchanged.
      const std::map<std::string, std::optional<std::string>>& changes
  );
  ~CardMutation();
  CardMutation(const CardMutation&) = delete;
  CardMutation& operator=(const CardMutation&) = delete;
  void begin();
  void preserve_unrelated_index();
  void rollback();
  void finish();

  static bool pending(const std::filesystem::path& root);
  static void recover_files(holder::core::Fs& fs, const std::filesystem::path& root);
  static void finish_recovery(holder::core::Fs& fs, const std::filesystem::path& root);

 private:
  struct State;
  std::unique_ptr<State> state_;
};
} // namespace holder::card
