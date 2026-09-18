#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "card/CardPlacementResolver.h"
#include "card/CardRepo.h"
#include "core_test_helpers.h"
#include "model/Card.h"
#include "model/Project.h"
#include "project/ProjectRepo.h"

#include <optional>
#include <string>

namespace {

using holder::card::CardPlacementIntent;
using holder::card::CardPlacementRequest;
using holder::card::CardPlacementResolver;

void create_project(holder::platform::Db& db, const std::string& project_id) {
  holder::model::Project project;
  project.project_id = project_id;
  project.name = project_id;
  project.root_path = "/tmp/" + project_id;
  project.privacy_mode = "plain";
  project.created_at = 1;
  project.updated_at = 1;
  holder::project::ProjectRepo(db).create(project);
}

void create_card(
    holder::card::CardRepo& cards,
    const std::string& card_id,
    const std::string& project_id,
    const std::string& title,
    double sort_key,
    const std::optional<std::string>& parent_card_id = std::nullopt,
    long long updated_at = 1,
    const std::optional<long long>& deleted_at = std::nullopt
) {
  holder::model::Card card;
  card.card_id = card_id;
  card.project_id = project_id;
  card.title = title;
  card.rel_path = "cards/" + card_id + ".md";
  card.parent_card_id = parent_card_id;
  card.sort_key = sort_key;
  card.created_at = 1;
  card.updated_at = updated_at;
  card.deleted_at = deleted_at;
  cards.create(card);
}

CardPlacementRequest into_request(const std::string& target_card_id) {
  CardPlacementRequest request;
  request.intent = CardPlacementIntent::Into;
  request.target_card_id = target_card_id;
  return request;
}

CardPlacementRequest before_after_request(CardPlacementIntent intent, const std::string& target_card_id) {
  CardPlacementRequest request;
  request.intent = intent;
  request.target_card_id = target_card_id;
  return request;
}

CardPlacementRequest simple_request(
    CardPlacementIntent intent,
    const std::optional<std::string>& parent_card_id = std::nullopt
) {
  CardPlacementRequest request;
  request.intent = intent;
  request.parent_card_id = parent_card_id;
  return request;
}

} // namespace

TEST_CASE("CardPlacementResolver Into targets a card with no children", "[card][placement]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");

  holder::card::CardRepo cards(db);
  CardPlacementResolver resolver(cards);
  create_card(cards, "aaaaaaaa-0000-4000-8000-000000000001", "proj-1", "A", 10.0);
  create_card(cards, "aaaaaaaa-0000-4000-8000-000000000002", "proj-1", "B", 20.0);

  const auto result = resolver.resolve(
      "proj-1",
      "aaaaaaaa-0000-4000-8000-000000000001",
      into_request("aaaaaaaa-0000-4000-8000-000000000002")
  );
  REQUIRE(result.parent_card_id == "aaaaaaaa-0000-4000-8000-000000000002");
  REQUIRE(result.sort_key == 0.0);
  REQUIRE(result.moved_into_title == "B");
}

TEST_CASE("CardPlacementResolver Into targets a card with children", "[card][placement]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");

  holder::card::CardRepo cards(db);
  CardPlacementResolver resolver(cards);
  create_card(cards, "aaaaaaaa-0000-4000-8000-000000000001", "proj-1", "A", 10.0);
  create_card(cards, "aaaaaaaa-0000-4000-8000-000000000002", "proj-1", "B", 20.0);
  create_card(
      cards,
      "aaaaaaaa-0000-4000-8000-000000000003",
      "proj-1",
      "B-child",
      5.0,
      "aaaaaaaa-0000-4000-8000-000000000002"
  );

  const auto result = resolver.resolve(
      "proj-1",
      "aaaaaaaa-0000-4000-8000-000000000001",
      into_request("aaaaaaaa-0000-4000-8000-000000000002")
  );
  REQUIRE(result.parent_card_id == "aaaaaaaa-0000-4000-8000-000000000002");
  REQUIRE(result.sort_key == 6.0);
  REQUIRE(result.moved_into_title == "B");
}

TEST_CASE("CardPlacementResolver Into rejects moving a card into its own descendant", "[card][placement]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");

  holder::card::CardRepo cards(db);
  CardPlacementResolver resolver(cards);
  create_card(cards, "aaaaaaaa-0000-4000-8000-000000000001", "proj-1", "A", 10.0);
  create_card(
      cards,
      "aaaaaaaa-0000-4000-8000-000000000002",
      "proj-1",
      "A-child",
      10.0,
      "aaaaaaaa-0000-4000-8000-000000000001"
  );

  REQUIRE_THROWS_WITH(
      resolver.resolve(
          "proj-1",
          "aaaaaaaa-0000-4000-8000-000000000001",
          into_request("aaaaaaaa-0000-4000-8000-000000000002")
      ),
      "move_would_create_cycle"
  );
}

TEST_CASE("CardPlacementResolver Before and After use real siblings", "[card][placement]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");

  holder::card::CardRepo cards(db);
  CardPlacementResolver resolver(cards);
  create_card(cards, "bbbbbbbb-0000-4000-8000-000000000001", "proj-1", "X", 10.0);
  create_card(cards, "bbbbbbbb-0000-4000-8000-000000000002", "proj-1", "Y", 20.0);
  create_card(cards, "bbbbbbbb-0000-4000-8000-000000000003", "proj-1", "Z", 30.0);

  const auto before_y = resolver.resolve(
      "proj-1",
      "bbbbbbbb-0000-4000-8000-000000000003",
      before_after_request(CardPlacementIntent::Before, "bbbbbbbb-0000-4000-8000-000000000002")
  );
  REQUIRE_FALSE(before_y.parent_card_id.has_value());
  REQUIRE(before_y.sort_key == 15.0);

  const auto after_y = resolver.resolve(
      "proj-1",
      "bbbbbbbb-0000-4000-8000-000000000001",
      before_after_request(CardPlacementIntent::After, "bbbbbbbb-0000-4000-8000-000000000002")
  );
  REQUIRE(after_y.sort_key == 25.0);
}

TEST_CASE("CardPlacementResolver Before extrapolates a dense sort-key gap", "[card][placement]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");

  holder::card::CardRepo cards(db);
  CardPlacementResolver resolver(cards);
  create_card(cards, "cccccccc-0000-4000-8000-000000000001", "proj-1", "S1", 10.0);
  create_card(cards, "cccccccc-0000-4000-8000-000000000002", "proj-1", "S2", 10.00001);
  create_card(cards, "cccccccc-0000-4000-8000-000000000003", "proj-1", "S3", 500.0);

  const auto result = resolver.resolve(
      "proj-1",
      "cccccccc-0000-4000-8000-000000000003",
      before_after_request(CardPlacementIntent::Before, "cccccccc-0000-4000-8000-000000000002")
  );
  // Gap between S1 and S2 is under 0.0001 -- falls back to left - 1.0.
  REQUIRE(result.sort_key == 9.0);
}

TEST_CASE("CardPlacementResolver Before with self as target is invalid", "[card][placement]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");

  holder::card::CardRepo cards(db);
  CardPlacementResolver resolver(cards);
  create_card(cards, "dddddddd-0000-4000-8000-000000000001", "proj-1", "A", 10.0);

  REQUIRE_THROWS_WITH(
      resolver.resolve(
          "proj-1",
          "dddddddd-0000-4000-8000-000000000001",
          before_after_request(CardPlacementIntent::Before, "dddddddd-0000-4000-8000-000000000001")
      ),
      "invalid_target"
  );
}

TEST_CASE("CardPlacementResolver ToStart/ToEnd honor an explicit parent override", "[card][placement]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");

  holder::card::CardRepo cards(db);
  CardPlacementResolver resolver(cards);
  create_card(cards, "eeeeeeee-0000-4000-8000-000000000001", "proj-1", "Parent1", 1.0);
  create_card(cards, "eeeeeeee-0000-4000-8000-000000000002", "proj-1", "Parent2", 2.0);
  create_card(
      cards,
      "eeeeeeee-0000-4000-8000-000000000003",
      "proj-1",
      "Source",
      10.0,
      "eeeeeeee-0000-4000-8000-000000000001"
  );
  create_card(
      cards,
      "eeeeeeee-0000-4000-8000-000000000004",
      "proj-1",
      "P2-child-1",
      5.0,
      "eeeeeeee-0000-4000-8000-000000000002"
  );
  create_card(
      cards,
      "eeeeeeee-0000-4000-8000-000000000005",
      "proj-1",
      "P2-child-2",
      15.0,
      "eeeeeeee-0000-4000-8000-000000000002"
  );

  const auto to_start = resolver.resolve(
      "proj-1",
      "eeeeeeee-0000-4000-8000-000000000003",
      simple_request(CardPlacementIntent::ToStart, "eeeeeeee-0000-4000-8000-000000000002")
  );
  REQUIRE(to_start.parent_card_id == "eeeeeeee-0000-4000-8000-000000000002");
  REQUIRE(to_start.sort_key == 4.0);

  const auto to_end = resolver.resolve(
      "proj-1",
      "eeeeeeee-0000-4000-8000-000000000003",
      simple_request(CardPlacementIntent::ToEnd, "eeeeeeee-0000-4000-8000-000000000002")
  );
  REQUIRE(to_end.parent_card_id == "eeeeeeee-0000-4000-8000-000000000002");
  REQUIRE(to_end.sort_key == 16.0);
}

TEST_CASE("CardPlacementResolver ToStart/ToEnd no-op when the target parent has no other children", "[card][placement]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");

  holder::card::CardRepo cards(db);
  CardPlacementResolver resolver(cards);
  create_card(cards, "ffffffff-0000-4000-8000-000000000001", "proj-1", "Alone", 42.0);

  const auto result = resolver.resolve(
      "proj-1",
      "ffffffff-0000-4000-8000-000000000001",
      simple_request(CardPlacementIntent::ToStart)
  );
  REQUIRE_FALSE(result.parent_card_id.has_value());
  REQUIRE(result.sort_key == 42.0);
}

TEST_CASE("CardPlacementResolver excludes deleted tied siblings", "[card][placement]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");

  holder::card::CardRepo cards(db);
  CardPlacementResolver resolver(cards);
  create_card(cards, "abababab-0000-4000-8000-000000000001", "proj-1", "Later", 20.0, std::nullopt, 2);
  create_card(cards, "abababab-0000-4000-8000-000000000002", "proj-1", "Earlier", 20.0, std::nullopt, 3);
  create_card(
      cards,
      "abababab-0000-4000-8000-000000000004",
      "proj-1",
      "Deleted",
      1.0,
      std::nullopt,
      4,
      5
  );

  const auto ordered = resolver.resolve(
      "proj-1", "abababab-0000-4000-8000-000000000001", simple_request(CardPlacementIntent::ToStart)
  );
  REQUIRE(ordered.sort_key == 19.0);
}

TEST_CASE("CardPlacementResolver Left/Right move within siblings", "[card][placement]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");

  holder::card::CardRepo cards(db);
  CardPlacementResolver resolver(cards);
  create_card(cards, "11111111-0000-4000-8000-000000000001", "proj-1", "First", 10.0);
  create_card(cards, "11111111-0000-4000-8000-000000000002", "proj-1", "Middle", 20.0);
  create_card(cards, "11111111-0000-4000-8000-000000000003", "proj-1", "Last", 30.0);

  // Boundary: Left on the first sibling is a no-op.
  const auto left_first = resolver.resolve(
      "proj-1",
      "11111111-0000-4000-8000-000000000001",
      simple_request(CardPlacementIntent::Left)
  );
  REQUIRE(left_first.sort_key == 10.0);

  // Middle: Left on the middle sibling swaps it ahead of First (First has no sibling to its
  // own left once Middle is excluded, so this extrapolates to First.sort_key - 1, then splits
  // the gap): (9 + 10) / 2 == 9.5.
  const auto left_middle = resolver.resolve(
      "proj-1",
      "11111111-0000-4000-8000-000000000002",
      simple_request(CardPlacementIntent::Left)
  );
  REQUIRE(left_middle.sort_key == 9.5);

  // Middle: Right on the middle sibling swaps it past Last (Last has no sibling to its own
  // right once Middle is excluded, so this extrapolates to Last.sort_key + 1, then splits the
  // gap): (30 + 31) / 2 == 30.5.
  const auto right_middle = resolver.resolve(
      "proj-1",
      "11111111-0000-4000-8000-000000000002",
      simple_request(CardPlacementIntent::Right)
  );
  REQUIRE(right_middle.sort_key == 30.5);

  // Boundary: Right on the last sibling is a no-op.
  const auto right_last = resolver.resolve(
      "proj-1",
      "11111111-0000-4000-8000-000000000003",
      simple_request(CardPlacementIntent::Right)
  );
  REQUIRE(right_last.sort_key == 30.0);
}

TEST_CASE("CardPlacementResolver UpLevel from a nested card sets moved_into_title", "[card][placement]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");

  holder::card::CardRepo cards(db);
  CardPlacementResolver resolver(cards);
  create_card(cards, "22222222-0000-4000-8000-000000000001", "proj-1", "Grandparent", 1.0);
  create_card(
      cards,
      "22222222-0000-4000-8000-000000000002",
      "proj-1",
      "Parent",
      2.0,
      "22222222-0000-4000-8000-000000000001"
  );
  create_card(
      cards,
      "22222222-0000-4000-8000-000000000003",
      "proj-1",
      "Source",
      3.0,
      "22222222-0000-4000-8000-000000000002"
  );

  const auto result = resolver.resolve(
      "proj-1",
      "22222222-0000-4000-8000-000000000003",
      simple_request(CardPlacementIntent::UpLevel)
  );
  REQUIRE(result.parent_card_id == "22222222-0000-4000-8000-000000000001");
  REQUIRE(result.moved_into_title == "Grandparent");
}

TEST_CASE("CardPlacementResolver UpLevel from a root card is already_at_project_root", "[card][placement]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");

  holder::card::CardRepo cards(db);
  CardPlacementResolver resolver(cards);
  create_card(cards, "33333333-0000-4000-8000-000000000001", "proj-1", "Root", 1.0);

  REQUIRE_THROWS_WITH(
      resolver.resolve(
          "proj-1",
          "33333333-0000-4000-8000-000000000001",
          simple_request(CardPlacementIntent::UpLevel)
      ),
      "already_at_project_root"
  );
}

TEST_CASE("CardPlacementResolver Into/Before/After require target_card_id", "[card][placement]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");

  holder::card::CardRepo cards(db);
  CardPlacementResolver resolver(cards);
  create_card(cards, "44444444-0000-4000-8000-000000000001", "proj-1", "A", 1.0);

  CardPlacementRequest request;
  request.intent = CardPlacementIntent::Into;
  REQUIRE_THROWS_WITH(
      resolver.resolve("proj-1", "44444444-0000-4000-8000-000000000001", request),
      "missing_target_card_id"
  );
}

TEST_CASE("CardPlacementResolver rejects a missing or cross-project card", "[card][placement]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");
  create_project(db, "proj-2");

  holder::card::CardRepo cards(db);
  CardPlacementResolver resolver(cards);
  create_card(cards, "55555555-0000-4000-8000-000000000001", "proj-1", "A", 1.0);
  create_card(cards, "55555555-0000-4000-8000-000000000002", "proj-2", "X", 1.0);
  create_card(
      cards,
      "55555555-0000-4000-8000-000000000003",
      "proj-1",
      "Trashed",
      1.0,
      std::nullopt,
      1,
      50
  );

  REQUIRE_THROWS_WITH(
      resolver.resolve(
          "proj-1",
          "does-not-exist",
          simple_request(CardPlacementIntent::UpLevel)
      ),
      "card_not_found"
  );
  REQUIRE_THROWS_WITH(
      resolver.resolve(
          "proj-1",
          "55555555-0000-4000-8000-000000000003",
          simple_request(CardPlacementIntent::UpLevel)
      ),
      "card_not_found"
  );
  REQUIRE_THROWS_WITH(
      resolver.resolve(
          "proj-1",
          "55555555-0000-4000-8000-000000000002",
          simple_request(CardPlacementIntent::UpLevel)
      ),
      "cross_project_move_forbidden"
  );
  REQUIRE_THROWS_WITH(
      resolver.resolve(
          "proj-1",
          "55555555-0000-4000-8000-000000000001",
          into_request("does-not-exist")
      ),
      "target_not_found"
  );
}

TEST_CASE("CardPlacementResolver handles tied siblings and parent edge cases", "[card][placement]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");
  holder::card::CardRepo cards(db);
  CardPlacementResolver resolver(cards);
  const std::string source = "66666666-0000-4000-8000-000000000001";
  const std::string title_first = "66666666-0000-4000-8000-000000000002";
  const std::string title_last = "66666666-0000-4000-8000-000000000003";
  const std::string deleted_parent = "66666666-0000-4000-8000-000000000004";
  create_card(cards, source, "proj-1", "Source", 10.0);
  // Identical sort_key and updated_at: only the title decides the sibling order.
  create_card(cards, title_first, "proj-1", "A", 20.0, std::nullopt, 2);
  create_card(cards, title_last, "proj-1", "Z", 20.0, std::nullopt, 2);
  create_card(cards, deleted_parent, "proj-1", "Deleted parent", 30.0, std::nullopt, 1, 9);

  REQUIRE(resolver.resolve("proj-1", source,
                           simple_request(CardPlacementIntent::ToEnd)).sort_key == 21.0);
  // Siblings order as [A, Z], so "after A" sits in a zero-width gap and steps past it (21.0).
  // Were Z first, A would be last and the result would be the midpoint 20.5.
  REQUIRE(resolver.resolve("proj-1", source,
                           before_after_request(CardPlacementIntent::After, title_first))
              .sort_key == 21.0);
  REQUIRE(resolver.resolve("proj-1", source,
                           before_after_request(CardPlacementIntent::Before, title_last))
              .sort_key == 19.0);
  REQUIRE_THROWS_WITH(
      resolver.resolve("proj-1", source, simple_request(CardPlacementIntent::ToStart, deleted_parent)),
      "target_not_found"
  );
  REQUIRE_THROWS_WITH(
      resolver.resolve("proj-1", source, simple_request(CardPlacementIntent::Left, deleted_parent)),
      "target_not_found"
  );

  const std::string orphan = "66666666-0000-4000-8000-000000000005";
  create_card(cards, orphan, "proj-1", "Orphan", 1.0, deleted_parent);
  const auto up = resolver.resolve("proj-1", orphan, simple_request(CardPlacementIntent::UpLevel));
  REQUIRE_FALSE(up.parent_card_id.has_value());
}
