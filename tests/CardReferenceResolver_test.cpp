#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "card/CardReferenceResolver.h"
#include "card/CardRepo.h"
#include "core_test_helpers.h"
#include "model/Card.h"
#include "model/Project.h"
#include "project/ProjectRepo.h"

#include <optional>
#include <string>

namespace {

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
    const std::optional<long long>& deleted_at = std::nullopt
) {
  holder::model::Card card;
  card.card_id = card_id;
  card.project_id = project_id;
  card.title = title;
  card.rel_path = "cards/" + card_id + ".md";
  card.created_at = 1;
  card.updated_at = 1;
  card.deleted_at = deleted_at;
  cards.create(card);
}

void require_not_found(const holder::card::CardReferenceResult& result) {
  REQUIRE(result.status == holder::card::CardReferenceStatus::NotFound);
  REQUIRE_FALSE(result.match_kind.has_value());
  REQUIRE_FALSE(result.card.has_value());
  REQUIRE(result.candidates.empty());
}

void require_resolved(
    const holder::card::CardReferenceResult& result,
    holder::card::CardReferenceMatchKind match_kind,
    const std::string& card_id
) {
  REQUIRE(result.status == holder::card::CardReferenceStatus::Resolved);
  REQUIRE(result.match_kind == match_kind);
  REQUIRE(result.card.has_value());
  REQUIRE(result.card->card_id == card_id);
  REQUIRE(result.candidates.empty());
}

} // namespace

TEST_CASE("CardReferenceResolver resolves authoritative full UUIDs", "[card][reference]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");
  create_project(db, "proj-2");

  holder::card::CardRepo cards(db);
  holder::card::CardReferenceResolver resolver(cards);
  const std::string live_id = "abcdef12-1111-4111-8111-111111111111";
  const std::string trashed_id = "01890f3e-7b5a-7cc8-98c4-dc0c0c07398f";
  const std::string missing_id = "99999999-9999-4999-8999-999999999999";
  create_card(cards, live_id, "proj-1", "Live");
  create_card(cards, trashed_id, "proj-1", "Trashed", 50);
  create_card(cards, "77777777-7777-4777-8777-777777777777", "proj-1", missing_id);

  require_resolved(
      resolver.resolve("proj-1", live_id, holder::model::CardScope::Live),
      holder::card::CardReferenceMatchKind::FullId,
      live_id
  );
  require_resolved(
      resolver.resolve(
          "proj-1",
          "ABCDEF12-1111-4111-8111-111111111111",
          holder::model::CardScope::Either
      ),
      holder::card::CardReferenceMatchKind::FullId,
      live_id
  );
  require_not_found(resolver.resolve("proj-1", live_id, holder::model::CardScope::Trashed));
  require_resolved(
      resolver.resolve("proj-1", trashed_id, holder::model::CardScope::Trashed),
      holder::card::CardReferenceMatchKind::FullId,
      trashed_id
  );
  require_resolved(
      resolver.resolve("proj-1", trashed_id, holder::model::CardScope::Either),
      holder::card::CardReferenceMatchKind::FullId,
      trashed_id
  );
  require_not_found(resolver.resolve("proj-2", live_id, holder::model::CardScope::Either));

  // A valid full UUID is authoritative, even when another card has that exact
  // title.
  require_not_found(resolver.resolve("proj-1", missing_id, holder::model::CardScope::Either));
}

TEST_CASE("CardReferenceResolver recognizes canonical UUID prefixes", "[card][reference]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");

  holder::card::CardRepo cards(db);
  holder::card::CardReferenceResolver resolver(cards);
  const std::string card_id = "abcdef12-3456-4567-8567-123456789abc";
  create_card(cards, card_id, "proj-1", "Prefix card");
  create_card(cards, "99999999-abcd-4999-8999-abcdef123456", "proj-1", "Fragment only");

  require_resolved(
      resolver.resolve("proj-1", "abcdef12", holder::model::CardScope::Live),
      holder::card::CardReferenceMatchKind::IdPrefix,
      card_id
  );
  require_resolved(
      resolver.resolve("proj-1", "ABCDEF12", holder::model::CardScope::Live),
      holder::card::CardReferenceMatchKind::IdPrefix,
      card_id
  );
  require_resolved(
      resolver.resolve("proj-1", "abcdef12-3456-4", holder::model::CardScope::Live),
      holder::card::CardReferenceMatchKind::IdPrefix,
      card_id
  );
}

TEST_CASE(
    "CardReferenceResolver sends ineligible prefix-like input to exact title",
    "[card][reference]"
) {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");

  holder::card::CardRepo cards(db);
  holder::card::CardReferenceResolver resolver(cards);
  const std::string short_title_id = "11111111-1111-4111-8111-111111111111";
  const std::string malformed_title_id = "22222222-2222-4222-8222-222222222222";
  const std::string hyphen_title_id = "33333333-3333-4333-8333-333333333333";
  const std::string version_title_id = "34444444-4444-4444-8444-444444444444";
  const std::string variant_title_id = "35555555-5555-4555-8555-555555555555";
  create_card(cards, short_title_id, "proj-1", "abcdef1");
  create_card(cards, malformed_title_id, "proj-1", "abcdef12x");
  create_card(cards, hyphen_title_id, "proj-1", "abcd-ef12");
  create_card(cards, version_title_id, "proj-1", "abcdef12-3456-5");
  create_card(cards, variant_title_id, "proj-1", "abcdef12-3456-4567-7");

  require_resolved(
      resolver.resolve("proj-1", "abcdef1", holder::model::CardScope::Live),
      holder::card::CardReferenceMatchKind::ExactTitle,
      short_title_id
  );
  require_resolved(
      resolver.resolve("proj-1", "abcdef12x", holder::model::CardScope::Live),
      holder::card::CardReferenceMatchKind::ExactTitle,
      malformed_title_id
  );
  require_resolved(
      resolver.resolve("proj-1", "abcd-ef12", holder::model::CardScope::Live),
      holder::card::CardReferenceMatchKind::ExactTitle,
      hyphen_title_id
  );
  require_resolved(
      resolver.resolve("proj-1", "abcdef12-3456-5", holder::model::CardScope::Live),
      holder::card::CardReferenceMatchKind::ExactTitle,
      version_title_id
  );
  require_resolved(
      resolver.resolve("proj-1", "abcdef12-3456-4567-7", holder::model::CardScope::Live),
      holder::card::CardReferenceMatchKind::ExactTitle,
      variant_title_id
  );
}

TEST_CASE(
    "CardReferenceResolver prefix ambiguity is terminal and scope aware",
    "[card][reference]"
) {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");

  holder::card::CardRepo cards(db);
  holder::card::CardReferenceResolver resolver(cards);
  const std::string live_id = "12345678-1111-4111-8111-111111111111";
  const std::string trashed_id = "12345678-2222-7222-8222-222222222222";
  create_card(cards, live_id, "proj-1", "Live prefix");
  create_card(cards, trashed_id, "proj-1", "Trashed prefix", 50);
  create_card(cards, "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "proj-1", "12345678");

  require_resolved(
      resolver.resolve("proj-1", "12345678", holder::model::CardScope::Live),
      holder::card::CardReferenceMatchKind::IdPrefix,
      live_id
  );
  require_resolved(
      resolver.resolve("proj-1", "12345678", holder::model::CardScope::Trashed),
      holder::card::CardReferenceMatchKind::IdPrefix,
      trashed_id
  );

  const auto either = resolver.resolve("proj-1", "12345678", holder::model::CardScope::Either);
  REQUIRE(either.status == holder::card::CardReferenceStatus::Ambiguous);
  REQUIRE(either.match_kind == holder::card::CardReferenceMatchKind::IdPrefix);
  REQUIRE_FALSE(either.card.has_value());
  REQUIRE(either.candidates.size() == 2);
  REQUIRE(either.candidates[0].card_id == live_id);
  REQUIRE(either.candidates[1].card_id == trashed_id);
  REQUIRE_FALSE(either.candidates[0].deleted_at.has_value());
  REQUIRE(either.candidates[1].deleted_at == 50);
}

TEST_CASE(
    "CardReferenceResolver falls through an unmatched prefix to its "
    "original title",
    "[card][reference]"
) {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");

  holder::card::CardRepo cards(db);
  holder::card::CardReferenceResolver resolver(cards);
  const std::string card_id = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
  create_card(cards, card_id, "proj-1", "DEADBEEF");

  require_resolved(
      resolver.resolve("proj-1", "DEADBEEF", holder::model::CardScope::Live),
      holder::card::CardReferenceMatchKind::ExactTitle,
      card_id
  );
}

TEST_CASE(
    "CardReferenceResolver resolves exact titles without fuzzy matching",
    "[card][reference]"
) {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");
  create_project(db, "proj-2");

  holder::card::CardRepo cards(db);
  holder::card::CardReferenceResolver resolver(cards);
  const std::string first_id = "41111111-1111-4111-8111-111111111111";
  const std::string second_id = "42222222-2222-4222-8222-222222222222";
  const std::string trashed_id = "43333333-3333-7333-8333-333333333333";
  const std::string unique_id = "44444444-4444-4444-8444-444444444444";
  create_card(cards, first_id, "proj-1", "Roadmap");
  create_card(cards, second_id, "proj-1", "Roadmap");
  create_card(cards, trashed_id, "proj-1", "Roadmap", 50);
  create_card(cards, unique_id, "proj-1", "Release plan");
  create_card(cards, "45555555-5555-4555-8555-555555555555", "proj-1", "Roadmaps");
  create_card(cards, "46666666-6666-4666-8666-666666666666", "proj-1", "roadmap");
  create_card(cards, "47777777-7777-4777-8777-777777777777", "proj-2", "Release plan");

  require_resolved(
      resolver.resolve("proj-1", "Release plan", holder::model::CardScope::Live),
      holder::card::CardReferenceMatchKind::ExactTitle,
      unique_id
  );

  const auto live = resolver.resolve("proj-1", "Roadmap", holder::model::CardScope::Live);
  REQUIRE(live.status == holder::card::CardReferenceStatus::Ambiguous);
  REQUIRE(live.match_kind == holder::card::CardReferenceMatchKind::ExactTitle);
  REQUIRE_FALSE(live.card.has_value());
  REQUIRE(live.candidates.size() == 2);
  REQUIRE_FALSE(live.candidates[0].deleted_at.has_value());
  REQUIRE_FALSE(live.candidates[1].deleted_at.has_value());

  require_resolved(
      resolver.resolve("proj-1", "Roadmap", holder::model::CardScope::Trashed),
      holder::card::CardReferenceMatchKind::ExactTitle,
      trashed_id
  );

  const auto either = resolver.resolve("proj-1", "Roadmap", holder::model::CardScope::Either);
  REQUIRE(either.status == holder::card::CardReferenceStatus::Ambiguous);
  REQUIRE(either.match_kind == holder::card::CardReferenceMatchKind::ExactTitle);
  REQUIRE(either.candidates.size() == 2);

  require_not_found(resolver.resolve("proj-1", "Road", holder::model::CardScope::Either));
  require_not_found(resolver.resolve("proj-1", "ROADMAP", holder::model::CardScope::Either));
  require_not_found(resolver.resolve("proj-2", "Roadmap", holder::model::CardScope::Either));
}

TEST_CASE("CardReferenceResolver propagates repository errors", "[card][reference]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1");

  holder::card::CardRepo cards(db);
  holder::card::CardReferenceResolver resolver(cards);
  db.close();

  REQUIRE_THROWS_WITH(
      resolver.resolve(
          "proj-1",
          "11111111-1111-4111-8111-111111111111",
          holder::model::CardScope::Either
      ),
      Catch::Matchers::ContainsSubstring("prepare find card by id failed: unknown sqlite error")
  );
  REQUIRE_THROWS_WITH(
      resolver.resolve("proj-1", "12345678", holder::model::CardScope::Either),
      Catch::Matchers::ContainsSubstring(
          "prepare find cards by id prefix failed: unknown sqlite error"
      )
  );
  REQUIRE_THROWS_WITH(
      resolver.resolve("proj-1", "Title", holder::model::CardScope::Either),
      Catch::Matchers::ContainsSubstring(
          "prepare find cards by exact title failed: unknown sqlite error"
      )
  );
}
