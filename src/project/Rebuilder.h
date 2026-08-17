#pragma once

#include "index/FtsIndexer.h"
#include "model/Project.h"
#include "platform/Db.h"
#include "platform/Fs.h"

#include <cstddef>

namespace holder::store {

class Rebuilder {
 public:
  Rebuilder(
      holder::platform::Db& db,
      holder::index::FtsIndexer* fts,
      holder::core::Fs* fs = nullptr,
      bool tolerate_invalid_ai_messages = false
  );

  struct RebuildStats {
    std::size_t cards = 0;
    std::size_t ai_messages = 0;
    std::size_t ai_threads = 0;
    std::size_t links = 0;
  };

  RebuildStats rebuild_project(const holder::model::Project& project);

 private:
  holder::platform::Db& db_;
  holder::index::FtsIndexer* fts_ = nullptr;
  holder::core::Fs* fs_ = nullptr;
  bool tolerate_invalid_ai_messages_ = false;
};

} // namespace holder::store
