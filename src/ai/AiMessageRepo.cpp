#include "ai/AiMessageRepo.h"

#include "ai/AiMessageFrontMatter.h"
#include "ai/AiMessagePaths.h"
#include "git/GitOps.h"
#include "platform/Fs.h"

#include <sqlite3.h>

#include <filesystem>
#include <stdexcept>
#include <utility>

namespace holder::ai {
namespace {

holder::core::Fs& resolve_fs(holder::core::Fs* fs) {
  static holder::core::RealFs real_fs;
  return fs ? *fs : real_fs;
}

holder::git::GitOps& resolve_git(holder::git::GitOps* git) {
  static holder::git::RealGitOps real_git;
  return git ? *git : real_git;
}

void throw_sqlite(sqlite3* db, const std::string& what) {
  const char* msg = db ? sqlite3_errmsg(db) : "unknown sqlite error";
  throw std::runtime_error(what + ": " + msg);
}

void bind_text(sqlite3_stmt* stmt, int idx, const std::string& value) {
  if (sqlite3_bind_text(stmt, idx, value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
    throw std::runtime_error("sqlite bind_text failed"); // LCOV_EXCL_LINE
  }
}

void bind_text_optional(sqlite3_stmt* stmt, int idx, const std::optional<std::string>& value) {
  if (value.has_value()) {
    bind_text(stmt, idx, value.value());
  } else if (sqlite3_bind_null(stmt, idx) != SQLITE_OK) {
    throw std::runtime_error("sqlite bind_null failed"); // LCOV_EXCL_LINE
  }
}

void bind_int64(sqlite3_stmt* stmt, int idx, long long value) {
  if (sqlite3_bind_int64(stmt, idx, static_cast<sqlite3_int64>(value)) != SQLITE_OK) {
    throw std::runtime_error("sqlite bind_int64 failed"); // LCOV_EXCL_LINE
  }
}

holder::model::AiMessage read_message(sqlite3_stmt* stmt) {
  holder::model::AiMessage m;
  m.message_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  m.thread_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  m.role = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
  m.source = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
  if (sqlite3_column_type(stmt, 4) == SQLITE_NULL) {
    m.provider.reset();
  } else {
    m.provider = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
  }
  if (sqlite3_column_type(stmt, 5) == SQLITE_NULL) {
    m.model.reset();
  } else {
    m.model = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
  }
  m.content = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
  m.created_at = sqlite3_column_int64(stmt, 7);
  if (sqlite3_column_type(stmt, 8) == SQLITE_NULL) {
    m.deleted_at.reset();
  } else {
    m.deleted_at = sqlite3_column_int64(stmt, 8);
  }
  if (sqlite3_column_type(stmt, 9) == SQLITE_NULL) {
    m.prompt_hash.reset();
  } else {
    m.prompt_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
  }
  if (sqlite3_column_type(stmt, 10) == SQLITE_NULL) {
    m.meta_json.reset();
  } else {
    m.meta_json = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
  }
  return m;
} // LCOV_EXCL_LINE

} // namespace

AiMessageRepo::AiMessageRepo(
    holder::platform::Db& db,
    holder::index::FtsIndexer* fts,
    holder::core::Fs* fs,
    holder::git::GitOps* git
)
    : db_(db),
      fs_(&resolve_fs(fs)),
      git_(&resolve_git(git)),
      link_repo_(db),
      thread_repo_(db),
      project_repo_(db),
      fts_(fts) {}

void AiMessageRepo::append(const holder::model::AiMessage& message) {
  const auto thread_opt = thread_repo_.get(message.thread_id);
  if (!thread_opt.has_value()) {
    throw std::runtime_error("thread not found for ai message");
  }
  const auto project_opt = project_repo_.get(thread_opt->project_id);
  if (!project_opt.has_value()) {
    throw std::runtime_error("project not found for ai message thread");
  }

  git_->open_or_init(project_opt->root_path);
  if (project_opt->git_remote_url.has_value()) {
    git_->set_remote("origin", project_opt->git_remote_url.value());
  }

  const std::string rel_path = holder::core::ai_message_rel_path(message.message_id);
  const auto full_path = git_->repo_dir() / rel_path;
  if (fs_->exists(full_path)) {
    throw std::runtime_error("conflict: ai message file already exists");
  }

  const auto front_matter =
      holder::core::render_ai_message_front_matter(message, project_opt->project_id, {});
  git_->write_file(rel_path, front_matter + message.content);

  static constexpr const char* SQL =
      "INSERT INTO ai_messages(message_id, thread_id, role, source, provider, model, content, "
      "created_at, deleted_at, prompt_hash, meta_json) "
      "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare insert ai message failed");
  }

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
    throw std::runtime_error("sqlite bind_null failed"); // LCOV_EXCL_LINE
  }
  bind_text_optional(stmt, 10, message.prompt_hash);
  bind_text_optional(stmt, 11, message.meta_json);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    fs_->remove(full_path);
    throw_sqlite(db_.handle(), "insert ai message failed");
  }

  if (fts_) {
    fts_->upsert_message(
        message.message_id,
        message.thread_id,
        project_opt->project_id,
        message.content
    );
  }

  git_->stage_path(rel_path);
  git_->commit("Add ai message " + message.message_id);
}

std::vector<holder::model::AiMessage> AiMessageRepo::list_by_thread(const std::string& thread_id
) const {
  static constexpr const char* SQL =
      "SELECT message_id, thread_id, role, source, provider, model, content, created_at, deleted_at, prompt_hash, meta_json "
      "FROM ai_messages WHERE thread_id = ? ORDER BY created_at ASC;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare list ai messages failed");
  }

  bind_text(stmt, 1, thread_id);

  std::vector<holder::model::AiMessage> out;
  while (true) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      out.push_back(read_message(stmt));
      continue;
    }
    if (rc == SQLITE_DONE) break;
    sqlite3_finalize(stmt);
    throw_sqlite(db_.handle(), "list ai messages failed");
  }

  sqlite3_finalize(stmt);
  return out;
} // LCOV_EXCL_LINE

void AiMessageRepo::update(const holder::model::AiMessage& message) {
  static constexpr const char* SQL =
      "UPDATE ai_messages SET role = ?, source = ?, provider = ?, model = ?, content = ?, "
      "created_at = ?, prompt_hash = ?, meta_json = ? WHERE message_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare update ai message failed");
  }

  bind_text(stmt, 1, message.role);
  bind_text(stmt, 2, message.source);
  bind_text_optional(stmt, 3, message.provider);
  bind_text_optional(stmt, 4, message.model);
  bind_text(stmt, 5, message.content);
  bind_int64(stmt, 6, message.created_at);
  bind_text_optional(stmt, 7, message.prompt_hash);
  bind_text_optional(stmt, 8, message.meta_json);
  bind_text(stmt, 9, message.message_id);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "update ai message failed");
  }

  const auto thread_opt = thread_repo_.get(message.thread_id);
  if (!thread_opt.has_value()) {
    throw std::runtime_error("thread not found for ai message");
  }
  const auto project_opt = project_repo_.get(thread_opt->project_id);
  if (!project_opt.has_value()) {
    throw std::runtime_error("project not found for ai message thread");
  }

  git_->open_or_init(project_opt->root_path);
  if (project_opt->git_remote_url.has_value()) {
    git_->set_remote("origin", project_opt->git_remote_url.value());
  }

  const std::string rel_path = holder::core::ai_message_rel_path(message.message_id);
  const auto links = link_repo_.list_outgoing(project_opt->project_id, message.message_id);
  const auto front_matter =
      holder::core::render_ai_message_front_matter(message, project_opt->project_id, links);
  git_->write_file(rel_path, front_matter + message.content);
  git_->stage_path(rel_path);
  git_->commit("Update ai message " + message.message_id);
}

std::vector<holder::model::AiMessage> AiMessageRepo::list_deleted_by_project(
    const std::string& project_id
) const {
  static constexpr const char* SQL =
      "SELECT m.message_id, m.thread_id, m.role, m.source, m.provider, m.model, "
      "m.content, m.created_at, m.deleted_at, m.prompt_hash, m.meta_json "
      "FROM ai_messages m JOIN ai_threads t ON m.thread_id = t.thread_id "
      "WHERE t.project_id = ? AND m.deleted_at IS NOT NULL "
      "ORDER BY m.deleted_at DESC;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare list deleted ai messages failed");
  }

  bind_text(stmt, 1, project_id);

  std::vector<holder::model::AiMessage> out;
  while (true) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      out.push_back(read_message(stmt));
      continue;
    }
    if (rc == SQLITE_DONE) break;
    sqlite3_finalize(stmt);
    throw_sqlite(db_.handle(), "list deleted ai messages failed");
  }

  sqlite3_finalize(stmt);
  return out;
} // LCOV_EXCL_LINE

void AiMessageRepo::trash(const std::string& message_id, long long deleted_at) {
  const auto msg_opt = get(message_id);
  if (!msg_opt.has_value()) {
    throw std::runtime_error("ai message not found: " + message_id);
  }
  if (msg_opt->deleted_at.has_value()) {
    throw std::runtime_error("ai message already deleted");
  }

  const auto thread_opt = thread_repo_.get(msg_opt->thread_id);
  if (!thread_opt.has_value()) {
    throw std::runtime_error("thread not found for ai message");
  }
  const auto project_opt = project_repo_.get(thread_opt->project_id);
  if (!project_opt.has_value()) {
    throw std::runtime_error("project not found for ai message thread");
  }

  git_->open_or_init(project_opt->root_path);
  if (project_opt->git_remote_url.has_value()) {
    git_->set_remote("origin", project_opt->git_remote_url.value());
  }

  const std::string rel_path = holder::core::ai_message_rel_path(message_id);
  const std::string trash_rel = holder::core::ai_message_trash_rel_path(message_id);
  const auto src_path = git_->repo_dir() / rel_path;
  const auto dst_path = git_->repo_dir() / trash_rel;
  if (!fs_->exists(src_path)) {
    throw std::runtime_error("ai message content missing");
  }
  fs_->create_directories(dst_path.parent_path());
  fs_->rename(src_path, dst_path);

  static constexpr const char* SQL = "UPDATE ai_messages SET deleted_at = ? WHERE message_id = ?;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare trash ai message failed");
  }
  bind_int64(stmt, 1, deleted_at);
  bind_text(stmt, 2, message_id);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "trash ai message failed");
  }

  if (fts_) {
    fts_->delete_message(message_id);
  }

  git_->remove_path(rel_path);
  git_->stage_path(trash_rel);
  git_->commit("Delete ai message " + message_id);
}

void AiMessageRepo::restore(const std::string& message_id) {
  const auto msg_opt = get(message_id);
  if (!msg_opt.has_value()) {
    throw std::runtime_error("ai message not found: " + message_id);
  }
  if (!msg_opt->deleted_at.has_value()) {
    throw std::runtime_error("ai message is not deleted");
  }

  const auto thread_opt = thread_repo_.get(msg_opt->thread_id);
  if (!thread_opt.has_value()) {
    throw std::runtime_error("thread not found for ai message");
  }
  const auto project_opt = project_repo_.get(thread_opt->project_id);
  if (!project_opt.has_value()) {
    throw std::runtime_error("project not found for ai message thread");
  }

  git_->open_or_init(project_opt->root_path);
  if (project_opt->git_remote_url.has_value()) {
    git_->set_remote("origin", project_opt->git_remote_url.value());
  }

  const std::string rel_path = holder::core::ai_message_rel_path(message_id);
  const std::string trash_rel = holder::core::ai_message_trash_rel_path(message_id);
  const auto src_path = git_->repo_dir() / trash_rel;
  const auto dst_path = git_->repo_dir() / rel_path;
  if (!fs_->exists(src_path)) {
    throw std::runtime_error("ai message content missing");
  }
  fs_->create_directories(dst_path.parent_path());
  fs_->rename(src_path, dst_path);

  static constexpr const char* SQL =
      "UPDATE ai_messages SET deleted_at = NULL WHERE message_id = ?;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare restore ai message failed");
  }
  bind_text(stmt, 1, message_id);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "restore ai message failed");
  }

  if (fts_) {
    fts_->upsert_message(message_id, msg_opt->thread_id, project_opt->project_id, msg_opt->content);
  }

  git_->remove_path(trash_rel);
  git_->stage_path(rel_path);
  git_->commit("Restore ai message " + message_id);
}

void AiMessageRepo::remove(const std::string& message_id) {
  static constexpr const char* SQL = "DELETE FROM ai_messages WHERE message_id = ?;";

  const auto msg_opt = get(message_id);
  std::string thread_id;
  if (msg_opt.has_value()) {
    thread_id = msg_opt->thread_id;
  }

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare delete ai message failed");
  }

  bind_text(stmt, 1, message_id);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "delete ai message failed");
  }

  if (!thread_id.empty()) {
    const auto thread_opt = thread_repo_.get(thread_id);
    if (thread_opt.has_value()) {
      const auto project_opt = project_repo_.get(thread_opt->project_id);
      if (project_opt.has_value()) {
        link_repo_.delete_links_from(project_opt->project_id, message_id);
        link_repo_.delete_links_to_typed(project_opt->project_id, message_id, "ai_message");
        git_->open_or_init(project_opt->root_path);
        if (project_opt->git_remote_url.has_value()) {
          git_->set_remote("origin", project_opt->git_remote_url.value());
        }
        const std::string rel_path = holder::core::ai_message_rel_path(message_id);
        const std::string trash_rel = holder::core::ai_message_trash_rel_path(message_id);
        const auto full_path = git_->repo_dir() / trash_rel;
        const auto rel_full = git_->repo_dir() / rel_path;
        if (fs_->exists(rel_full)) {
          fs_->remove(rel_full);
          git_->remove_path(rel_path);
        }
        if (fs_->exists(full_path)) {
          fs_->remove(full_path);
          git_->remove_path(trash_rel);
          git_->commit("Remove ai message " + message_id);
        }
      }
    }
  }
}

void AiMessageRepo::update_links(const std::string& message_id) {
  const auto msg_opt = get(message_id);
  if (!msg_opt.has_value()) {
    throw std::runtime_error("ai message not found: " + message_id);
  }

  const auto thread_opt = thread_repo_.get(msg_opt->thread_id);
  if (!thread_opt.has_value()) {
    throw std::runtime_error("thread not found for ai message");
  }

  const auto project_opt = project_repo_.get(thread_opt->project_id);
  if (!project_opt.has_value()) {
    throw std::runtime_error("project not found for ai message thread");
  }

  git_->open_or_init(project_opt->root_path);
  if (project_opt->git_remote_url.has_value()) {
    git_->set_remote("origin", project_opt->git_remote_url.value());
  }

  const std::string rel_path = holder::core::ai_message_rel_path(message_id);
  const auto full_path = git_->repo_dir() / rel_path;
  if (!fs_->exists(full_path)) {
    throw std::runtime_error("ai message file missing");
  }

  const auto links = link_repo_.list_outgoing(project_opt->project_id, message_id);
  const auto front_matter =
      holder::core::render_ai_message_front_matter(msg_opt.value(), project_opt->project_id, links);
  git_->write_file(rel_path, front_matter + msg_opt->content);
  git_->stage_path(rel_path);
  git_->commit("Update ai message links " + message_id);
}

std::optional<holder::model::AiMessage> AiMessageRepo::get(const std::string& message_id) const {
  static constexpr const char* SQL =
      "SELECT message_id, thread_id, role, source, provider, model, content, created_at, deleted_at, prompt_hash, meta_json "
      "FROM ai_messages WHERE message_id = ? LIMIT 1;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare get ai message failed");
  }

  bind_text(stmt, 1, message_id);

  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    auto message = read_message(stmt);
    sqlite3_finalize(stmt);
    return message;
  }
  sqlite3_finalize(stmt);
  if (rc == SQLITE_DONE) {
    return std::nullopt;
  }

  throw_sqlite(db_.handle(), "get ai message failed");
  return std::nullopt; // LCOV_EXCL_LINE
}

} // namespace holder::ai
