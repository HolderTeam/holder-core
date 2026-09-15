#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "model/Location.h"
#include "model/Project.h"
#include "model/Resource.h"
#include "card/CardFrontMatter.h"
#include "card/CardPaths.h"
#include "card/CardRepo.h"
#include "card/LinkRepo.h"
#include "git/GitOps.h"
#include "platform/Db.h"
#include "platform/Tx.h"
#include "privacy/ProjectPrivacy.h"
#include "project/ProjectRepo.h"
#include "project/Rebuilder.h"
#include "resource/LocationRepo.h"
#include "resource/LocationStore.h"
#include "resource/ResourceManifest.h"
#include "resource/MetadataMapping.h"
#include "resource/ResourcePaths.h"
#include "resource/ResourceRepo.h"
#include "resource/ResourceStore.h"
#include "core_test_helpers.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <sqlite3.h>

namespace {

std::filesystem::path find_schema_sql() {
#ifdef SCHEMA_SQL_PATH
  std::filesystem::path path = SCHEMA_SQL_PATH;
  if (std::filesystem::exists(path)) return path;
#endif
  throw std::runtime_error("schema.sql not found for tests");
}

std::filesystem::path make_temp_dir() {
  const auto suffix = std::to_string(
      static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())
  );
  auto path = std::filesystem::temp_directory_path() / ("holder_resource_test_" + suffix);
  std::filesystem::create_directories(path);
  return path;
}

void apply_schema(holder::platform::Db& db) {
  std::ifstream input(find_schema_sql());
  REQUIRE(input.is_open());
  db.exec(std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()));
}

void create_project(holder::platform::Db& db, const std::string& project_id) {
  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Project";
  project.root_path = "/tmp/project";
  project.created_at = 1;
  project.updated_at = 1;
  holder::project::ProjectRepo(db).create(project);
}

void write_text(const std::filesystem::path &path, const std::string &text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  REQUIRE(output.is_open());
  output << text;
}

holder::model::ResourceBundle sample_bundle() {
  holder::model::ResourceBundle bundle;
  bundle.resource.resource_id = "resource-1234";
  bundle.resource.project_id = "project-1234";
  bundle.resource.type = "image";
  bundle.resource.label = "Boiler фото";
  bundle.resource.metadata = {
      {"creator", {"Zeth", "Alex"}},
      {"custom:room", {"Kitchen"}},
      {"description", {"Heating controls"}},
  };
  bundle.resource.created_at = 10;
  bundle.resource.updated_at = 11;

  holder::model::Asset asset;
  asset.asset_id = "asset-1234";
  asset.resource_id = bundle.resource.resource_id;
  asset.original_filename = "boiler.jpg";
  asset.media_type = "image/jpeg";
  asset.byte_size = 123;
  asset.plaintext_sha256 = std::string(64, 'a');
  asset.created_at = 10;
  asset.updated_at = 11;

  holder::model::Placement placement;
  placement.placement_id = "placement-1234";
  placement.asset_id = asset.asset_id;
  placement.location_id = "location-1234";
  placement.object_key = "project-1234/asset-1234.holderasset";
  placement.encoding = "holder_asset_v1";
  placement.stored_byte_size = 180;
  placement.stored_sha256 = std::string(64, 'b');
  placement.created_at = 10;
  asset.placements.push_back(placement);
  bundle.assets.push_back(asset);
  return bundle;
}

holder::model::Location sample_location() {
  holder::model::Location location;
  location.location_id = "location-1234";
  location.project_id = "project-1234";
  location.name = "Family Assets";
  location.provider = "s3_compatible";
  location.configuration = {
      {"bucket", "holder-family"},
      {"endpoint", "https://objects.example"},
      {"prefix", "assets"},
      {"region", "eu-west-2"},
  };
  location.created_at = 10;
  location.updated_at = 11;
  return location;
}

int always_interrupt(void*) {
  return 1;
}

struct InterruptAfter {
  int remaining;
};

int interrupt_after(void* data) {
  auto* state = static_cast<InterruptAfter*>(data);
  return --state->remaining <= 0 ? 1 : 0;
}

template <typename Fn>
bool observes_sqlite_failure(
    holder::platform::Db& db,
    const std::string& expected,
    Fn&& operation
) {
  for (int threshold = 1; threshold <= 1000; ++threshold) {
    InterruptAfter state{threshold};
    sqlite3_progress_handler(db.handle(), 1, interrupt_after, &state);
    try {
      operation();
    } catch (const std::exception& error) {
      sqlite3_progress_handler(db.handle(), 0, nullptr, nullptr);
      if (std::string(error.what()).rfind(expected, 0) == 0) return true;
      continue;
    }
    sqlite3_progress_handler(db.handle(), 0, nullptr, nullptr);
  }
  return false;
}

} // namespace

TEST_CASE("Card resource join isolates attachments and paginates live cards", "[resource][attachments]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  create_project(db, "project-1234");
  create_project(db, "other-project");
  holder::model::Card card;
  card.card_id = "card-1234";
  card.project_id = "project-1234";
  card.title = "Card";
  card.rel_path = "cards/card.md";
  holder::card::CardRepo(db).create(card);
  holder::resource::ResourceRepo resources(db);
  auto resource = sample_bundle().resource;
  resources.add(resource);
  auto second = resource;
  second.resource_id = "resource-2";
  resources.add(second);
  auto foreign = resource;
  foreign.resource_id = "foreign-resource";
  foreign.project_id = "other-project";
  resources.add(foreign);
  holder::model::CardLink link;
  link.project_id = card.project_id;
  link.from_card_id = card.card_id;
  link.to_card_id = resource.resource_id;
  link.to_type = "resource";
  link.kind = "attachment";
  holder::card::LinkRepo links(db);
  links.upsert_links(card.project_id, card.card_id, {link, link});
  REQUIRE(resources.list_for_card(card.project_id, card.card_id).size() == 1);
  REQUIRE(resources.list_for_card(card.project_id, card.card_id)[0].metadata == resource.metadata);
  link.to_card_id = second.resource_id;
  link.kind = "ref";
  links.upsert_links(card.project_id, card.card_id, {link});
  REQUIRE(resources.list_for_card(card.project_id, card.card_id).size() == 1);
  link.kind = "attachment";
  links.upsert_links(card.project_id, card.card_id, {link});
  link.to_card_id = foreign.resource_id;
  links.upsert_links(card.project_id, card.card_id, {link});
  REQUIRE(resources.list_for_card(card.project_id, card.card_id).size() == 2);
  REQUIRE(resources.list_for_card("other-project", card.card_id).empty());
  const auto first = resources.list_for_card(card.project_id, card.card_id, 1, 0);
  const auto next = resources.list_for_card(card.project_id, card.card_id, 1, 1);
  REQUIRE(first.size() == 1);
  REQUIRE(next.size() == 1);
  REQUIRE(first[0].resource_id != next[0].resource_id);
  REQUIRE(resources.list_for_card(card.project_id, card.card_id, 1, 2).empty());
  REQUIRE_THROWS_AS(resources.list_for_card(card.project_id, card.card_id, 0), std::invalid_argument);
  REQUIRE_THROWS_AS(resources.list_for_card(card.project_id, card.card_id, 1001), std::invalid_argument);
  REQUIRE_THROWS_AS(resources.list_for_card(card.project_id, card.card_id, 1, -1), std::invalid_argument);
  links.delete_link(card.project_id, card.card_id, resource.resource_id, "resource", "attachment");
  REQUIRE(resources.get(resource.resource_id).has_value());
  REQUIRE(resources.list_for_card(card.project_id, card.card_id).size() == 1);
  db.exec("UPDATE cards SET deleted_at = 1 WHERE card_id = 'card-1234';");
  REQUIRE(resources.list_for_card(card.project_id, card.card_id).empty());
}

TEST_CASE("Resource manifests round-trip canonical complete bundles", "[resource]") {
  const auto bundle = sample_bundle();
  const auto rendered = holder::resource::render_resource_manifest(bundle);
  REQUIRE(rendered.back() == '\n');
  const auto parsed = holder::resource::parse_resource_manifest(rendered);
  REQUIRE(parsed.resource.label == bundle.resource.label);
  REQUIRE(parsed.resource.metadata == bundle.resource.metadata);
  REQUIRE(parsed.assets.size() == 1);
  REQUIRE(parsed.assets[0].placements.size() == 1);
  REQUIRE(holder::resource::render_resource_manifest(parsed) == rendered);
}

TEST_CASE("Location manifests round-trip safe configuration", "[resource]") {
  const auto location = sample_location();
  const auto rendered = holder::resource::render_location_manifest(location);
  const auto parsed = holder::resource::parse_location_manifest(rendered);
  REQUIRE(parsed.configuration == location.configuration);
  REQUIRE(holder::resource::render_location_manifest(parsed) == rendered);
}

TEST_CASE("Resource manifests reject malformed durable fields", "[resource]") {
  const auto valid_bundle = sample_bundle();

  auto invalid_bundle = valid_bundle;
  invalid_bundle.resource.resource_id.clear();
  REQUIRE_THROWS(holder::resource::render_resource_manifest(invalid_bundle));

  invalid_bundle = valid_bundle;
  invalid_bundle.assets[0].resource_id = "other-resource";
  REQUIRE_THROWS(holder::resource::render_resource_manifest(invalid_bundle));

  invalid_bundle = valid_bundle;
  invalid_bundle.assets[0].placements[0].asset_id = "other-asset";
  REQUIRE_THROWS(holder::resource::render_resource_manifest(invalid_bundle));

  auto body = nlohmann::json::parse(holder::resource::render_resource_manifest(valid_bundle));
  body["format_version"] = 2;
  REQUIRE_THROWS(holder::resource::parse_resource_manifest(body.dump()));

  body = nlohmann::json::parse(holder::resource::render_resource_manifest(valid_bundle));
  body["assets"] = "not an array";
  REQUIRE_THROWS(holder::resource::parse_resource_manifest(body.dump()));

  body = nlohmann::json::parse(holder::resource::render_resource_manifest(valid_bundle));
  body["assets"][0]["byte_size"] = -1;
  REQUIRE_THROWS(holder::resource::parse_resource_manifest(body.dump()));

  body = nlohmann::json::parse(holder::resource::render_resource_manifest(valid_bundle));
  body["assets"][0]["placements"] = "not an array";
  REQUIRE_THROWS(holder::resource::parse_resource_manifest(body.dump()));

  body = nlohmann::json::parse(holder::resource::render_resource_manifest(valid_bundle));
  body["assets"][0]["placements"][0]["stored_byte_size"] = -1;
  REQUIRE_THROWS(holder::resource::parse_resource_manifest(body.dump()));

  auto location_body = nlohmann::json::parse(
      holder::resource::render_location_manifest(sample_location())
  );
  location_body["format_version"] = 2;
  REQUIRE_THROWS(holder::resource::parse_location_manifest(location_body.dump()));
}

TEST_CASE("Dublin Core mapping keeps Holder friendly and unknown terms lossless", "[resource]") {
  REQUIRE(
      holder::resource::dublin_core_term_for("description") ==
      "http://purl.org/dc/terms/description"
  );
  REQUIRE(holder::resource::holder_property_for_dublin_core("dcterms:creator") == "creator");
  REQUIRE(holder::resource::holder_property_for_dublin_core("schema:recipeCuisine") == "schema:recipeCuisine");
  REQUIRE_FALSE(holder::resource::dublin_core_term_for("schema:recipeCuisine").has_value());
}

TEST_CASE("Resource paths are sharded and reject short identifiers", "[resource]") {
  REQUIRE(
      holder::resource::resource_rel_path("abcdef") ==
      std::filesystem::path("resources/ab/cd/abcdef.json")
  );
  REQUIRE(
      holder::resource::location_rel_path("123456") ==
      std::filesystem::path("locations/12/34/123456.json")
  );
  REQUIRE_THROWS(holder::resource::resource_rel_path("abc"));
}

TEST_CASE("Resource and Location repositories preserve complete projection", "[resource]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  create_project(db, "project-1234");

  holder::resource::LocationRepo locations(db);
  locations.put(sample_location());

  holder::resource::ResourceRepo resources(db);
  resources.put_bundle(sample_bundle());

  const auto fetched = resources.get_bundle("resource-1234");
  REQUIRE(fetched.has_value());
  REQUIRE(fetched->resource.metadata.at("creator").size() == 2);
  REQUIRE(fetched->assets[0].placements[0].location_id == "location-1234");
  REQUIRE(resources.find_by_asset_hash("project-1234", std::string(64, 'a')).has_value());
  REQUIRE_FALSE(resources.find_by_asset_hash("other-project", std::string(64, 'a')).has_value());

  auto updated_resource = fetched->resource;
  updated_resource.label = "Renamed boiler";
  updated_resource.metadata["description"] = {"Recently serviced"};
  updated_resource.updated_at = 20;
  resources.update(updated_resource);
  const auto updated_bundle = resources.get_bundle("resource-1234");
  REQUIRE(updated_bundle->resource.label == "Renamed boiler");
  REQUIRE(updated_bundle->resource.metadata.at("description") == std::vector<std::string>{"Recently serviced"});
  REQUIRE(updated_bundle->assets.size() == 1);
  REQUIRE_THROWS(resources.update(holder::model::Resource{}));

  REQUIRE(locations.list("project-1234").size() == 1);
  REQUIRE(locations.is_in_use("location-1234"));
  auto location = locations.get("location-1234");
  REQUIRE(location.has_value());
  location->name = "Renamed";
  location->updated_at = 20;
  locations.put(*location);
  REQUIRE(locations.get("location-1234")->name == "Renamed");

  resources.remove_project("project-1234");
  REQUIRE_FALSE(resources.get("resource-1234").has_value());
  locations.remove_project("project-1234");
  REQUIRE_FALSE(locations.get("location-1234").has_value());
}

TEST_CASE("Resource repository validates ownership links", "[resource]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  create_project(db, "project-1234");
  holder::resource::ResourceRepo resources(db);

  auto invalid = sample_bundle();
  invalid.assets[0].resource_id = "another-resource";
  REQUIRE_THROWS(resources.put_bundle(invalid));

  invalid = sample_bundle();
  invalid.assets[0].placements[0].asset_id = "another-asset";
  REQUIRE_THROWS(resources.put_bundle(invalid));

  invalid = sample_bundle();
  invalid.resource.label.clear();
  REQUIRE_THROWS(resources.put_bundle(invalid));

  invalid = sample_bundle();
  invalid.resource.metadata[""] = {"invalid"};
  REQUIRE_THROWS(resources.put_bundle(invalid));

  holder::resource::LocationRepo(db).put(sample_location());
  auto invalid_location = sample_location();
  invalid_location.location_id.clear();
  REQUIRE_THROWS(holder::resource::LocationRepo(db).put(invalid_location));
  resources.put_bundle(sample_bundle());
  REQUIRE_THROWS(resources.add(sample_bundle().resource));
}

TEST_CASE("Resource and Location repositories surface sqlite prepare failures", "[resource]") {
  holder::platform::Db unopened;
  holder::resource::ResourceRepo resources(unopened);
  holder::resource::LocationRepo locations(unopened);

  REQUIRE_THROWS(resources.get("resource-1234"));
  REQUIRE_THROWS(resources.get_bundle("resource-1234"));
  REQUIRE_THROWS(resources.find_by_asset_hash("project-1234", "hash"));
  REQUIRE_THROWS(resources.put_bundle(sample_bundle()));
  REQUIRE_THROWS(resources.list("project-1234"));
  REQUIRE_THROWS(resources.remove("resource-1234"));
  REQUIRE_THROWS(resources.remove_project("project-1234"));

  REQUIRE_THROWS(locations.put(sample_location()));
  REQUIRE_THROWS(locations.get("location-1234"));
  REQUIRE_THROWS(locations.list("project-1234"));
  REQUIRE_THROWS(locations.is_in_use("location-1234"));
  REQUIRE_THROWS(locations.remove("location-1234"));
  REQUIRE_THROWS(locations.remove_project("project-1234"));
}

TEST_CASE("Resource and Location repositories surface interrupted sqlite steps", "[resource]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  create_project(db, "project-1234");
  holder::resource::LocationRepo locations(db);
  holder::resource::ResourceRepo resources(db);
  locations.put(sample_location());
  resources.put_bundle(sample_bundle());

  sqlite3_progress_handler(db.handle(), 1, always_interrupt, nullptr);
  REQUIRE_THROWS(locations.get("location-1234"));
  REQUIRE_THROWS(locations.list("project-1234"));
  REQUIRE_THROWS(locations.is_in_use("location-1234"));
  REQUIRE_THROWS(locations.put(sample_location()));
  REQUIRE_THROWS(locations.remove("location-1234"));
  REQUIRE_THROWS(locations.remove_project("project-1234"));
  REQUIRE_THROWS(resources.get("resource-1234"));
  REQUIRE_THROWS(resources.get_bundle("resource-1234"));
  REQUIRE_THROWS(resources.find_by_asset_hash("project-1234", std::string(64, 'a')));
  REQUIRE_THROWS(resources.list("project-1234"));
  REQUIRE_THROWS(resources.remove("resource-1234"));
  REQUIRE_THROWS(resources.remove_project("project-1234"));
  sqlite3_progress_handler(db.handle(), 0, nullptr, nullptr);
}

TEST_CASE("Resource and Location repositories report nested sqlite scan failures",
          "[resource]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  create_project(db, "project-1234");
  holder::resource::LocationRepo locations(db);
  holder::resource::ResourceRepo resources(db);
  locations.put(sample_location());
  resources.put_bundle(sample_bundle());

  CHECK(observes_sqlite_failure(db, "get resource metadata failed", [&] {
    (void)resources.get("resource-1234");
  }));
  CHECK(observes_sqlite_failure(db, "get assets failed", [&] {
    (void)resources.get_bundle("resource-1234");
  }));
  CHECK(observes_sqlite_failure(db, "get placements failed", [&] {
    (void)resources.get_bundle("resource-1234");
  }));
  CHECK(observes_sqlite_failure(db, "find asset hash failed", [&] {
    (void)resources.find_by_asset_hash("project-1234", std::string(64, 'a'));
  }));
  CHECK(observes_sqlite_failure(db, "list resources failed", [&] {
    (void)resources.list("project-1234");
  }));
  CHECK(observes_sqlite_failure(db, "get location failed", [&] {
    (void)locations.get("location-1234");
  }));
  CHECK(observes_sqlite_failure(db, "list locations failed", [&] {
    (void)locations.list("project-1234");
  }));
  CHECK(observes_sqlite_failure(db, "location use check failed", [&] {
    (void)locations.is_in_use("location-1234");
  }));

  db.exec("CREATE TRIGGER block_resource_remove BEFORE DELETE ON resources "
          "BEGIN SELECT RAISE(ABORT, 'blocked resource removal'); END;");
  REQUIRE_THROWS(resources.remove("resource-1234"));
}

TEST_CASE("Project rebuild reconstructs resources assets placements and locations", "[resource]") {
  const auto dir = make_temp_dir();
  const auto project_root = dir / "project";
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  holder::model::Project project;
  project.project_id = "project-1234";
  project.name = "Project";
  project.root_path = project_root.string();
  project.privacy_mode = "plain";
  project.created_at = 1;
  project.updated_at = 1;
  holder::project::ProjectRepo(db).create(project);

  const auto location = sample_location();
  const auto bundle = sample_bundle();
  const auto location_path = project_root / holder::resource::location_rel_path(location.location_id);
  const auto resource_path = project_root / holder::resource::resource_rel_path(bundle.resource.resource_id);
  std::filesystem::create_directories(location_path.parent_path());
  std::filesystem::create_directories(resource_path.parent_path());
  std::ofstream(location_path) << holder::resource::render_location_manifest(location);
  std::ofstream(resource_path) << holder::resource::render_resource_manifest(bundle);

  const auto stats = holder::store::Rebuilder(db, nullptr).rebuild_project(project);
  REQUIRE(stats.locations == 1);
  REQUIRE(stats.resources == 1);
  REQUIRE(stats.assets == 1);
  REQUIRE(stats.placements == 1);
  REQUIRE(holder::resource::ResourceRepo(db).get_bundle("resource-1234").has_value());
  REQUIRE(holder::resource::LocationRepo(db).get("location-1234").has_value());

  holder::resource::ResourceRepo(db).remove_project(project.project_id);
  holder::resource::LocationRepo(db).remove_project(project.project_id);
  REQUIRE_FALSE(holder::resource::ResourceRepo(db).get("resource-1234").has_value());
  REQUIRE_NOTHROW(holder::store::Rebuilder(db, nullptr).rebuild_project(project));
  REQUIRE(holder::resource::ResourceRepo(db).get("resource-1234").has_value());
}

TEST_CASE(
    "Project rebuild rejects corrupt resource and location ownership data",
    "[resource]") {
  const auto dir = make_temp_dir();
  const auto project_root = dir / "project";
  std::filesystem::create_directories(project_root);
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  holder::model::Project project;
  project.project_id = "project-1234";
  project.name = "Project";
  project.root_path = project_root.string();
  project.created_at = 1;
  project.updated_at = 1;
  holder::project::ProjectRepo(db).create(project);

  const auto write_location = [&](const holder::model::Location &location) {
    write_text(project_root /
                   holder::resource::location_rel_path(location.location_id),
               holder::resource::render_location_manifest(location));
  };
  const auto write_resource = [&](const holder::model::ResourceBundle &bundle) {
    write_text(project_root / holder::resource::resource_rel_path(
                                  bundle.resource.resource_id),
               holder::resource::render_resource_manifest(bundle));
  };
  const auto rebuild = [&] {
    holder::store::Rebuilder(db, nullptr).rebuild_project(project);
  };

  SECTION("malformed location manifest") {
    write_text(project_root /
                   holder::resource::location_rel_path("location-1234"),
               "{not-json");
    REQUIRE_THROWS_WITH(
        rebuild(), Catch::Matchers::ContainsSubstring("location-1234.json"));
  }

  SECTION("location belongs to another project") {
    auto location = sample_location();
    location.project_id = "another-project";
    write_location(location);
    REQUIRE_THROWS_WITH(rebuild(),
                        Catch::Matchers::ContainsSubstring("another project"));
  }

  SECTION("malformed resource manifest") {
    write_text(project_root /
                   holder::resource::resource_rel_path("resource-1234"),
               "{not-json");
    REQUIRE_THROWS_WITH(
        rebuild(), Catch::Matchers::ContainsSubstring("resource-1234.json"));
  }

  SECTION("resource belongs to another project") {
    auto bundle = sample_bundle();
    bundle.resource.project_id = "another-project";
    write_resource(bundle);
    REQUIRE_THROWS_WITH(rebuild(),
                        Catch::Matchers::ContainsSubstring("another project"));
  }

  SECTION("duplicate asset id") {
    auto first = sample_bundle();
    auto second = sample_bundle();
    second.resource.resource_id = "resource-5678";
    second.assets[0].resource_id = second.resource.resource_id;
    second.assets[0].placements[0].asset_id = second.assets[0].asset_id;
    write_resource(first);
    write_resource(second);
    REQUIRE_THROWS_WITH(
        rebuild(), Catch::Matchers::ContainsSubstring("duplicate asset_id"));
  }

  SECTION("invalid plaintext digest") {
    auto bundle = sample_bundle();
    bundle.assets[0].plaintext_sha256 = "not-a-sha256";
    write_resource(bundle);
    REQUIRE_THROWS_WITH(rebuild(), Catch::Matchers::ContainsSubstring(
                                       "invalid plaintext_sha256"));
  }

  SECTION("duplicate placement id") {
    auto bundle = sample_bundle();
    auto second_asset = bundle.assets[0];
    second_asset.asset_id = "asset-5678";
    second_asset.resource_id = bundle.resource.resource_id;
    second_asset.placements[0].asset_id = second_asset.asset_id;
    bundle.assets.push_back(second_asset);
    write_resource(bundle);
    REQUIRE_THROWS_WITH(rebuild(), Catch::Matchers::ContainsSubstring(
                                       "duplicate placement_id"));
  }

  SECTION("invalid stored digest") {
    auto bundle = sample_bundle();
    bundle.assets[0].placements[0].stored_sha256 = "not-a-sha256";
    write_resource(bundle);
    REQUIRE_THROWS_WITH(
        rebuild(), Catch::Matchers::ContainsSubstring("invalid stored_sha256"));
  }

  SECTION("placement refers to unknown location") {
    write_resource(sample_bundle());
    REQUIRE_THROWS_WITH(rebuild(),
                        Catch::Matchers::ContainsSubstring("unknown location"));
  }
}

TEST_CASE("Encrypted Resource and Location manifests rebuild after projection "
          "deletion",
          "[resource][privacy]") {
  const auto dir = make_temp_dir();
  const auto project_root = dir / "project";
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  holder::model::Project project;
  project.project_id = "project-1234";
  project.name = "Encrypted project";
  project.root_path = project_root.string();
  project.privacy_mode = "encrypted_git";
  project.created_at = 1;
  project.updated_at = 1;
  holder::project::ProjectRepo projects(db);
  projects.create(project);

  holder::git::RealGitOps git;
  holder::privacy::ensure_encrypted_project_ready(
      git,
      projects,
      project.project_id,
      project.root_path,
      std::nullopt,
      2,
      [] { return std::string("resource-rebuild-key"); }
  );
  project = *projects.get(project.project_id);

  holder::resource::LocationStore(db, nullptr, &git).put(sample_location());
  holder::resource::ResourceStore(db, nullptr, &git).put(sample_bundle());
  REQUIRE(holder::resource::LocationStore(db, nullptr, &git).get("location-1234").has_value());
  REQUIRE_FALSE(holder::resource::LocationStore(db, nullptr, &git).get("missing-location").has_value());
  REQUIRE(holder::resource::ResourceStore(db, nullptr, &git).get("resource-1234").has_value());
  REQUIRE_FALSE(holder::resource::ResourceStore(db, nullptr, &git).get("missing-resource").has_value());

  const auto location_path = project_root /
                             holder::resource::location_rel_path("location-1234");
  const auto resource_path = project_root /
                             holder::resource::resource_rel_path("resource-1234");
  std::ifstream location_file(location_path, std::ios::binary);
  std::ifstream resource_file(resource_path, std::ios::binary);
  const std::string location_raw{
      std::istreambuf_iterator<char>(location_file), std::istreambuf_iterator<char>()
  };
  const std::string resource_raw{
      std::istreambuf_iterator<char>(resource_file), std::istreambuf_iterator<char>()
  };
  REQUIRE(location_raw.rfind("HolderPriv1\n", 0) == 0);
  REQUIRE(resource_raw.rfind("HolderPriv1\n", 0) == 0);
  REQUIRE(resource_raw.find("Boiler") == std::string::npos);

  holder::resource::ResourceRepo(db).remove_project(project.project_id);
  holder::resource::LocationRepo(db).remove_project(project.project_id);
  REQUIRE_FALSE(holder::resource::ResourceRepo(db).get("resource-1234").has_value());
  REQUIRE_FALSE(holder::resource::LocationRepo(db).get("location-1234").has_value());

  const auto rebuilt = holder::store::Rebuilder(db, nullptr).rebuild_project(project);
  REQUIRE(rebuilt.resources == 1);
  REQUIRE(rebuilt.assets == 1);
  REQUIRE(rebuilt.placements == 1);
  REQUIRE(rebuilt.locations == 1);
  REQUIRE(holder::resource::ResourceRepo(db).get_bundle("resource-1234")->resource.label == "Boiler фото");
  REQUIRE(holder::resource::LocationRepo(db).get("location-1234")->name == "Family Assets");

  REQUIRE_THROWS(holder::resource::LocationStore(db, nullptr, &git)
                     .remove("location-1234"));
  holder::resource::ResourceStore(db, nullptr, &git).remove("resource-1234");
  REQUIRE_FALSE(holder::resource::ResourceRepo(db).get("resource-1234").has_value());
  holder::resource::LocationStore(db, nullptr, &git).remove("location-1234");
  REQUIRE_FALSE(holder::resource::LocationRepo(db).get("location-1234").has_value());
}

TEST_CASE("Encrypted resource stores reject projects without key identities",
          "[resource][privacy]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  holder::model::Project project;
  project.project_id = "project-1234";
  project.name = "Missing key";
  project.root_path = (dir / "project").string();
  project.privacy_mode = "encrypted_git";
  project.created_at = 1;
  project.updated_at = 1;
  holder::project::ProjectRepo(db).create(project);

  holder::git::RealGitOps git;
  REQUIRE_THROWS(holder::resource::LocationStore(db, nullptr, &git)
                     .put(sample_location()));
  REQUIRE_THROWS(
      holder::resource::ResourceStore(db, nullptr, &git).put(sample_bundle()));

  holder::resource::LocationRepo(db).put(sample_location());
  holder::resource::ResourceRepo(db).put_bundle(sample_bundle());
  holder::model::Card card;
  card.card_id = "card-1234";
  card.project_id = project.project_id;
  card.title = "Card";
  card.rel_path = holder::core::card_rel_path(card.card_id);
  card.created_at = 1;
  card.updated_at = 1;
  holder::card::CardRepo(db).create(card);
  holder::model::CardLink link;
  link.project_id = project.project_id;
  link.from_card_id = card.card_id;
  link.to_card_id = "resource-1234";
  link.to_type = "resource";
  link.kind = "attachment";
  link.created_at = 1;
  holder::card::LinkRepo(db).upsert_links(project.project_id, card.card_id,
                                           {link});
  const auto card_path = std::filesystem::path(project.root_path) / card.rel_path;
  write_text(card_path,
             holder::core::render_card_front_matter(card, {link}, {}) + "body");
  REQUIRE_THROWS(holder::resource::ResourceStore(db, nullptr, &git)
                     .remove("resource-1234"));
}

TEST_CASE("Resource and Location stores rebuild after projection writes fail",
          "[resource][rebuild]") {
  const auto dir = make_temp_dir();
  const auto project_root = dir / "project";
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  holder::model::Project project;
  project.project_id = "project-1234";
  project.name = "Project";
  project.root_path = project_root.string();
  project.privacy_mode = "plain";
  project.created_at = 1;
  project.updated_at = 1;
  holder::project::ProjectRepo(db).create(project);
  holder::git::RealGitOps git;

  SECTION("location put") {
    db.exec("CREATE TRIGGER block_location_put BEFORE INSERT ON storage_locations "
            "BEGIN SELECT RAISE(ABORT, 'blocked location put'); END;");
    REQUIRE_THROWS(holder::resource::LocationStore(db, nullptr, &git)
                       .put(sample_location()));
  }

  SECTION("location remove") {
    holder::resource::LocationStore store(db, nullptr, &git);
    store.put(sample_location());
    db.exec("CREATE TRIGGER block_location_remove BEFORE DELETE ON storage_locations "
            "BEGIN SELECT RAISE(ABORT, 'blocked location remove'); END;");
    REQUIRE_THROWS(store.remove("location-1234"));
  }

  SECTION("resource put") {
    holder::resource::LocationRepo(db).put(sample_location());
    db.exec("CREATE TRIGGER block_resource_put BEFORE INSERT ON resources "
            "BEGIN SELECT RAISE(ABORT, 'blocked resource put'); END;");
    REQUIRE_THROWS(holder::resource::ResourceStore(db, nullptr, &git)
                       .put(sample_bundle()));
  }

  SECTION("resource remove") {
    holder::resource::LocationStore(db, nullptr, &git).put(sample_location());
    holder::resource::ResourceStore store(db, nullptr, &git);
    store.put(sample_bundle());
    db.exec("CREATE TRIGGER block_resource_remove BEFORE DELETE ON resources "
            "BEGIN SELECT RAISE(ABORT, 'blocked resource remove'); END;");
    REQUIRE_THROWS(store.remove("resource-1234"));
  }
}
