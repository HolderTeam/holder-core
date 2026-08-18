#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "card/CardRepo.h"
#include "model/Card.h"
#include "model/Project.h"
#include "platform/Db.h"
#include "project/ProjectRepo.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sqlite3.h>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

std::filesystem::path find_schema_sql() {
#ifdef SCHEMA_SQL_PATH
  std::filesystem::path p = SCHEMA_SQL_PATH;
  if (std::filesystem::exists(p)) return p;
#endif
  namespace fs = std::filesystem;
  fs::path p1 = fs::current_path() / "schema" / "schema.sql";
  if (fs::exists(p1)) return p1;
  fs::path p2 = fs::current_path().parent_path() / "schema" / "schema.sql";
  if (fs::exists(p2)) return p2;
  throw std::runtime_error("schema.sql not found for tests");
}

std::filesystem::path make_temp_dir() {
  const auto base = std::filesystem::temp_directory_path();
  auto pattern = (base / "holder_card_test_XXXXXX").string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');

#ifndef _WIN32
  char* created = ::mkdtemp(writable.data());
  if (created == nullptr) {
    throw std::runtime_error("mkdtemp failed creating holder_card_test temp dir");
  }
  return std::filesystem::path(created);
#else
  for (int attempt = 0; attempt < 64; ++attempt) {
    const auto suffix = std::to_string(
        static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())
    );
    auto dir = base / ("holder_card_test_" + suffix);
    std::error_code ec;
    if (std::filesystem::create_directory(dir, ec)) {
      return dir;
    }
    if (ec && ec != std::errc::file_exists) {
      throw std::filesystem::filesystem_error("create_directory", dir, ec);
    }
  }
  throw std::runtime_error("failed to create unique holder_card_test temp dir");
#endif
}

void apply_schema(holder::platform::Db& db) {
  const auto schema_path = find_schema_sql();
  std::ifstream in(schema_path);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);
}

void create_project(holder::platform::Db& db, const std::string& project_id) {
  holder::project::ProjectRepo repo(db);
  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Project";
  project.root_path = "/tmp/project";
  project.created_at = 1;
  project.updated_at = 1;
  repo.create(project);
}

int sqlite_interrupt_cb(void* data) {
  auto* flag = static_cast<int*>(data);
  return (flag && *flag) ? 1 : 0;
}

} // namespace

TEST_CASE("CardRepo CRUD", "[cardrepo]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  create_project(db, "proj-1");

  holder::card::CardRepo repo(db);

  holder::model::Card card;
  card.card_id = "card-1";
  card.project_id = "proj-1";
  card.title = "First";
  card.rel_path = "cards/ca/rd/card-1.md";
  card.sort_key = 1.0;
  card.created_at = 10;
  card.updated_at = 10;

  repo.create(card);

  const auto fetched = repo.get("card-1");
  REQUIRE(fetched.has_value());
  REQUIRE(fetched->title == "First");
  REQUIRE(fetched->parent_card_id.has_value() == false);

  auto list_root = repo.list_roots("proj-1");
  REQUIRE(list_root.size() == 1);

  holder::model::Card child;
  child.card_id = "card-2";
  child.project_id = "proj-1";
  child.title = "Child";
  child.rel_path = "cards/ca/rd/card-2.md";
  child.parent_card_id = "card-1";
  child.sort_key = 2.0;
  child.created_at = 11;
  child.updated_at = 11;
  repo.create(child);

  list_root = repo.list_roots("proj-1");
  REQUIRE(list_root.size() == 1);

  auto list_child = repo.list_children("proj-1", "card-1");
  REQUIRE(list_child.size() == 1);
  REQUIRE(list_child[0].card_id == "card-2");

  repo.update_title("card-1", "Renamed", 20);
  const auto renamed = repo.get("card-1");
  REQUIRE(renamed.has_value());
  REQUIRE(renamed->title == "Renamed");
  REQUIRE(renamed->updated_at == 20);

  repo.move("card-2", std::nullopt, 3.0, 30);
  list_root = repo.list_roots("proj-1");
  REQUIRE(list_root.size() == 2);

  repo.soft_delete("card-2", 40, 41);
  const auto deleted = repo.get("card-2");
  REQUIRE(deleted.has_value());
  REQUIRE(deleted->deleted_at.has_value());
  REQUIRE(deleted->deleted_at.value() == 40);
  REQUIRE(deleted->updated_at == 41);
}

TEST_CASE("CardRepo counts children and handles deleted_at on create", "[cardrepo]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  create_project(db, "proj-1");

  holder::card::CardRepo repo(db);

  holder::model::Card parent;
  parent.card_id = "parent-1";
  parent.project_id = "proj-1";
  parent.title = "Parent";
  parent.rel_path = "cards/pa/re/parent-1.md";
  parent.sort_key = 1.0;
  parent.created_at = 1;
  parent.updated_at = 1;
  repo.create(parent);

  holder::model::Card child_visible;
  child_visible.card_id = "child-1";
  child_visible.project_id = "proj-1";
  child_visible.title = "Child 1";
  child_visible.rel_path = "cards/ch/il/child-1.md";
  child_visible.parent_card_id = parent.card_id;
  child_visible.sort_key = 2.0;
  child_visible.created_at = 2;
  child_visible.updated_at = 2;
  repo.create(child_visible);

  holder::model::Card child_deleted;
  child_deleted.card_id = "child-2";
  child_deleted.project_id = "proj-1";
  child_deleted.title = "Child 2";
  child_deleted.rel_path = "cards/ch/il/child-2.md";
  child_deleted.parent_card_id = parent.card_id;
  child_deleted.sort_key = 3.0;
  child_deleted.created_at = 3;
  child_deleted.updated_at = 3;
  child_deleted.deleted_at = 99; // exercises bind_int64_optional(value.has_value())
  repo.create(child_deleted);

  REQUIRE(repo.count_children_not_deleted("proj-1", parent.card_id) == 1);
}

TEST_CASE("CardRepo throws sqlite error when DB handle is invalid", "[cardrepo]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  create_project(db, "proj-1");

  holder::card::CardRepo repo(db);
  db.close();

  holder::model::Card card;
  card.card_id = "invalid-db";
  card.project_id = "proj-1";
  card.title = "Broken";
  card.rel_path = "cards/in/va/invalid-db.md";
  card.sort_key = 1.0;
  card.created_at = 1;
  card.updated_at = 1;

  REQUIRE_THROWS(repo.create(card));
}

TEST_CASE("CardRepo create throws on duplicate primary key", "[cardrepo]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  create_project(db, "proj-1");

  holder::card::CardRepo repo(db);

  holder::model::Card card;
  card.card_id = "dupe-card";
  card.project_id = "proj-1";
  card.title = "First";
  card.rel_path = "cards/du/pe/dupe-card.md";
  card.sort_key = 1.0;
  card.created_at = 1;
  card.updated_at = 1;

  repo.create(card);
  REQUIRE_THROWS(repo.create(card));
}

TEST_CASE("CardRepo methods throw sqlite errors when DB is closed", "[cardrepo]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  create_project(db, "proj-1");

  holder::card::CardRepo repo(db);

  holder::model::Card card;
  card.card_id = "closed-db-card";
  card.project_id = "proj-1";
  card.title = "Closed";
  card.rel_path = "cards/cl/os/closed-db-card.md";
  card.sort_key = 1.0;
  card.created_at = 1;
  card.updated_at = 1;

  db.close();

  REQUIRE_THROWS(repo.get("missing"));
  REQUIRE_THROWS(repo.list_roots("proj-1"));
  REQUIRE_THROWS(repo.list_children("proj-1", "parent"));
  REQUIRE_THROWS(repo.list_all("proj-1"));
  REQUIRE_THROWS(repo.count_all_not_deleted("proj-1"));
  REQUIRE_THROWS(repo.count_roots_not_deleted("proj-1"));
  REQUIRE_THROWS(repo.count_children_not_deleted("proj-1", "parent"));
  REQUIRE_THROWS(repo.next_sort_key("proj-1", std::nullopt));
  REQUIRE_THROWS(repo.next_sort_key("proj-1", std::optional<std::string>("parent")));
  REQUIRE_THROWS(repo.update_title("card-1", "Title", 2));
  REQUIRE_THROWS(repo.touch_updated("card-1", 2));
  REQUIRE_THROWS(repo.soft_delete("card-1", 3, 4));
  REQUIRE_THROWS(repo.restore("card-1", 5));
  REQUIRE_THROWS(repo.remove("card-1"));
  REQUIRE_THROWS(repo.move("card-1", std::nullopt, 1.5, 6));
}

TEST_CASE("CardRepo read/count queries throw on interrupted sqlite step", "[cardrepo]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  create_project(db, "proj-1");

  holder::card::CardRepo repo(db);

  holder::model::Card parent;
  parent.card_id = "parent-int";
  parent.project_id = "proj-1";
  parent.title = "Parent";
  parent.rel_path = "cards/pa/re/parent-int.md";
  parent.sort_key = 1.0;
  parent.created_at = 1;
  parent.updated_at = 1;
  repo.create(parent);

  holder::model::Card child;
  child.card_id = "child-int";
  child.project_id = "proj-1";
  child.title = "Child";
  child.rel_path = "cards/ch/il/child-int.md";
  child.parent_card_id = "parent-int";
  child.sort_key = 2.0;
  child.created_at = 2;
  child.updated_at = 2;
  repo.create(child);

  int interrupt_on = 1;
  sqlite3_progress_handler(db.handle(), 1, sqlite_interrupt_cb, &interrupt_on);

  REQUIRE_THROWS(repo.get("parent-int"));
  REQUIRE_THROWS(repo.list_roots("proj-1"));
  REQUIRE_THROWS(repo.list_children("proj-1", "parent-int"));
  REQUIRE_THROWS(repo.list_all("proj-1"));
  REQUIRE_THROWS(repo.count_all_not_deleted("proj-1"));
  REQUIRE_THROWS(repo.count_roots_not_deleted("proj-1"));
  REQUIRE_THROWS(repo.count_children_not_deleted("proj-1", "parent-int"));
  REQUIRE_THROWS(repo.next_sort_key("proj-1", std::nullopt));
  REQUIRE_THROWS(repo.next_sort_key("proj-1", std::optional<std::string>("parent-int")));

  sqlite3_progress_handler(db.handle(), 0, nullptr, nullptr);
}

TEST_CASE("CardRepo list/count/next_sort throw on interrupted step under load", "[cardrepo]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  create_project(db, "proj-1");

  holder::card::CardRepo repo(db);

  // Seed enough rows to ensure SELECTs execute enough VM ops for interrupt callback.
  for (int i = 0; i < 500; ++i) {
    holder::model::Card c;
    c.card_id = "root-int-" + std::to_string(i);
    c.project_id = "proj-1";
    c.title = "Root";
    c.rel_path = "cards/ro/ot/root-int-" + std::to_string(i) + ".md";
    c.sort_key = static_cast<double>(i);
    c.created_at = i + 1;
    c.updated_at = i + 1;
    repo.create(c);
  }

  holder::model::Card parent;
  parent.card_id = "parent-heavy";
  parent.project_id = "proj-1";
  parent.title = "Parent";
  parent.rel_path = "cards/pa/re/parent-heavy.md";
  parent.sort_key = 1000.0;
  parent.created_at = 1;
  parent.updated_at = 1;
  repo.create(parent);

  for (int i = 0; i < 500; ++i) {
    holder::model::Card c;
    c.card_id = "child-heavy-" + std::to_string(i);
    c.project_id = "proj-1";
    c.title = "Child";
    c.rel_path = "cards/ch/il/child-heavy-" + std::to_string(i) + ".md";
    c.parent_card_id = parent.card_id;
    c.sort_key = static_cast<double>(i);
    c.created_at = i + 10;
    c.updated_at = i + 10;
    repo.create(c);
  }

  int interrupt_on = 1;
  sqlite3_progress_handler(db.handle(), 1, sqlite_interrupt_cb, &interrupt_on);

  REQUIRE_THROWS(repo.list_roots("proj-1"));
  REQUIRE_THROWS(repo.list_children("proj-1", "parent-lock"));
  REQUIRE_THROWS(repo.list_all("proj-1"));
  REQUIRE_THROWS(repo.count_all_not_deleted("proj-1"));
  REQUIRE_THROWS(repo.count_roots_not_deleted("proj-1"));
  REQUIRE_THROWS(repo.count_children_not_deleted("proj-1", "parent-lock"));
  REQUIRE_THROWS(repo.next_sort_key("proj-1", std::nullopt));
  REQUIRE_THROWS(repo.next_sort_key("proj-1", std::optional<std::string>("parent-lock")));

  sqlite3_progress_handler(db.handle(), 0, nullptr, nullptr);
}

TEST_CASE("CardRepo update/delete/move throw when sqlite step aborts", "[cardrepo]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  create_project(db, "proj-1");

  holder::card::CardRepo repo(db);
  holder::model::Card card;
  card.card_id = "abort-card";
  card.project_id = "proj-1";
  card.title = "Abort";
  card.rel_path = "cards/ab/or/abort-card.md";
  card.sort_key = 1.0;
  card.created_at = 1;
  card.updated_at = 1;
  repo.create(card);

  // Force sqlite3_step failures for UPDATE and DELETE (prepare still succeeds).
  db.exec("CREATE TRIGGER cards_fail_update BEFORE UPDATE ON cards "
          "BEGIN SELECT RAISE(ABORT, 'blocked update'); END;");
  db.exec("CREATE TRIGGER cards_fail_delete BEFORE DELETE ON cards "
          "BEGIN SELECT RAISE(ABORT, 'blocked delete'); END;");

  REQUIRE_THROWS(repo.update_title(card.card_id, "New", 2));
  REQUIRE_THROWS(repo.touch_updated(card.card_id, 3));
  REQUIRE_THROWS(repo.soft_delete(card.card_id, 4, 5));
  REQUIRE_THROWS(repo.restore(card.card_id, 6));
  REQUIRE_THROWS(repo.move(card.card_id, std::nullopt, 2.0, 7));
  REQUIRE_THROWS(repo.remove(card.card_id));
}

TEST_CASE("CardRepo read/count queries throw when sqlite step hits locked database", "[cardrepo]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  create_project(db, "proj-1");

  holder::card::CardRepo repo(db);

  holder::model::Card parent;
  parent.card_id = "parent-lock";
  parent.project_id = "proj-1";
  parent.title = "Parent";
  parent.rel_path = "cards/pa/re/parent-lock.md";
  parent.sort_key = 1.0;
  parent.created_at = 1;
  parent.updated_at = 1;
  repo.create(parent);

  for (int i = 0; i < 128; ++i) {
    holder::model::Card root;
    root.card_id = "root-lock-" + std::to_string(i);
    root.project_id = "proj-1";
    root.title = "Root";
    root.rel_path = "cards/ro/ot/root-lock-" + std::to_string(i) + ".md";
    root.sort_key = static_cast<double>(i + 10);
    root.created_at = i + 10;
    root.updated_at = i + 10;
    repo.create(root);

    holder::model::Card child;
    child.card_id = "child-lock-" + std::to_string(i);
    child.project_id = "proj-1";
    child.title = "Child";
    child.rel_path = "cards/ch/il/child-lock-" + std::to_string(i) + ".md";
    child.parent_card_id = "parent-lock";
    child.sort_key = static_cast<double>(i + 10);
    child.created_at = i + 10;
    child.updated_at = i + 10;
    repo.create(child);
  }

  int interrupt_on = 1;
  sqlite3_progress_handler(db.handle(), 1, sqlite_interrupt_cb, &interrupt_on);

  REQUIRE_THROWS(repo.list_roots("proj-1"));
  REQUIRE_THROWS(repo.list_children("proj-1", "parent-lock"));
  REQUIRE_THROWS(repo.list_all("proj-1"));
  REQUIRE_THROWS(repo.count_all_not_deleted("proj-1"));
  REQUIRE_THROWS(repo.count_roots_not_deleted("proj-1"));
  REQUIRE_THROWS(repo.count_children_not_deleted("proj-1", "parent-lock"));

  sqlite3_progress_handler(db.handle(), 0, nullptr, nullptr);
}
