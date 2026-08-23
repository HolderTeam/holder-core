#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "card/CardRepo.h"
#include "card/MilestoneRepo.h"
#include "core_test_helpers.h"
#include "model/Card.h"
#include "model/Milestone.h"
#include "model/Project.h"
#include "platform/Db.h"
#include "project/ProjectRepo.h"

#include <string>
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

holder::model::Milestone make_milestone(
    const std::string& milestone_id,
    const std::string& project_id,
    const std::string& card_id,
    long long start_at
) {
  holder::model::Milestone milestone;
  milestone.milestone_id = milestone_id;
  milestone.project_id = project_id;
  milestone.card_id = card_id;
  milestone.start_at = start_at;
  milestone.created_at = start_at;
  milestone.updated_at = start_at;
  return milestone;
}

} // namespace

TEST_CASE(
    "MilestoneRepo replace_for_card inserts and list_for_card reads them back ordered by start_at",
    "[milestonerepo]"
) {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");
  create_card(db, "card-a", "proj-1");

  holder::card::MilestoneRepo repo(db);
  auto later = make_milestone("mile-2", "proj-1", "card-a", 200);
  auto earlier = make_milestone("mile-1", "proj-1", "card-a", 100);
  repo.replace_for_card("proj-1", "card-a", {later, earlier});

  const auto milestones = repo.list_for_card("proj-1", "card-a");
  REQUIRE(milestones.size() == 2);
  REQUIRE(milestones[0].milestone_id == "mile-1");
  REQUIRE(milestones[1].milestone_id == "mile-2");
}

TEST_CASE("MilestoneRepo replace_for_card replaces the whole set, not upserts", "[milestonerepo]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");
  create_card(db, "card-a", "proj-1");

  holder::card::MilestoneRepo repo(db);
  repo.replace_for_card("proj-1", "card-a", {make_milestone("mile-1", "proj-1", "card-a", 100)});
  repo.replace_for_card("proj-1", "card-a", {make_milestone("mile-2", "proj-1", "card-a", 200)});

  const auto milestones = repo.list_for_card("proj-1", "card-a");
  REQUIRE(milestones.size() == 1);
  REQUIRE(milestones[0].milestone_id == "mile-2");
}

TEST_CASE("MilestoneRepo replace_for_card with an empty list clears existing milestones", "[milestonerepo]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");
  create_card(db, "card-a", "proj-1");

  holder::card::MilestoneRepo repo(db);
  repo.replace_for_card("proj-1", "card-a", {make_milestone("mile-1", "proj-1", "card-a", 100)});
  repo.replace_for_card("proj-1", "card-a", {});

  REQUIRE(repo.list_for_card("proj-1", "card-a").empty());
}

TEST_CASE(
    "MilestoneRepo replace_for_card rejects a milestone with mismatched project_id or card_id",
    "[milestonerepo]"
) {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");
  create_card(db, "card-a", "proj-1");

  holder::card::MilestoneRepo repo(db);
  REQUIRE_THROWS(
      repo.replace_for_card("proj-1", "card-a", {make_milestone("mile-1", "proj-1", "card-b", 100)})
  );
}

TEST_CASE("MilestoneRepo delete_for_card removes a card's milestones", "[milestonerepo]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");
  create_card(db, "card-a", "proj-1");

  holder::card::MilestoneRepo repo(db);
  repo.replace_for_card("proj-1", "card-a", {make_milestone("mile-1", "proj-1", "card-a", 100)});
  repo.delete_for_card("proj-1", "card-a");

  REQUIRE(repo.list_for_card("proj-1", "card-a").empty());
}

TEST_CASE("MilestoneRepo delete_for_card on a card with no milestones is a harmless no-op", "[milestonerepo]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");
  create_card(db, "card-a", "proj-1");

  holder::card::MilestoneRepo repo(db);
  REQUIRE_NOTHROW(repo.delete_for_card("proj-1", "card-a"));
}

TEST_CASE("MilestoneRepo preserves nullable end_at, kind, and description", "[milestonerepo]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");
  create_card(db, "card-a", "proj-1");

  holder::model::Milestone milestone = make_milestone("mile-1", "proj-1", "card-a", 100);
  milestone.end_at = 150;
  milestone.all_day = true;
  milestone.kind = "Renewal";
  milestone.description = "Car insurance renewal";

  holder::card::MilestoneRepo repo(db);
  repo.replace_for_card("proj-1", "card-a", {milestone});

  const auto milestones = repo.list_for_card("proj-1", "card-a");
  REQUIRE(milestones.size() == 1);
  REQUIRE(milestones[0].end_at.has_value());
  REQUIRE(milestones[0].end_at.value() == 150);
  REQUIRE(milestones[0].all_day);
  REQUIRE(milestones[0].kind == std::optional<std::string>("Renewal"));
  REQUIRE(milestones[0].description == std::optional<std::string>("Car insurance renewal"));

  holder::model::Milestone bare = make_milestone("mile-2", "proj-1", "card-a", 200);
  repo.replace_for_card("proj-1", "card-a", {bare});
  const auto bare_milestones = repo.list_for_card("proj-1", "card-a");
  REQUIRE(bare_milestones.size() == 1);
  REQUIRE_FALSE(bare_milestones[0].end_at.has_value());
  REQUIRE_FALSE(bare_milestones[0].all_day);
  REQUIRE_FALSE(bare_milestones[0].kind.has_value());
  REQUIRE_FALSE(bare_milestones[0].description.has_value());
}

TEST_CASE("MilestoneRepo list_in_range returns milestones within range, ordered by start_at", "[milestonerepo]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");
  create_card(db, "card-a", "proj-1");
  create_card(db, "card-b", "proj-1");

  holder::card::MilestoneRepo repo(db);
  repo.replace_for_card("proj-1", "card-a", {make_milestone("mile-late", "proj-1", "card-a", 300)});
  repo.replace_for_card("proj-1", "card-b", {make_milestone("mile-early", "proj-1", "card-b", 100)});

  const auto milestones = repo.list_in_range("proj-1", 100, 300);
  REQUIRE(milestones.size() == 2);
  REQUIRE(milestones[0].milestone_id == "mile-early");
  REQUIRE(milestones[1].milestone_id == "mile-late");
}

TEST_CASE("MilestoneRepo list_in_range excludes milestones outside the range", "[milestonerepo]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");
  create_card(db, "card-a", "proj-1");

  holder::card::MilestoneRepo repo(db);
  repo.replace_for_card(
      "proj-1",
      "card-a",
      {make_milestone("mile-before", "proj-1", "card-a", 50),
       make_milestone("mile-inside", "proj-1", "card-a", 150),
       make_milestone("mile-after", "proj-1", "card-a", 500)}
  );

  const auto milestones = repo.list_in_range("proj-1", 100, 300);
  REQUIRE(milestones.size() == 1);
  REQUIRE(milestones[0].milestone_id == "mile-inside");
}

TEST_CASE("MilestoneRepo list_in_range treats the range bounds as inclusive", "[milestonerepo]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");
  create_card(db, "card-a", "proj-1");

  holder::card::MilestoneRepo repo(db);
  repo.replace_for_card(
      "proj-1",
      "card-a",
      {make_milestone("mile-start", "proj-1", "card-a", 100),
       make_milestone("mile-end", "proj-1", "card-a", 300)}
  );

  const auto milestones = repo.list_in_range("proj-1", 100, 300);
  REQUIRE(milestones.size() == 2);
}

TEST_CASE("MilestoneRepo list_in_range excludes a trashed card's milestones", "[milestonerepo]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");
  create_card(db, "card-a", "proj-1");
  create_card(db, "card-b", "proj-1");

  holder::card::MilestoneRepo repo(db);
  repo.replace_for_card("proj-1", "card-a", {make_milestone("mile-a", "proj-1", "card-a", 100)});
  repo.replace_for_card("proj-1", "card-b", {make_milestone("mile-b", "proj-1", "card-b", 110)});

  // CardRepo::soft_delete directly, bypassing CardStore::trash's own milestone cleanup -- this
  // exercises the query's own defensive join, same rationale as TagRepo's equivalent test.
  holder::card::CardRepo cards(db);
  cards.soft_delete("card-a", 200, 200);

  const auto milestones = repo.list_in_range("proj-1", 0, 1000);
  REQUIRE(milestones.size() == 1);
  REQUIRE(milestones[0].milestone_id == "mile-b");
}

TEST_CASE("MilestoneRepo list_in_range is scoped per project", "[milestonerepo]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");
  create_project(db, "proj-2");
  create_card(db, "card-a", "proj-1");
  create_card(db, "card-b", "proj-2");

  holder::card::MilestoneRepo repo(db);
  repo.replace_for_card("proj-1", "card-a", {make_milestone("mile-a", "proj-1", "card-a", 100)});
  repo.replace_for_card("proj-2", "card-b", {make_milestone("mile-b", "proj-2", "card-b", 100)});

  const auto milestones = repo.list_in_range("proj-1", 0, 1000);
  REQUIRE(milestones.size() == 1);
  REQUIRE(milestones[0].milestone_id == "mile-a");
}

TEST_CASE("MilestoneRepo list_for_card returns nothing for a card with no milestones", "[milestonerepo]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");
  create_card(db, "card-a", "proj-1");

  holder::card::MilestoneRepo repo(db);
  REQUIRE(repo.list_for_card("proj-1", "card-a").empty());
}
