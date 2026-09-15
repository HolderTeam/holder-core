#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "card/CardFrontMatter.h"
#include "card/CardPaths.h"
#include "card/CardStore.h"
#include "card/MilestoneRepo.h"
#include "core_test_helpers.h"
#include "git/GitRepo.h"
#include "index/FtsIndexer.h"
#include "model/Card.h"
#include "model/Milestone.h"
#include "model/Project.h"
#include "project/ProjectRepo.h"
#include "project/Rebuilder.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

holder::model::Project create_project(holder::platform::Db &db,
                                      const std::string &project_id,
                                      const std::filesystem::path &root) {
  holder::model::Project project;
  project.project_id = project_id;
  project.name = project_id;
  project.root_path = root.string();
  project.privacy_mode = "plain";
  project.created_at = 1;
  project.updated_at = 1;
  holder::project::ProjectRepo(db).create(project);
  return project;
}

holder::model::Card create_card(holder::card::CardStore &store,
                                const std::string &project_id,
                                const std::string &card_id) {
  holder::model::Card card;
  card.project_id = project_id;
  card.card_id = card_id;
  card.title = card_id;
  card.created_at = 1;
  card.updated_at = 1;
  store.create(card, "Card body\n");
  return *store.get(card_id);
}

holder::model::Milestone milestone_for(const std::string &project_id,
                                       const std::string &card_id,
                                       const std::string &milestone_id) {
  holder::model::Milestone milestone;
  milestone.project_id = project_id;
  milestone.card_id = card_id;
  milestone.milestone_id = milestone_id;
  milestone.start_at = 100;
  milestone.end_at = 200;
  milestone.all_day = false;
  milestone.kind = "Review";
  milestone.description = "Original description";
  milestone.created_at = 10;
  milestone.updated_at = 10;
  return milestone;
}

std::string read_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  REQUIRE(input.is_open());
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("CardStore milestone update preserves fields and survives projection "
          "rebuild",
          "[cardstore][milestone-update][rebuild]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  const auto project = create_project(db, "proj-1", dir / "project");
  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);
  const auto card = create_card(store, project.project_id,
                                "aaaaaaaa-1111-4111-8111-111111111111");
  holder::card::MilestoneRepo milestones(db);
  const auto original = milestone_for(project.project_id, card.card_id,
                                      "11111111-1111-4111-8111-111111111111");
  auto second = milestone_for(project.project_id, card.card_id,
                              "22222222-2222-4222-8222-222222222222");
  second.start_at = 300;
  second.end_at.reset();
  milestones.replace_for_card(project.project_id, card.card_id,
                              {original, second});
  store.update_milestones(card.card_id, 20);

  holder::git::GitRepo git;
  git.open_existing(project.root_path);
  const auto before = git.head_oid();
  REQUIRE(before.has_value());

  holder::card::MilestoneUpdate update;
  update.has_kind = true;
  update.kind = "Renewal";
  const auto result = store.update_milestone(project.project_id, card.card_id,
                                             original.milestone_id, update, 30);
  REQUIRE(result.has_value());
  CHECK(result->milestone_id == original.milestone_id);
  CHECK(result->project_id == original.project_id);
  CHECK(result->card_id == original.card_id);
  CHECK(result->created_at == original.created_at);
  CHECK(result->updated_at == 30);
  CHECK(result->start_at == original.start_at);
  CHECK(result->end_at == original.end_at);
  CHECK(result->all_day == original.all_day);
  CHECK(result->kind == std::optional<std::string>("Renewal"));
  CHECK(result->description == original.description);

  git.open_existing(project.root_path);
  const auto after = git.head_oid();
  REQUIRE(after.has_value());
  CHECK(*after != *before);

  const auto projected =
      milestones.list_for_card(project.project_id, card.card_id);
  REQUIRE(projected.size() == 2);
  CHECK(projected[0].milestone_id == original.milestone_id);
  CHECK(projected[0].kind == std::optional<std::string>("Renewal"));
  CHECK(projected[1].milestone_id == second.milestone_id);
  CHECK(projected[1].kind == second.kind);

  const auto parsed = holder::core::parse_card_file(
      read_file(std::filesystem::path(project.root_path) /
                holder::core::card_rel_path(card.card_id)));
  REQUIRE(parsed.milestones.size() == 2);
  CHECK(parsed.milestones[0].milestone_id == original.milestone_id);
  CHECK(parsed.milestones[0].kind == std::optional<std::string>("Renewal"));

  const auto unchanged = store.update_milestone(
      project.project_id, card.card_id, original.milestone_id, update, 40);
  REQUIRE(unchanged.has_value());
  CHECK(unchanged->updated_at == 30);
  git.open_existing(project.root_path);
  CHECK(git.head_oid() == after);

  const auto stats =
      holder::store::Rebuilder(db, &fts).rebuild_project(project);
  CHECK(stats.milestones == 2);
  const auto rebuilt =
      milestones.list_for_card(project.project_id, card.card_id);
  REQUIRE(rebuilt.size() == 2);
  CHECK(rebuilt[0].milestone_id == original.milestone_id);
  CHECK(rebuilt[0].created_at == original.created_at);
  CHECK(rebuilt[0].updated_at == 30);
  CHECK(rebuilt[0].kind == std::optional<std::string>("Renewal"));
  CHECK(rebuilt[0].description == original.description);
}

TEST_CASE(
    "CardStore milestone update validates the result and enforces ownership",
    "[cardstore][milestone-update]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  const auto first_project = create_project(db, "proj-1", dir / "first");
  const auto second_project = create_project(db, "proj-2", dir / "second");
  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);
  const auto first_card = create_card(store, first_project.project_id,
                                      "aaaaaaaa-1111-4111-8111-111111111111");
  const auto second_card = create_card(store, second_project.project_id,
                                       "bbbbbbbb-2222-4222-8222-222222222222");
  holder::card::MilestoneRepo milestones(db);
  const auto original =
      milestone_for(first_project.project_id, first_card.card_id,
                    "11111111-1111-4111-8111-111111111111");
  milestones.replace_for_card(first_project.project_id, first_card.card_id,
                              {original});
  store.update_milestones(first_card.card_id, 20);

  holder::card::MilestoneUpdate update;
  update.start_at = 120;
  update.has_end_at = true;
  update.end_at = 300;
  update.all_day = true;
  update.has_kind = true;
  update.kind.reset();
  update.has_description = true;
  update.description.reset();
  const auto changed =
      store.update_milestone(first_project.project_id, first_card.card_id,
                             original.milestone_id, update, 30);
  REQUIRE(changed.has_value());
  CHECK(changed->start_at == 120);
  CHECK(changed->end_at == std::optional<long long>(300));
  CHECK(changed->all_day);
  CHECK_FALSE(changed->kind.has_value());
  CHECK_FALSE(changed->description.has_value());
  CHECK(changed->milestone_id == original.milestone_id);
  CHECK(changed->created_at == original.created_at);

  holder::card::MilestoneUpdate invalid_start;
  invalid_start.start_at = 301;
  CHECK_THROWS_AS(
      store.update_milestone(first_project.project_id, first_card.card_id,
                             original.milestone_id, invalid_start, 40),
      std::invalid_argument);
  holder::card::MilestoneUpdate invalid_end;
  invalid_end.has_end_at = true;
  invalid_end.end_at = 119;
  CHECK_THROWS_AS(
      store.update_milestone(first_project.project_id, first_card.card_id,
                             original.milestone_id, invalid_end, 40),
      std::invalid_argument);
  const auto after_invalid =
      milestones.list_for_card(first_project.project_id, first_card.card_id);
  REQUIRE(after_invalid.size() == 1);
  CHECK(after_invalid[0].start_at == 120);
  CHECK(after_invalid[0].end_at == std::optional<long long>(300));
  CHECK(after_invalid[0].updated_at == 30);

  CHECK_FALSE(store
                  .update_milestone(second_project.project_id,
                                    first_card.card_id, original.milestone_id,
                                    update, 50)
                  .has_value());
  CHECK_FALSE(store
                  .update_milestone(second_project.project_id,
                                    second_card.card_id, original.milestone_id,
                                    update, 50)
                  .has_value());
  CHECK_FALSE(store
                  .update_milestone(first_project.project_id,
                                    first_card.card_id, "missing", update, 50)
                  .has_value());

  store.trash(first_card.card_id, 60);
  CHECK_FALSE(store
                  .update_milestone(first_project.project_id,
                                    first_card.card_id, original.milestone_id,
                                    update, 70)
                  .has_value());
}
