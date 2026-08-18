#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "ai/AiMessageFrontMatter.h"
#include "ai/AiMessagePaths.h"
#include "ai/AiMessageRepo.h"
#include "ai/AiThreadRepo.h"
#include "card/CardFrontMatter.h"
#include "card/CardPaths.h"
#include "card/CardRepo.h"
#include "card/CardStore.h"
#include "index/FtsIndexer.h"
#include "model/AiMessage.h"
#include "model/AiThread.h"
#include "model/Card.h"
#include "model/Project.h"
#include "platform/Db.h"
#include "platform/Fs.h"
#include "project/ProjectRepo.h"
#include "project/Rebuilder.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sqlite3.h>
#include <string>

namespace {

std::filesystem::path make_temp_dir() {
  const auto base = std::filesystem::temp_directory_path();
  const auto suffix = std::to_string(
      static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())
  );
  auto dir = base / ("holder_fs_adapter_test_" + suffix);
  std::filesystem::create_directories(dir);
  return dir;
}

void apply_schema(holder::platform::Db& db) {
  std::filesystem::path schema_path = SCHEMA_SQL_PATH;
  std::ifstream in(schema_path);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);
}

void create_project(
    holder::platform::Db& db,
    const std::string& project_id,
    const std::string& root_path
) {
  holder::project::ProjectRepo repo(db);
  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Project";
  project.root_path = root_path;
  project.privacy_mode = "plain";
  project.project_key_id.reset();
  project.created_at = 1;
  project.updated_at = 1;
  repo.create(project);
}

std::optional<std::string> select_text_optional(
    holder::platform::Db& db,
    const std::string& sql,
    const std::string& key
) {
  sqlite3_stmt* stmt = nullptr;
  REQUIRE(sqlite3_prepare_v2(db.handle(), sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK);
  std::optional<std::string> out;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
      out = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    }
  }
  sqlite3_finalize(stmt);
  return out;
}

long long select_int64(holder::platform::Db& db, const std::string& sql, const std::string& key) {
  sqlite3_stmt* stmt = nullptr;
  REQUIRE(sqlite3_prepare_v2(db.handle(), sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK);
  long long out = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    out = sqlite3_column_int64(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return out;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  if (!out.is_open()) {
    throw std::runtime_error("Failed to open for write: " + path.string());
  }
  out << content;
}

struct FailingFs final : holder::core::Fs {
  holder::core::RealFs real;
  std::optional<std::filesystem::path> fail_rename_from;
  std::optional<std::filesystem::path> fail_read_path;

  bool exists(const std::filesystem::path& path) const override { return real.exists(path); }
  void create_directories(const std::filesystem::path& path) const override {
    real.create_directories(path);
  }
  void rename(const std::filesystem::path& from, const std::filesystem::path& to) const override {
    if (fail_rename_from.has_value() && from == fail_rename_from.value()) {
      throw std::runtime_error("rename failed");
    }
    real.rename(from, to);
  }
  void remove(const std::filesystem::path& path) const override { real.remove(path); }
  long long last_write_time_seconds(const std::filesystem::path& path) const override {
    return real.last_write_time_seconds(path);
  }
  std::string read_file(const std::filesystem::path& path) const override {
    if (fail_read_path.has_value() && path == fail_read_path.value()) {
      throw std::runtime_error("read failed");
    }
    return real.read_file(path);
  }
  void write_file(const std::filesystem::path& path, const std::string& content) const override {
    real.write_file(path, content);
  }
};

} // namespace

TEST_CASE("CardStore trash propagates fs rename failure", "[fs]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  const auto project_root = dir / "repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  FailingFs fs;
  holder::card::CardStore store(db, &fts, &fs);

  holder::model::Card card;
  card.card_id = "deadbeef";
  card.project_id = "proj-1";
  card.title = "Fail";
  card.created_at = 1;
  card.updated_at = 1;
  store.create(card, "body");

  const auto rel_path = holder::core::card_rel_path(card.card_id);
  const auto src_path = project_root / rel_path;
  fs.fail_rename_from = src_path;

  REQUIRE_THROWS(store.trash(card.card_id, 10));
  REQUIRE(std::filesystem::exists(src_path));

  holder::card::CardRepo repo(db);
  const auto fetched = repo.get(card.card_id);
  REQUIRE(fetched.has_value());
  REQUIRE(!fetched->deleted_at.has_value());
}

TEST_CASE("AiMessageRepo trash propagates fs rename failure", "[fs]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  const auto project_root = dir / "repo";
  create_project(db, "proj-1", project_root.string());

  holder::model::AiThread thread;
  thread.thread_id = "thread-1";
  thread.project_id = "proj-1";
  thread.title = "Thread";
  thread.created_at = 1;
  thread.updated_at = 1;
  holder::ai::AiThreadRepo thread_repo(db);
  thread_repo.create(thread);

  holder::index::FtsIndexer fts(db);
  FailingFs fs;
  holder::ai::AiMessageRepo repo(db, &fts, &fs);

  holder::model::AiMessage msg;
  msg.message_id = "msg-1";
  msg.thread_id = "thread-1";
  msg.role = "user";
  msg.source = "manual";
  msg.content = "hello";
  msg.created_at = 2;
  repo.append(msg);

  const auto rel_path = holder::core::ai_message_rel_path(msg.message_id);
  const auto src_path = project_root / rel_path;
  fs.fail_rename_from = src_path;

  REQUIRE_THROWS(repo.trash(msg.message_id, 10));
  REQUIRE(std::filesystem::exists(src_path));

  const auto fetched = repo.get(msg.message_id);
  REQUIRE(fetched.has_value());
  REQUIRE(!fetched->deleted_at.has_value());
}

TEST_CASE("Rebuilder propagates fs read failure", "[fs]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  const std::string project_id = "proj-1";
  const auto root = dir / "repo";
  std::filesystem::create_directories(root);
  create_project(db, project_id, root.string());

  holder::model::Card card;
  card.card_id = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
  card.project_id = project_id;
  card.title = "Card";
  card.rel_path = holder::core::card_rel_path(card.card_id);
  card.created_at = 1;
  card.updated_at = 1;
  const auto raw = holder::core::render_card_front_matter(card, {}) + "body";
  const auto full_path = root / card.rel_path;
  std::filesystem::create_directories(full_path.parent_path());
  std::ofstream out(full_path);
  out << raw;
  out.close();

  holder::index::FtsIndexer fts(db);
  FailingFs fs;
  fs.fail_read_path = full_path;

  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Project";
  project.root_path = root.string();
  project.created_at = 1;
  project.updated_at = 1;

  holder::store::Rebuilder rebuilder(db, &fts, &fs);
  REQUIRE_THROWS(rebuilder.rebuild_project(project));
}

TEST_CASE("CardStore restore propagates fs rename failure", "[fs]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  const auto project_root = dir / "repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  FailingFs fs;
  holder::card::CardStore store(db, &fts, &fs);

  holder::model::Card card;
  card.card_id = "restorefail";
  card.project_id = "proj-1";
  card.title = "Restore";
  card.created_at = 1;
  card.updated_at = 1;
  store.create(card, "body");
  store.trash(card.card_id, 10);

  const auto trash_rel = holder::core::card_trash_rel_path(card.card_id);
  const auto src_path = project_root / trash_rel;
  fs.fail_rename_from = src_path;

  REQUIRE_THROWS(store.restore(card.card_id, 11));
  REQUIRE(std::filesystem::exists(src_path));

  holder::card::CardRepo repo(db);
  const auto fetched = repo.get(card.card_id);
  REQUIRE(fetched.has_value());
  REQUIRE(fetched->deleted_at.has_value());
}

TEST_CASE("AiMessageRepo restore propagates fs rename failure", "[fs]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  const auto project_root = dir / "repo";
  create_project(db, "proj-1", project_root.string());

  holder::model::AiThread thread;
  thread.thread_id = "thread-1";
  thread.project_id = "proj-1";
  thread.title = "Thread";
  thread.created_at = 1;
  thread.updated_at = 1;
  holder::ai::AiThreadRepo thread_repo(db);
  thread_repo.create(thread);

  holder::index::FtsIndexer fts(db);
  FailingFs fs;
  holder::ai::AiMessageRepo repo(db, &fts, &fs);

  holder::model::AiMessage msg;
  msg.message_id = "msg-restore";
  msg.thread_id = "thread-1";
  msg.role = "user";
  msg.source = "manual";
  msg.content = "hello";
  msg.created_at = 2;
  repo.append(msg);
  repo.trash(msg.message_id, 10);

  const auto trash_rel = holder::core::ai_message_trash_rel_path(msg.message_id);
  const auto src_path = project_root / trash_rel;
  fs.fail_rename_from = src_path;

  REQUIRE_THROWS(repo.restore(msg.message_id));
  REQUIRE(std::filesystem::exists(src_path));

  const auto fetched = repo.get(msg.message_id);
  REQUIRE(fetched.has_value());
  REQUIRE(fetched->deleted_at.has_value());
}

TEST_CASE("Rebuilder rebuilds cards/messages with defaults, links, trash and FTS", "[rebuild]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  const std::string project_id = "proj-1";
  const auto root = dir / "repo";
  std::filesystem::create_directories(root);
  create_project(db, project_id, root.string());

  // Active card without front matter -> card_id from stem + title from first markdown heading.
  const auto card_a_rel = holder::core::card_rel_path("abcd1234");
  write_file(root / card_a_rel, "# Heading Title\nbody\n");

  // Active card with front matter + links.
  holder::model::Card card_b;
  card_b.card_id = "beef5678";
  card_b.project_id = project_id;
  card_b.title = "Linked Card";
  card_b.rel_path = holder::core::card_rel_path(card_b.card_id);
  card_b.created_at = 11;
  card_b.updated_at = 12;
  holder::model::CardLink card_link;
  card_link.to_card_id = "abcd1234";
  card_link.to_type = "card";
  card_link.kind = "ref";
  card_link.created_at = 0; // rebuilt default path
  write_file(
      root / card_b.rel_path,
      holder::core::render_card_front_matter(card_b, {card_link}) + "linked body\n"
  );

  // Trash card without front matter -> deleted_at gets mtime.
  const auto card_t_rel = holder::core::card_trash_rel_path("dead9999");
  write_file(root / card_t_rel, "trashed card body\n");

  // Active message without front matter -> message_id/thread_id defaults + role/source defaults.
  const auto msg_a_rel = holder::core::ai_message_rel_path("mesa1234");
  write_file(root / msg_a_rel, "hello ai\n");

  // Active message with front matter + link.
  holder::model::AiMessage msg_b;
  msg_b.message_id = "msgb5678";
  msg_b.thread_id = "threadxyz";
  msg_b.role = "assistant";
  msg_b.source = "manual";
  msg_b.content = "answer";
  msg_b.created_at = 20;
  holder::model::CardLink msg_link;
  msg_link.to_card_id = "abcd1234";
  msg_link.to_type = "card";
  msg_link.kind = "ref";
  msg_link.created_at = 0; // rebuilt default path
  write_file(
      root / holder::core::ai_message_rel_path(msg_b.message_id),
      holder::core::render_ai_message_front_matter(msg_b, project_id, {msg_link}) + "answer\n"
  );

  // Trash message without front matter -> deleted_at gets mtime.
  const auto msg_t_rel = holder::core::ai_message_trash_rel_path("tras9999");
  write_file(root / msg_t_rel, "old ai\n");

  holder::index::FtsIndexer fts(db);
  holder::store::Rebuilder rebuilder(db, &fts);

  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Project";
  project.root_path = root.string();
  project.created_at = 1;
  project.updated_at = 1;

  const auto stats = rebuilder.rebuild_project(project);
  REQUIRE(stats.cards == 3);
  REQUIRE(stats.ai_messages == 3);
  REQUIRE(stats.ai_threads == 3);
  REQUIRE(stats.links == 2);

  // Derived title + created/updated fallback from mtime.
  const auto title =
      select_text_optional(db, "SELECT title FROM cards WHERE card_id = ?;", "abcd1234");
  REQUIRE(title.has_value());
  REQUIRE(title.value() == "Heading Title");
  const auto created =
      select_int64(db, "SELECT created_at FROM cards WHERE card_id = ?;", "abcd1234");
  const auto updated =
      select_int64(db, "SELECT updated_at FROM cards WHERE card_id = ?;", "abcd1234");
  REQUIRE(created > 0);
  REQUIRE(updated == created);

  // Trash card was marked deleted.
  const auto card_deleted =
      select_int64(db, "SELECT deleted_at FROM cards WHERE card_id = ?;", "dead9999");
  REQUIRE(card_deleted > 0);

  // Message defaults were applied for no-front-matter file.
  const auto role =
      select_text_optional(db, "SELECT role FROM ai_messages WHERE message_id = ?;", "mesa1234");
  const auto source =
      select_text_optional(db, "SELECT source FROM ai_messages WHERE message_id = ?;", "mesa1234");
  const auto thread = select_text_optional(
      db,
      "SELECT thread_id FROM ai_messages WHERE message_id = ?;",
      "mesa1234"
  );
  REQUIRE(role.has_value());
  REQUIRE(role.value() == "assistant");
  REQUIRE(source.has_value());
  REQUIRE(source.value() == "manual_paste");
  REQUIRE(thread.has_value());
  REQUIRE(thread.value() == "mesa1234");

  // Message link + card link were inserted and normalized by rebuilder.
  holder::card::LinkRepo links(db);
  const auto out_card = links.list_outgoing(project_id, "beef5678");
  REQUIRE(out_card.size() == 1);
  REQUIRE(out_card[0].to_type == "card");
  REQUIRE(out_card[0].kind == "ref");
  REQUIRE(out_card[0].created_at > 0);
  const auto out_msg = links.list_outgoing(project_id, "msgb5678");
  REQUIRE(out_msg.size() == 1);
  REQUIRE(out_msg[0].to_type == "card");
  REQUIRE(out_msg[0].kind == "ref");
  REQUIRE(out_msg[0].created_at > 0);

  // FTS receives non-deleted rows.
  REQUIRE(fts.search_cards(project_id, "heading", 10, 0).size() >= 1);
  REQUIRE(fts.search_messages(project_id, "answer", 10, 0).size() >= 1);
}

TEST_CASE("Rebuilder derive_title falls back when heading is blank", "[rebuild]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  const std::string project_id = "proj-1";
  const auto root = dir / "repo";
  std::filesystem::create_directories(root);
  create_project(db, project_id, root.string());

  // First line is whitespace-only heading marker; derive_title should fall back to card_id.
  const auto card_rel = holder::core::card_rel_path("abcd1234");
  write_file(root / card_rel, "#    \nbody\n");

  holder::index::FtsIndexer fts(db);
  holder::store::Rebuilder rebuilder(db, &fts);

  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Project";
  project.root_path = root.string();
  project.created_at = 1;
  project.updated_at = 1;

  const auto stats = rebuilder.rebuild_project(project);
  REQUIRE(stats.cards == 1);

  const auto title =
      select_text_optional(db, "SELECT title FROM cards WHERE card_id = ?;", "abcd1234");
  REQUIRE(title.has_value());
  REQUIRE(title.value() == "abcd1234");
}

TEST_CASE("Rebuilder derive_title falls back when first line is whitespace only", "[rebuild]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  const std::string project_id = "proj-1";
  const auto root = dir / "repo";
  std::filesystem::create_directories(root);
  create_project(db, project_id, root.string());

  const auto card_rel = holder::core::card_rel_path("abcd1234");
  write_file(root / card_rel, "    \nbody\n");

  holder::index::FtsIndexer fts(db);
  holder::store::Rebuilder rebuilder(db, &fts);

  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Project";
  project.root_path = root.string();
  project.created_at = 1;
  project.updated_at = 1;

  const auto stats = rebuilder.rebuild_project(project);
  REQUIRE(stats.cards == 1);

  const auto title =
      select_text_optional(db, "SELECT title FROM cards WHERE card_id = ?;", "abcd1234");
  REQUIRE(title.has_value());
  REQUIRE(title.value() == "abcd1234");
}

TEST_CASE("Rebuilder rejects invalid card front matter", "[rebuild]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  const std::string project_id = "proj-1";
  const auto root = dir / "repo";
  std::filesystem::create_directories(root);
  create_project(db, project_id, root.string());

  const auto rel = holder::core::card_rel_path("abcd1234");
  write_file(root / rel, "---\nnot: [valid\n---\nbody\n");

  holder::index::FtsIndexer fts(db);
  holder::store::Rebuilder rebuilder(db, &fts);

  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Project";
  project.root_path = root.string();
  project.created_at = 1;
  project.updated_at = 1;

  REQUIRE_THROWS(rebuilder.rebuild_project(project));
}

TEST_CASE("Rebuilder rejects ai message front matter with missing message_id", "[rebuild]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  const std::string project_id = "proj-1";
  const auto root = dir / "repo";
  std::filesystem::create_directories(root);
  create_project(db, project_id, root.string());

  // Valid card so rebuild reaches ai_messages phase.
  const auto card_rel = holder::core::card_rel_path("abcd1234");
  write_file(root / card_rel, "# ok\n");

  const auto msg_rel = holder::core::ai_message_rel_path("mesa1234");
  write_file(
      root / msg_rel,
      "---\nthread_id: thread-1\nrole: assistant\nsource: manual\ncreated_at: 1\n---\nbody\n"
  );

  holder::index::FtsIndexer fts(db);
  holder::store::Rebuilder rebuilder(db, &fts);

  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Project";
  project.root_path = root.string();
  project.created_at = 1;
  project.updated_at = 1;

  REQUIRE_THROWS(rebuilder.rebuild_project(project));
}

TEST_CASE("Rebuilder rejects short derived card_id from filename", "[rebuild]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  const std::string project_id = "proj-1";
  const auto root = dir / "repo";
  std::filesystem::create_directories(root);
  create_project(db, project_id, root.string());

  const auto short_card_path = root / "cards" / "misc" / "abc.md";
  write_file(short_card_path, "body\n");

  holder::index::FtsIndexer fts(db);
  holder::store::Rebuilder rebuilder(db, &fts);

  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Project";
  project.root_path = root.string();
  project.created_at = 1;
  project.updated_at = 1;

  REQUIRE_THROWS(rebuilder.rebuild_project(project));
}

TEST_CASE("Rebuilder rejects invalid ai message front matter", "[rebuild]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  const std::string project_id = "proj-1";
  const auto root = dir / "repo";
  std::filesystem::create_directories(root);
  create_project(db, project_id, root.string());

  const auto card_rel = holder::core::card_rel_path("abcd1234");
  write_file(root / card_rel, "# ok\n");

  const auto msg_rel = holder::core::ai_message_rel_path("mesa1234");
  write_file(root / msg_rel, "---\nnot: [valid\n---\nbody\n");

  holder::index::FtsIndexer fts(db);
  holder::store::Rebuilder rebuilder(db, &fts);

  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Project";
  project.root_path = root.string();
  project.created_at = 1;
  project.updated_at = 1;

  REQUIRE_THROWS(rebuilder.rebuild_project(project));
}

TEST_CASE("Rebuilder rejects ai message path mismatch with message_id", "[rebuild]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  const std::string project_id = "proj-1";
  const auto root = dir / "repo";
  std::filesystem::create_directories(root);
  create_project(db, project_id, root.string());

  const auto card_rel = holder::core::card_rel_path("abcd1234");
  write_file(root / card_rel, "# ok\n");

  holder::model::AiMessage msg;
  msg.message_id = "other5678";
  msg.thread_id = "thread-a";
  msg.role = "assistant";
  msg.source = "manual";
  msg.created_at = 1;
  const auto wrong_rel = holder::core::ai_message_rel_path("mesa1234");
  write_file(
      root / wrong_rel,
      holder::core::render_ai_message_front_matter(msg, project_id, {}) + "body\n"
  );

  holder::index::FtsIndexer fts(db);
  holder::store::Rebuilder rebuilder(db, &fts);

  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Project";
  project.root_path = root.string();
  project.created_at = 1;
  project.updated_at = 1;

  REQUIRE_THROWS(rebuilder.rebuild_project(project));
}

TEST_CASE("Rebuilder rejects cards with unresolved parent graph", "[rebuild]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  const std::string project_id = "proj-1";
  const auto root = dir / "repo";
  std::filesystem::create_directories(root);
  create_project(db, project_id, root.string());

  holder::model::Card child;
  child.card_id = "abcd1234";
  child.project_id = project_id;
  child.title = "Child";
  child.parent_card_id = "missing999";
  child.rel_path = holder::core::card_rel_path(child.card_id);
  child.created_at = 1;
  child.updated_at = 1;
  write_file(root / child.rel_path, holder::core::render_card_front_matter(child, {}) + "body\n");

  holder::index::FtsIndexer fts(db);
  holder::store::Rebuilder rebuilder(db, &fts);

  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Project";
  project.root_path = root.string();
  project.created_at = 1;
  project.updated_at = 1;

  try {
    (void)rebuilder.rebuild_project(project);
    FAIL("Expected unresolved parent_card_id during rebuild");
  } catch (const std::exception& ex) {
    REQUIRE(std::string(ex.what()) == "unresolved parent_card_id during rebuild");
  }
}

TEST_CASE("Rebuilder tolerates invalid trashed ai messages when configured", "[rebuild]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  const std::string project_id = "proj-1";
  const auto root = dir / "repo";
  std::filesystem::create_directories(root);
  create_project(db, project_id, root.string());

  const auto card_rel = holder::core::card_rel_path("abcd1234");
  write_file(root / card_rel, "# ok\n");

  holder::model::AiMessage good_msg;
  good_msg.message_id = "good5678";
  good_msg.thread_id = "thread-a";
  good_msg.role = "assistant";
  good_msg.source = "manual";
  good_msg.created_at = 1;
  write_file(
      root / holder::core::ai_message_rel_path(good_msg.message_id),
      holder::core::render_ai_message_front_matter(good_msg, project_id, {}) + "good body\n"
  );

  const auto bad_trash_rel = holder::core::ai_message_trash_rel_path("tras9999");
  write_file(root / bad_trash_rel, "---\nnot: [valid\n---\nbad body\n");

  holder::index::FtsIndexer fts(db);
  holder::store::Rebuilder rebuilder(db, &fts, nullptr, true);

  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Project";
  project.root_path = root.string();
  project.created_at = 1;
  project.updated_at = 1;

  const auto stats = rebuilder.rebuild_project(project);
  REQUIRE(stats.cards == 1);
  REQUIRE(stats.ai_messages == 1);
  REQUIRE(stats.ai_threads == 1);

  holder::ai::AiMessageRepo msg_repo(db, &fts);
  const auto msgs = msg_repo.list_by_thread("thread-a");
  REQUIRE(msgs.size() == 1);
  REQUIRE(msgs[0].message_id == "good5678");
}
