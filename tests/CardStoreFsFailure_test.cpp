#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "card/CardPaths.h"
#include "card/CardRepo.h"
#include "card/CardStore.h"
#include "index/FtsIndexer.h"
#include "model/Card.h"
#include "model/Project.h"
#include "platform/Db.h"
#include "project/ProjectRepo.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path make_temp_dir() {
  const auto base = std::filesystem::temp_directory_path();
  const auto suffix = std::to_string(
      static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())
  );
  auto dir = base / ("holder_card_store_fs_fail_" + suffix);
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

} // namespace

TEST_CASE("CardStore update recreates file if missing", "[cardstore]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  const auto project_root = dir / "repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);

  holder::model::Card card;
  card.card_id = "deadbeef";
  card.project_id = "proj-1";
  card.title = "Missing";
  card.created_at = 1;
  card.updated_at = 1;
  store.create(card, "body");

  const auto rel_path = holder::core::card_rel_path(card.card_id);
  std::filesystem::remove(project_root / rel_path);

  store.update_content(card.card_id, "new body", std::nullopt, 2);
  REQUIRE(std::filesystem::exists(project_root / rel_path));
}

TEST_CASE("CardStore update_links fails if card file missing", "[cardstore]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  const auto project_root = dir / "repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);

  holder::model::Card card;
  card.card_id = "cafebabe";
  card.project_id = "proj-1";
  card.title = "Missing";
  card.created_at = 1;
  card.updated_at = 1;
  store.create(card, "body");

  const auto rel_path = holder::core::card_rel_path(card.card_id);
  std::filesystem::remove(project_root / rel_path);

  REQUIRE_THROWS(store.update_links(card.card_id, 2));
}

TEST_CASE("CardStore trash fails if card file missing", "[cardstore]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  const auto project_root = dir / "repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);

  holder::model::Card card;
  card.card_id = "feedface";
  card.project_id = "proj-1";
  card.title = "Missing";
  card.created_at = 1;
  card.updated_at = 1;
  store.create(card, "body");

  const auto rel_path = holder::core::card_rel_path(card.card_id);
  std::filesystem::remove(project_root / rel_path);

  REQUIRE_THROWS(store.trash(card.card_id, 10));
}

TEST_CASE("CardStore restore fails if trash file missing", "[cardstore]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  const auto project_root = dir / "repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);

  holder::model::Card card;
  card.card_id = "baddcafe";
  card.project_id = "proj-1";
  card.title = "Restore";
  card.created_at = 1;
  card.updated_at = 1;
  store.create(card, "body");
  store.trash(card.card_id, 10);

  const auto trash_rel = holder::core::card_trash_rel_path(card.card_id);
  std::filesystem::remove(project_root / trash_rel);

  REQUIRE_THROWS(store.restore(card.card_id, 11));
}

TEST_CASE("CardStore update fails on rel_path mismatch", "[cardstore]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  const auto project_root = dir / "repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);

  holder::model::Card card;
  card.card_id = "faceb00c";
  card.project_id = "proj-1";
  card.title = "Mismatch";
  card.created_at = 1;
  card.updated_at = 1;
  store.create(card, "body");

  db.exec("UPDATE cards SET rel_path = 'cards/xx/yy/other.md' WHERE card_id = 'faceb00c';");

  REQUIRE_THROWS(store.update_content(card.card_id, "next", std::nullopt, 3));
}
