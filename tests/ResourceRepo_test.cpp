#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "model/Location.h"
#include "model/Project.h"
#include "model/Resource.h"
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

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

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

} // namespace

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

TEST_CASE("Encrypted Resource and Location manifests rebuild after projection deletion", "[resource][privacy]") {
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
}
