#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "card/CardRepo.h"
#include "card/TagRepo.h"
#include "core_test_helpers.h"
#include "model/Card.h"
#include "model/Project.h"
#include "platform/Db.h"
#include "project/ProjectRepo.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace {

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

void create_card(holder::platform::Db& db, const std::string& card_id, const std::string& project_id) {
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

TEST_CASE("TagRepo set_tags_for_card inserts and list_tags_for_card reads them back sorted", "[tagrepo]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");
  create_card(db, "card-a", "proj-1");

  holder::card::TagRepo repo(db);
  repo.set_tags_for_card("proj-1", "card-a", {"todo", "android"}, 10);

  REQUIRE(repo.list_tags_for_card("proj-1", "card-a") == std::vector<std::string>{"android", "todo"});
}

TEST_CASE("TagRepo set_tags_for_card replaces the whole set, not upserts", "[tagrepo]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");
  create_card(db, "card-a", "proj-1");

  holder::card::TagRepo repo(db);
  repo.set_tags_for_card("proj-1", "card-a", {"todo", "android"}, 10);
  repo.set_tags_for_card("proj-1", "card-a", {"urgent"}, 20);

  REQUIRE(repo.list_tags_for_card("proj-1", "card-a") == std::vector<std::string>{"urgent"});
}

TEST_CASE("TagRepo set_tags_for_card with an empty list clears existing tags", "[tagrepo]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");
  create_card(db, "card-a", "proj-1");

  holder::card::TagRepo repo(db);
  repo.set_tags_for_card("proj-1", "card-a", {"todo"}, 10);
  repo.set_tags_for_card("proj-1", "card-a", {}, 20);

  REQUIRE(repo.list_tags_for_card("proj-1", "card-a").empty());
}

TEST_CASE("TagRepo delete_tags_for_card removes a card's tags", "[tagrepo]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");
  create_card(db, "card-a", "proj-1");

  holder::card::TagRepo repo(db);
  repo.set_tags_for_card("proj-1", "card-a", {"todo"}, 10);
  repo.delete_tags_for_card("proj-1", "card-a");

  REQUIRE(repo.list_tags_for_card("proj-1", "card-a").empty());
}

TEST_CASE("TagRepo delete_tags_for_card on a card with no tags is a harmless no-op", "[tagrepo]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");
  create_card(db, "card-a", "proj-1");

  holder::card::TagRepo repo(db);
  REQUIRE_NOTHROW(repo.delete_tags_for_card("proj-1", "card-a"));
}

TEST_CASE("TagRepo list_card_ids_with_tag finds every card carrying a tag", "[tagrepo]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");
  create_card(db, "card-a", "proj-1");
  create_card(db, "card-b", "proj-1");
  create_card(db, "card-c", "proj-1");

  holder::card::TagRepo repo(db);
  repo.set_tags_for_card("proj-1", "card-a", {"todo"}, 10);
  repo.set_tags_for_card("proj-1", "card-b", {"urgent"}, 11);
  repo.set_tags_for_card("proj-1", "card-c", {"todo", "urgent"}, 12);

  const auto with_todo = repo.list_card_ids_with_tag("proj-1", "todo");
  REQUIRE(with_todo.size() == 2);
  REQUIRE(std::find(with_todo.begin(), with_todo.end(), "card-a") != with_todo.end());
  REQUIRE(std::find(with_todo.begin(), with_todo.end(), "card-c") != with_todo.end());
}

TEST_CASE("TagRepo list_card_ids_with_tag returns nothing for an unused tag", "[tagrepo]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");
  create_card(db, "card-a", "proj-1");

  holder::card::TagRepo repo(db);
  repo.set_tags_for_card("proj-1", "card-a", {"todo"}, 10);

  REQUIRE(repo.list_card_ids_with_tag("proj-1", "nonexistent").empty());
}

TEST_CASE("TagRepo list_project_tags counts distinct tags, most-used first", "[tagrepo]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");
  create_card(db, "card-a", "proj-1");
  create_card(db, "card-b", "proj-1");
  create_card(db, "card-c", "proj-1");

  holder::card::TagRepo repo(db);
  repo.set_tags_for_card("proj-1", "card-a", {"todo"}, 10);
  repo.set_tags_for_card("proj-1", "card-b", {"todo", "urgent"}, 11);
  repo.set_tags_for_card("proj-1", "card-c", {"todo"}, 12);

  const auto tags = repo.list_project_tags("proj-1");
  REQUIRE(tags.size() == 2);
  REQUIRE(tags[0] == std::make_pair(std::string("todo"), 3));
  REQUIRE(tags[1] == std::make_pair(std::string("urgent"), 1));
}

TEST_CASE("TagRepo list_project_tags returns nothing for a project with no tags", "[tagrepo]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");

  holder::card::TagRepo repo(db);
  REQUIRE(repo.list_project_tags("proj-1").empty());
}

TEST_CASE("TagRepo tags are scoped per project", "[tagrepo]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");
  create_project(db, "proj-2");
  create_card(db, "card-a", "proj-1");
  create_card(db, "card-b", "proj-2");

  holder::card::TagRepo repo(db);
  repo.set_tags_for_card("proj-1", "card-a", {"todo"}, 10);
  repo.set_tags_for_card("proj-2", "card-b", {"todo"}, 10);

  REQUIRE(repo.list_card_ids_with_tag("proj-1", "todo") == std::vector<std::string>{"card-a"});
  REQUIRE(repo.list_card_ids_with_tag("proj-2", "todo") == std::vector<std::string>{"card-b"});
}

TEST_CASE("TagRepo reports the underlying sqlite error when card_tags is missing", "[tagrepo]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");
  create_card(db, "card-a", "proj-1");

  db.exec("DROP TABLE card_tags;");

  holder::card::TagRepo repo(db);
  REQUIRE_THROWS_WITH(
      repo.set_tags_for_card("proj-1", "card-a", {"todo"}, 10),
      Catch::Matchers::ContainsSubstring("prepare delete tags failed")
  );
  REQUIRE_THROWS_WITH(
      repo.delete_tags_for_card("proj-1", "card-a"),
      Catch::Matchers::ContainsSubstring("prepare delete tags failed")
  );
  REQUIRE_THROWS_WITH(
      repo.list_tags_for_card("proj-1", "card-a"),
      Catch::Matchers::ContainsSubstring("prepare list tags for card failed")
  );
  REQUIRE_THROWS_WITH(
      repo.list_card_ids_with_tag("proj-1", "todo"),
      Catch::Matchers::ContainsSubstring("prepare list cards with tag failed")
  );
  REQUIRE_THROWS_WITH(
      repo.list_project_tags("proj-1"),
      Catch::Matchers::ContainsSubstring("prepare list project tags failed")
  );
}

TEST_CASE("TagRepo set_tags_for_card rejects a duplicate tag in the same call", "[tagrepo]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");
  create_card(db, "card-a", "proj-1");

  holder::card::TagRepo repo(db);
  // Direct callers (unlike TagExtractor, which already de-dupes) can violate the primary key.
  // set_tags_for_card isn't wrapped in its own transaction (see the header comment for why), so
  // this throws after the first "todo" insert already landed, not as an all-or-nothing rollback.
  REQUIRE_THROWS(repo.set_tags_for_card("proj-1", "card-a", {"todo", "todo"}, 10));
  REQUIRE(repo.list_tags_for_card("proj-1", "card-a") == std::vector<std::string>{"todo"});
}
