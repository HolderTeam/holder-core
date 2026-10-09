#pragma once

#include "platform/Fs.h"

#include <memory>
#include <vector>

namespace holder::card {

// Used under the project operation lock. Journal contains original bytes (still
// encrypted for encrypted cards), index and HEAD, so interrupted writes can be
// rolled back before rebuilding SQLite. It never resets the working tree.
class CardMutation {
 public:
  CardMutation(
      holder::core::Fs& fs,
      const std::filesystem::path& root,
      const std::vector<std::string>& paths
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
