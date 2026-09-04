#pragma once

#include "platform/Db.h"

#include <optional>
#include <string>
#include <vector>

namespace holder::index {

class FtsIndexer {
 public:
  explicit FtsIndexer(holder::platform::Db& db);

  void upsert_card(
      const std::string& card_id,
      const std::string& project_id,
      const std::string& title,
      const std::string& body
  );
  void delete_card(const std::string& card_id);

  // Reads a single card's indexed body text back out, without going through search -- e.g.
  // for the Android backup snapshot (BACKUP_RESTORE_IMPLEMENTATION_PLAN.md step 1), which
  // needs plain card text. Returns nullopt if the card has no cards_fts row (e.g. mid-rebuild).
  std::optional<std::string> get_body(const std::string& card_id) const;

  void upsert_message(
      const std::string& message_id,
      const std::string& thread_id,
      const std::string& project_id,
      const std::string& content
  );
  void delete_message(const std::string& message_id);

  struct SearchRow {
    std::string id;
    std::string title;
    long long updated_at = 0;
    long long created_at = 0;
    std::string snippet;
    double rank = 0.0;
  };

  std::vector<SearchRow> search_cards(
      const std::string& project_id,
      const std::string& query,
      int limit,
      int offset
  );
  std::vector<SearchRow> search_messages(
      const std::string& project_id,
      const std::string& query,
      int limit,
      int offset
  );

 private:
  holder::platform::Db& db_;
};

} // namespace holder::index
