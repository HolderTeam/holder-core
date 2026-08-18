#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "card/CardRepo.h"
#include "card/LinkRepo.h"
#include "model/Card.h"
#include "model/CardLink.h"
#include "model/Project.h"
#include "platform/Db.h"
#include "project/ProjectRepo.h"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

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
  const auto suffix = std::to_string(
      static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())
  );
  auto dir = base / ("holder_link_test_" + suffix);
  std::filesystem::create_directories(dir);
  return dir;
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

void create_card(
    holder::platform::Db& db,
    const std::string& card_id,
    const std::string& project_id
) {
  holder::card::CardRepo repo(db);
  holder::model::Card card;
  card.card_id = card_id;
  card.project_id = project_id;
  card.title = card_id;
  card.rel_path = "cards/" + card_id + ".md";
  card.sort_key = 0.0;
  card.created_at = 1;
  card.updated_at = 1;
  repo.create(card);
}

} // namespace

TEST_CASE("LinkRepo upsert/list/delete", "[linkrepo]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  create_project(db, "proj-1");
  create_card(db, "card-a", "proj-1");
  create_card(db, "card-b", "proj-1");
  create_card(db, "card-c", "proj-1");

  holder::card::LinkRepo repo(db);

  holder::model::CardLink link1;
  link1.project_id = "proj-1";
  link1.from_card_id = "card-a";
  link1.to_card_id = "card-b";
  link1.to_type = "card";
  link1.kind = "wiki";
  link1.label = "Label";
  link1.created_at = 10;

  holder::model::CardLink link2;
  link2.project_id = "proj-1";
  link2.from_card_id = "card-a";
  link2.to_card_id = "res-1";
  link2.to_type = "resource";
  link2.kind = "ref";
  link2.created_at = 11;

  repo.upsert_links("proj-1", "card-a", {link1, link2});

  auto outgoing = repo.list_outgoing("proj-1", "card-a");
  REQUIRE(outgoing.size() == 2);

  auto backlinks = repo.list_backlinks("proj-1", "card-b");
  REQUIRE(backlinks.size() == 1);
  REQUIRE(backlinks[0].from_card_id == "card-a");
  REQUIRE(backlinks[0].label.has_value());

  link1.label = "New Label";
  link1.created_at = 12;
  repo.upsert_links("proj-1", "card-a", {link1});

  outgoing = repo.list_outgoing("proj-1", "card-a");
  REQUIRE(outgoing.size() == 2);

  repo.delete_links_from("proj-1", "card-a");
  outgoing = repo.list_outgoing("proj-1", "card-a");
  REQUIRE(outgoing.empty());
}

TEST_CASE("LinkRepo upsert rejects project/from mismatch", "[linkrepo]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  create_project(db, "proj-1");
  create_project(db, "proj-2");
  create_card(db, "card-a", "proj-1");
  create_card(db, "card-b", "proj-1");

  holder::card::LinkRepo repo(db);

  holder::model::CardLink bad;
  bad.project_id = "proj-2";
  bad.from_card_id = "card-a";
  bad.to_card_id = "card-b";
  bad.to_type = "card";
  bad.kind = "ref";
  bad.created_at = 1;

  REQUIRE_THROWS(repo.upsert_links("proj-1", "card-a", {bad}));
}

TEST_CASE("LinkRepo delete_link supports type-only and type+kind filters", "[linkrepo]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  create_project(db, "proj-1");
  create_card(db, "card-a", "proj-1");
  create_card(db, "card-b", "proj-1");

  holder::card::LinkRepo repo(db);

  holder::model::CardLink l1;
  l1.project_id = "proj-1";
  l1.from_card_id = "card-a";
  l1.to_card_id = "card-b";
  l1.to_type = "card";
  l1.kind = "wiki";
  l1.created_at = 1;

  holder::model::CardLink l2;
  l2.project_id = "proj-1";
  l2.from_card_id = "card-a";
  l2.to_card_id = "card-b";
  l2.to_type = "resource";
  l2.kind = "ref";
  l2.created_at = 2;

  holder::model::CardLink l3;
  l3.project_id = "proj-1";
  l3.from_card_id = "card-a";
  l3.to_card_id = "card-b";
  l3.to_type = "resource";
  l3.kind = "embed";
  l3.created_at = 3;

  repo.upsert_links("proj-1", "card-a", {l1, l2, l3});
  REQUIRE(repo.list_outgoing("proj-1", "card-a").size() == 3);

  // has_type only branch
  repo.delete_link(
      "proj-1",
      "card-a",
      "card-b",
      std::optional<std::string>("resource"),
      std::nullopt
  );
  auto outgoing = repo.list_outgoing("proj-1", "card-a");
  REQUIRE(outgoing.size() == 1);
  REQUIRE(outgoing[0].to_type == "card");

  // Reinsert resource links and delete exact type+kind
  repo.upsert_links("proj-1", "card-a", {l2, l3});
  REQUIRE(repo.list_outgoing("proj-1", "card-a").size() == 3);
  repo.delete_link(
      "proj-1",
      "card-a",
      "card-b",
      std::optional<std::string>("resource"),
      std::optional<std::string>("embed")
  );
  outgoing = repo.list_outgoing("proj-1", "card-a");
  REQUIRE(outgoing.size() == 2);

  auto typed = repo.list_backlinks_typed("proj-1", "card-b", "resource");
  REQUIRE(typed.size() == 1);
  REQUIRE(typed[0].kind == "ref");
}

TEST_CASE("LinkRepo methods throw sqlite errors when DB is closed", "[linkrepo]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  create_project(db, "proj-1");
  create_card(db, "card-a", "proj-1");
  create_card(db, "card-b", "proj-1");

  holder::card::LinkRepo repo(db);
  db.close();

  holder::model::CardLink link;
  link.project_id = "proj-1";
  link.from_card_id = "card-a";
  link.to_card_id = "card-b";
  link.to_type = "card";
  link.kind = "ref";
  link.created_at = 1;

  REQUIRE_THROWS(repo.upsert_links("proj-1", "card-a", {link}));
  REQUIRE_THROWS(repo.list_outgoing("proj-1", "card-a"));
  REQUIRE_THROWS(repo.list_backlinks("proj-1", "card-b"));
  REQUIRE_THROWS(repo.list_backlinks_typed("proj-1", "card-b", "card"));
  REQUIRE_THROWS(repo.delete_link("proj-1", "card-a", "card-b", std::nullopt, std::nullopt));
  REQUIRE_THROWS(repo.delete_links_to_typed("proj-1", "card-b", "card"));
  REQUIRE_THROWS(repo.delete_links_from("proj-1", "card-a"));
}

TEST_CASE("LinkRepo upsert throws when sqlite step fails", "[linkrepo]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  create_project(db, "proj-1");
  create_card(db, "card-a", "proj-1");
  create_card(db, "card-b", "proj-1");

  // Force INSERT/UPSERT step failure while still allowing statement prepare.
  db.exec("CREATE TRIGGER block_link_insert "
          "BEFORE INSERT ON card_links "
          "BEGIN SELECT RAISE(ABORT, 'blocked insert'); END;");

  holder::card::LinkRepo repo(db);
  holder::model::CardLink link;
  link.project_id = "proj-1";
  link.from_card_id = "card-a";
  link.to_card_id = "card-b";
  link.to_type = "card";
  link.kind = "ref";
  link.created_at = 1;

  REQUIRE_THROWS(repo.upsert_links("proj-1", "card-a", {link}));
}

namespace {
int interrupt_progress(void*) { return 1; }
} // namespace

TEST_CASE("LinkRepo list methods throw when sqlite step is interrupted", "[linkrepo]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  create_project(db, "proj-1");
  create_card(db, "card-a", "proj-1");
  create_card(db, "card-b", "proj-1");

  holder::card::LinkRepo repo(db);
  holder::model::CardLink link;
  link.project_id = "proj-1";
  link.from_card_id = "card-a";
  link.to_card_id = "card-b";
  link.to_type = "card";
  link.kind = "ref";
  link.created_at = 1;
  repo.upsert_links("proj-1", "card-a", {link});

  sqlite3_progress_handler(db.handle(), 1, interrupt_progress, nullptr);
  REQUIRE_THROWS(repo.list_outgoing("proj-1", "card-a"));
  REQUIRE_THROWS(repo.list_backlinks("proj-1", "card-b"));
  REQUIRE_THROWS(repo.list_backlinks_typed("proj-1", "card-b", "card"));
  sqlite3_progress_handler(db.handle(), 0, nullptr, nullptr);
}

TEST_CASE("LinkRepo delete methods throw when sqlite delete step fails", "[linkrepo]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  create_project(db, "proj-1");
  create_card(db, "card-a", "proj-1");
  create_card(db, "card-b", "proj-1");

  holder::card::LinkRepo repo(db);
  holder::model::CardLink link;
  link.project_id = "proj-1";
  link.from_card_id = "card-a";
  link.to_card_id = "card-b";
  link.to_type = "card";
  link.kind = "ref";
  link.created_at = 1;
  repo.upsert_links("proj-1", "card-a", {link});

  // Force DELETE step failure while still allowing prepare/bind.
  db.exec("CREATE TRIGGER block_link_delete "
          "BEFORE DELETE ON card_links "
          "BEGIN SELECT RAISE(ABORT, 'blocked delete'); END;");

  REQUIRE_THROWS(repo.delete_link(
      "proj-1",
      "card-a",
      "card-b",
      std::optional<std::string>("card"),
      std::nullopt
  ));
  REQUIRE_THROWS(repo.delete_links_to_typed("proj-1", "card-b", "card"));
  REQUIRE_THROWS(repo.delete_links_from("proj-1", "card-a"));
}
