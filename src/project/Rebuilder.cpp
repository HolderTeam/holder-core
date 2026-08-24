#include "project/Rebuilder.h"

#include "ai/AiMessageFrontMatter.h"
#include "ai/AiMessagePaths.h"
#include "ai/AiThreadRepo.h"
#include "ai/AiThreadManifest.h"
#include "card/CardFrontMatter.h"
#include "card/CardPaths.h"
#include "card/CardRepo.h"
#include "card/LinkRepo.h"
#include "card/MilestoneRepo.h"
#include "card/TagExtractor.h"
#include "card/TagRepo.h"
#include "platform/Fs.h"
#include "platform/Tx.h"
#include "privacy/ProjectPrivacy.h"
#include "resource/LocationRepo.h"
#include "resource/ResourceManifest.h"
#include "resource/ResourcePaths.h"
#include "resource/ResourceRepo.h"

#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <algorithm>
#include <cctype>
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
  std::vector<holder::model::Milestone> milestones;
};

bool is_sha256(const std::string& value) {
  return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return std::isxdigit(ch) != 0;
  });
}

} // namespace

Rebuilder::Rebuilder(
    holder::platform::Db& db,
    holder::index::FtsIndexer* fts,
    holder::core::Fs* fs,
    bool tolerate_invalid_ai_messages,
    bool require_ai_thread_manifests
)
    : db_(db),
      fts_(fts),
      fs_(&resolve_fs(fs)),
      tolerate_invalid_ai_messages_(tolerate_invalid_ai_messages),
      require_ai_thread_manifests_(require_ai_thread_manifests) {}

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
  exec_delete_project(db_.handle(), "DELETE FROM resources WHERE project_id = ?;", project.project_id);
  exec_delete_project(
      db_.handle(), "DELETE FROM storage_locations WHERE project_id = ?;", project.project_id
  );

  holder::card::CardRepo card_repo(db_);
  holder::card::LinkRepo link_repo(db_);
  holder::card::MilestoneRepo milestone_repo(db_);
  holder::card::TagRepo tag_repo(db_);
  holder::resource::ResourceRepo resource_repo(db_);
  holder::resource::LocationRepo location_repo(db_);

  auto collect_files = [&](const std::filesystem::path& base, const std::string& extension) {
    std::vector<std::filesystem::path> out;
    if (!fs.exists(base)) {
      return out;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(base)) {
      if (!entry.is_regular_file()) continue;
      if (entry.path().extension() != extension) continue;
      out.push_back(entry.path());
    }
    return out;
  }; // LCOV_EXCL_LINE

  const auto location_files = collect_files(root / "locations", ".json");
  const auto resource_files = collect_files(root / "resources", ".json");

  std::unordered_set<std::string> location_ids;
  std::vector<holder::model::Location> locations;
  for (const auto& path : location_files) {
    holder::model::Location location;
    try {
      location = holder::resource::parse_location_manifest(
          decode_blob_for_project(project, fs.read_file(path))
      );
    } catch (const std::exception& ex) {
      throw std::runtime_error(relative_path_string(root, path) + ": " + ex.what());
    }
    const auto actual = relative_path_string(root, path);
    const auto expected = holder::resource::location_rel_path(location.location_id);
    if (actual != expected) throw std::runtime_error(actual + ": location path does not match id");
    if (location.project_id != project.project_id) {
      throw std::runtime_error(actual + ": location belongs to another project");
    }
    if (!location_ids.insert(location.location_id).second) {
      throw std::runtime_error(actual + ": duplicate location_id");
    }
    locations.push_back(std::move(location));
  }

  std::unordered_set<std::string> resource_ids;
  std::unordered_set<std::string> asset_ids;
  std::unordered_set<std::string> placement_ids;
  std::vector<holder::model::ResourceBundle> resource_bundles;
  for (const auto& path : resource_files) {
    holder::model::ResourceBundle bundle;
    try {
      bundle = holder::resource::parse_resource_manifest(
          decode_blob_for_project(project, fs.read_file(path))
      );
    } catch (const std::exception& ex) {
      throw std::runtime_error(relative_path_string(root, path) + ": " + ex.what());
    }
    const auto actual = relative_path_string(root, path);
    const auto expected = holder::resource::resource_rel_path(bundle.resource.resource_id);
    if (actual != expected) throw std::runtime_error(actual + ": resource path does not match id");
    if (bundle.resource.project_id != project.project_id) {
      throw std::runtime_error(actual + ": resource belongs to another project");
    }
    if (!resource_ids.insert(bundle.resource.resource_id).second) {
      throw std::runtime_error(actual + ": duplicate resource_id");
    }
    for (const auto& asset : bundle.assets) {
      if (!asset_ids.insert(asset.asset_id).second) {
        throw std::runtime_error(actual + ": duplicate asset_id");
      }
      if (!is_sha256(asset.plaintext_sha256)) {
        throw std::runtime_error(actual + ": invalid plaintext_sha256");
      }
      for (const auto& placement : asset.placements) {
        if (!placement_ids.insert(placement.placement_id).second) {
          throw std::runtime_error(actual + ": duplicate placement_id");
        }
        if (!is_sha256(placement.stored_sha256)) {
          throw std::runtime_error(actual + ": invalid stored_sha256");
        }
      }
    }
    resource_bundles.push_back(std::move(bundle));
  }

  for (const auto& bundle : resource_bundles) {
    for (const auto& asset : bundle.assets) {
      for (const auto& placement : asset.placements) {
        if (location_ids.find(placement.location_id) == location_ids.end()) {
          throw std::runtime_error(
              holder::resource::resource_rel_path(bundle.resource.resource_id) +
              ": placement refers to unknown location " + placement.location_id
          );
        }
      }
    }
  }

  for (const auto& location : locations) {
    location_repo.put(location);
    ++stats.locations;
  }
  for (const auto& bundle : resource_bundles) {
    resource_repo.put_bundle(bundle);
    ++stats.resources;
    stats.assets += bundle.assets.size();
    for (const auto& asset : bundle.assets) stats.placements += asset.placements.size();
  }

  const auto card_files = collect_files(root / "cards", ".md");
  const auto trash_card_files = collect_files(root / "trash" / "cards", ".md");

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
    record.milestones = parsed.milestones;
    for (auto& milestone : record.milestones) {
      milestone.project_id = record.card.project_id;
      milestone.card_id = record.card.card_id;
      if (milestone.created_at <= 0) milestone.created_at = record.card.created_at;
      if (milestone.updated_at <= 0) milestone.updated_at = milestone.created_at;
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
      if (!record.card.deleted_at.has_value()) {
        if (fts_) {
          fts_->upsert_card(
              record.card.card_id,
              record.card.project_id,
              record.card.title,
              record.body
          );
        }
        const auto extracted_tags = holder::core::extract_tags(record.body);
        tag_repo.set_tags_for_card(
            record.card.project_id, record.card.card_id, extracted_tags, record.card.created_at
        );
        stats.tags += extracted_tags.size();

        if (!record.milestones.empty()) {
          milestone_repo.replace_for_card(
              record.card.project_id, record.card.card_id, record.milestones
          );
          stats.milestones += record.milestones.size();
        }
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

  const auto message_files = collect_files(root / "ai_messages", ".md");
  const auto trash_message_files = collect_files(root / "trash" / "ai_messages", ".md");
  const auto thread_files = collect_files(root / "ai_threads", ".json");

  std::unordered_map<std::string, holder::model::AiThread> durable_threads;
  for (const auto& path : thread_files) {
    holder::model::AiThread thread;
    try {
      thread = holder::ai::read_ai_thread_manifest(project, path);
    } catch (const std::exception& ex) {
      throw std::runtime_error(relative_path_string(root, path) + ": " + ex.what());
    }
    const auto actual = relative_path_string(root, path);
    const auto expected = holder::ai::ai_thread_manifest_rel_path(thread.thread_id);
    if (actual != expected) throw std::runtime_error(actual + ": AI thread path does not match id");
    if (!durable_threads.emplace(thread.thread_id, thread).second) {
      throw std::runtime_error(actual + ": duplicate thread_id");
    }
  }

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
  if (!durable_threads.empty()) {
    for (const auto& [thread_id, times] : thread_times) {
      (void)times;
      if (durable_threads.find(thread_id) == durable_threads.end()) {
        throw std::runtime_error("AI message refers to thread without durable manifest: " + thread_id);
      }
    }
    std::vector<std::string> ids;
    ids.reserve(durable_threads.size());
    for (const auto& [thread_id, thread] : durable_threads) {
      (void)thread;
      ids.push_back(thread_id);
    }
    std::sort(ids.begin(), ids.end());
    for (const auto& thread_id : ids) {
      thread_repo.create(durable_threads.at(thread_id));
      ++stats.ai_threads;
    }
  } else {
    if (require_ai_thread_manifests_ && !thread_times.empty()) {
      throw std::runtime_error("AI messages exist without durable thread manifests");
    }
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
