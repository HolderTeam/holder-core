#include "project/Rebuilder.h"

#include "ai/AiMessageFrontMatter.h"
#include "ai/AiMessagePaths.h"
#include "ai/AiThreadRepo.h"
#include "card/CardFrontMatter.h"
#include "card/CardPaths.h"
#include "card/CardRepo.h"
#include "card/LinkRepo.h"
#include "platform/Fs.h"
#include "platform/Tx.h"
#include "privacy/ProjectPrivacy.h"

#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace holder::store {
namespace {

holder::core::Fs& resolve_fs(holder::core::Fs* fs) {
  static holder::core::RealFs real_fs;
  return fs ? *fs : real_fs;
}

long long file_mtime_seconds(holder::core::Fs& fs, const std::filesystem::path& path) {
  return fs.last_write_time_seconds(path);
}

std::string relative_path_string(
    const std::filesystem::path& root,
    const std::filesystem::path& path
) {
  std::error_code ec;
  const auto rel = std::filesystem::relative(path, root, ec);
  if (ec) {
    throw std::runtime_error("failed to compute relative path"); // LCOV_EXCL_LINE
  }
  return rel.generic_string();
}

void bind_text(sqlite3_stmt* stmt, int idx, const std::string& value) {
  if (sqlite3_bind_text(stmt, idx, value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
    throw std::runtime_error("sqlite bind_text failed"); // LCOV_EXCL_LINE
  }
}

void bind_text_optional(sqlite3_stmt* stmt, int idx, const std::optional<std::string>& value) {
  if (value.has_value()) {
    bind_text(stmt, idx, value.value()); // LCOV_EXCL_LINE
  } else if (sqlite3_bind_null(stmt, idx) != SQLITE_OK) {
    throw std::runtime_error("sqlite bind_null failed"); // LCOV_EXCL_LINE
  }
}

void bind_int64(sqlite3_stmt* stmt, int idx, long long value) {
  if (sqlite3_bind_int64(stmt, idx, static_cast<sqlite3_int64>(value)) != SQLITE_OK) {
    throw std::runtime_error("sqlite bind_int64 failed"); // LCOV_EXCL_LINE
  }
}

void exec_delete_project(sqlite3* db, const std::string& sql, const std::string& project_id) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error("prepare delete failed"); // LCOV_EXCL_LINE
  }
  bind_text(stmt, 1, project_id);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw std::runtime_error("delete failed"); // LCOV_EXCL_LINE
  }
}

std::string derive_title(const std::string& body, const std::string& fallback) {
  std::string line;
  for (char ch : body) {
    if (ch == '\n') break;
    line.push_back(ch);
  }
  auto first = line;
  const auto non_space = first.find_first_not_of(" \t\r");
  if (non_space == std::string::npos) {
    return fallback;
  }
  first = first.substr(non_space);
  if (!first.empty() && first[0] == '#') {
    const auto title_start = first.find_first_not_of("# \t");
    if (title_start != std::string::npos) {
      return first.substr(title_start);
    }
  }
  return fallback;
}

std::string decode_blob_for_project(const holder::model::Project& project, const std::string& raw) {
  if (project.privacy_mode != "encrypted_git") {
    return raw;
  }
  if (!project.project_key_id.has_value() || project.project_key_id->empty()) {
    return raw;
  }
  return holder::privacy::decrypt_project_blob(
      project.project_id,
      project.project_key_id.value(),
      raw
  );
}

struct MessageRecord {
  holder::model::AiMessage message;
  std::string project_id;
  std::vector<holder::model::CardLink> links;
};

struct CardRecord {
  holder::model::Card card;
  std::string body;
  std::vector<holder::model::CardLink> links;
};

} // namespace

Rebuilder::Rebuilder(
    holder::platform::Db& db,
    holder::index::FtsIndexer* fts,
    holder::core::Fs* fs,
    bool tolerate_invalid_ai_messages
)
    : db_(db),
      fts_(fts),
      fs_(&resolve_fs(fs)),
      tolerate_invalid_ai_messages_(tolerate_invalid_ai_messages) {}

Rebuilder::RebuildStats Rebuilder::rebuild_project(const holder::model::Project& project) {
  RebuildStats stats;
  auto& fs = *fs_;
  const std::filesystem::path root = project.root_path;
  if (!fs.exists(root)) {
    throw std::runtime_error("project root not found");
  }

  holder::platform::Tx tx(db_);

  exec_delete_project(
      db_.handle(),
      "DELETE FROM card_links WHERE project_id = ?;",
      project.project_id
  );
  exec_delete_project(db_.handle(), "DELETE FROM cards WHERE project_id = ?;", project.project_id);
  exec_delete_project(
      db_.handle(),
      "DELETE FROM ai_messages WHERE thread_id IN "
      "(SELECT thread_id FROM ai_threads WHERE project_id = ?);",
      project.project_id
  );
  exec_delete_project(
      db_.handle(),
      "DELETE FROM ai_threads WHERE project_id = ?;",
      project.project_id
  );
  exec_delete_project(
      db_.handle(),
      "DELETE FROM cards_fts WHERE project_id = ?;",
      project.project_id
  );
  exec_delete_project(db_.handle(), "DELETE FROM ai_fts WHERE project_id = ?;", project.project_id);

  holder::card::CardRepo card_repo(db_);
  holder::card::LinkRepo link_repo(db_);

  auto collect_files = [&](const std::filesystem::path& base) {
    std::vector<std::filesystem::path> out;
    if (!fs.exists(base)) {
      return out;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(base)) {
      if (!entry.is_regular_file()) continue;
      if (entry.path().extension() != ".md") continue;
      out.push_back(entry.path());
    }
    return out;
  }; // LCOV_EXCL_LINE

  const auto card_files = collect_files(root / "cards");
  const auto trash_card_files = collect_files(root / "trash" / "cards");

  std::vector<CardRecord> card_records;
  auto load_card_file = [&](const std::filesystem::path& path, bool is_trash) {
    const std::string raw = fs.read_file(path);
    const auto parsed = holder::core::parse_card_file(decode_blob_for_project(project, raw));
    if (raw.rfind("---\n", 0) == 0 && !parsed.has_front_matter) {
      throw std::runtime_error("invalid card front matter");
    }
    holder::model::Card card = parsed.card;

    if (parsed.has_front_matter && card.card_id.empty()) {
      throw std::runtime_error("card_id missing in front matter");
    }
    if (card.card_id.empty()) {
      card.card_id = path.stem().string();
    }
    if (card.card_id.size() < 4) {
      throw std::runtime_error("invalid card_id in file");
    }

    const std::string expected_rel = is_trash ? holder::core::card_trash_rel_path(card.card_id)
                                              : holder::core::card_rel_path(card.card_id);
    const std::string actual_rel = relative_path_string(root, path);
    if (actual_rel != expected_rel) {
      throw std::runtime_error("card path does not match card_id");
    }

    card.project_id = project.project_id;
    card.rel_path = expected_rel;

    if (card.title.empty()) {
      card.title = derive_title(parsed.body, card.card_id);
    }

    const long long mtime = file_mtime_seconds(fs, path);
    if (card.created_at <= 0) {
      card.created_at = mtime;
    }
    if (card.updated_at <= 0) {
      card.updated_at = card.created_at;
    }

    if (is_trash) {
      if (!card.deleted_at.has_value()) {
        card.deleted_at = mtime;
      }
    } else {
      card.deleted_at.reset();
    }

    CardRecord record;
    record.card = std::move(card);
    record.body = parsed.body;
    record.links = parsed.links;
    for (auto& link : record.links) {
      link.project_id = record.card.project_id;
      link.from_card_id = record.card.card_id;
      if (link.to_type.empty()) link.to_type = "card";
      if (link.kind.empty()) link.kind = "ref";
      if (link.created_at <= 0) link.created_at = record.card.created_at;
    }
    card_records.push_back(std::move(record));
  };

  for (const auto& path : card_files) {
    load_card_file(path, false);
  }
  for (const auto& path : trash_card_files) {
    load_card_file(path, true);
  }

  std::unordered_set<std::string> inserted_cards;
  std::vector<bool> inserted(card_records.size(), false);
  std::size_t remaining_cards = card_records.size();
  while (remaining_cards > 0) {
    bool progress = false;
    for (std::size_t i = 0; i < card_records.size(); ++i) {
      if (inserted[i]) continue;
      const auto& record = card_records[i];
      if (record.card.parent_card_id.has_value() &&
          inserted_cards.find(record.card.parent_card_id.value()) == inserted_cards.end()) {
        continue;
      }

      card_repo.create(record.card);
      if (fts_ && !record.card.deleted_at.has_value()) {
        fts_->upsert_card(
            record.card.card_id,
            record.card.project_id,
            record.card.title,
            record.body
        );
      }
      inserted_cards.insert(record.card.card_id);
      inserted[i] = true;
      --remaining_cards;
      ++stats.cards;
      progress = true;
    }
    if (!progress) {
      throw std::runtime_error("unresolved parent_card_id during rebuild");
    }
  }

  for (const auto& record : card_records) {
    if (!record.links.empty()) {
      link_repo.upsert_links(record.card.project_id, record.card.card_id, record.links);
      stats.links += record.links.size();
    }
  }

  const auto message_files = collect_files(root / "ai_messages");
  const auto trash_message_files = collect_files(root / "trash" / "ai_messages");

  std::unordered_map<std::string, std::pair<long long, long long>> thread_times;
  std::vector<MessageRecord> records;
  auto rebuild_message_file = [&](const std::filesystem::path& path, bool is_trash) {
    const std::string raw = fs.read_file(path);
    const auto parsed = holder::core::parse_ai_message_file(decode_blob_for_project(project, raw));
    if (raw.rfind("---\n", 0) == 0 && !parsed.has_front_matter) {
      throw std::runtime_error("invalid ai message front matter");
    }
    holder::model::AiMessage message = parsed.message;
    message.content = parsed.body;

    if (parsed.has_front_matter && message.message_id.empty()) {
      throw std::runtime_error("message_id missing in front matter");
    }
    if (message.message_id.empty()) {
      message.message_id = path.stem().string();
    }
    if (message.message_id.size() < 4) {
      throw std::runtime_error("invalid message_id in file");
    }
    if (message.thread_id.empty()) {
      message.thread_id = message.message_id;
    }

    const std::string expected_rel =
        is_trash ? holder::core::ai_message_trash_rel_path(message.message_id)
                 : holder::core::ai_message_rel_path(message.message_id);
    const std::string actual_rel = relative_path_string(root, path);
    if (actual_rel != expected_rel) {
      throw std::runtime_error("ai message path does not match message_id");
    }

    if (message.role.empty()) message.role = "assistant";
    if (message.source.empty()) message.source = "manual_paste";

    const long long mtime = file_mtime_seconds(fs, path);
    if (message.created_at <= 0) {
      message.created_at = mtime;
    }
    if (is_trash) {
      if (!message.deleted_at.has_value()) {
        message.deleted_at = mtime;
      }
    } else {
      message.deleted_at.reset();
    }

    const std::string project_id = project.project_id;
    auto& times = thread_times[message.thread_id];
    if (times.first == 0 || message.created_at < times.first) {
      times.first = message.created_at;
    }
    if (message.created_at > times.second) {
      times.second = message.created_at;
    }

    MessageRecord record;
    record.message = std::move(message);
    record.project_id = project_id;
    record.links = parsed.links;
    for (auto& link : record.links) {
      link.project_id = project_id;
      link.from_card_id = record.message.message_id;
      if (link.to_type.empty()) link.to_type = "card";
      if (link.kind.empty()) link.kind = "ref";
      if (link.created_at <= 0) link.created_at = record.message.created_at;
    }
    records.push_back(std::move(record));
  };

  for (const auto& path : message_files) {
    if (!tolerate_invalid_ai_messages_) {
      rebuild_message_file(path, false);
      continue;
    }
    try {
      rebuild_message_file(path, false);
    } catch (const std::exception& ex) {
      spdlog::warn("Skipping ai message during rebuild at {}: {}", path.string(), ex.what());
    }
  }
  for (const auto& path : trash_message_files) {
    if (!tolerate_invalid_ai_messages_) {
      rebuild_message_file(path, true);
      continue;
    }
    try {
      rebuild_message_file(path, true);
    } catch (const std::exception& ex) {
      spdlog::warn("Skipping ai message during rebuild at {}: {}", path.string(), ex.what());
    }
  }

  holder::ai::AiThreadRepo thread_repo(db_);
  for (const auto& entry : thread_times) {
    holder::model::AiThread thread;
    thread.thread_id = entry.first;
    thread.project_id = project.project_id;
    thread.title = "AI Thread " + entry.first.substr(0, 8);
    thread.created_at = entry.second.first;
    thread.updated_at = entry.second.second;
    thread_repo.create(thread);
    stats.ai_threads += 1;
  }

  static constexpr const char* SQL_INSERT_MESSAGE =
      "INSERT INTO ai_messages(message_id, thread_id, role, source, provider, model, content, "
      "created_at, deleted_at, prompt_hash, meta_json) "
      "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL_INSERT_MESSAGE, -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error("prepare insert ai message failed"); // LCOV_EXCL_LINE
  }

  for (const auto& record : records) {
    const auto& message = record.message;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);

    bind_text(stmt, 1, message.message_id);
    bind_text(stmt, 2, message.thread_id);
    bind_text(stmt, 3, message.role);
    bind_text(stmt, 4, message.source);
    bind_text_optional(stmt, 5, message.provider);
    bind_text_optional(stmt, 6, message.model);
    bind_text(stmt, 7, message.content);
    bind_int64(stmt, 8, message.created_at);
    if (message.deleted_at.has_value()) {
      bind_int64(stmt, 9, message.deleted_at.value());
    } else if (sqlite3_bind_null(stmt, 9) != SQLITE_OK) {
      sqlite3_finalize(stmt); // LCOV_EXCL_LINE
      throw std::runtime_error("sqlite bind_null failed"); // LCOV_EXCL_LINE
    }
    bind_text_optional(stmt, 10, message.prompt_hash);
    bind_text_optional(stmt, 11, message.meta_json);

    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
      sqlite3_finalize(stmt); // LCOV_EXCL_LINE
      throw std::runtime_error("insert ai message failed"); // LCOV_EXCL_LINE
    }

    if (!record.links.empty()) {
      link_repo.upsert_links(record.project_id, message.message_id, record.links);
      stats.links += record.links.size();
    }
    if (fts_ && !message.deleted_at.has_value()) {
      fts_->upsert_message(
          message.message_id,
          message.thread_id,
          record.project_id,
          message.content
      );
    }
    stats.ai_messages += 1;
  }

  sqlite3_finalize(stmt);
  tx.commit();
  return stats;
}

} // namespace holder::store
