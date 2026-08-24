#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "model/Project.h"
#include "card/CardFrontMatter.h"
#include "card/CardPaths.h"
#include "card/CardRepo.h"
#include "card/LinkRepo.h"
#include "git/GitOps.h"
#include "platform/Db.h"
#include "privacy/ProjectPrivacy.h"
#include "privacy/SecretStore.h"
#include "project/ProjectRepo.h"
#include "resource/AssetEnvelope.h"
#include "resource/AssetImportService.h"
#include "resource/LocalDirectoryProvider.h"
#include "resource/LocationBindingStore.h"
#include "resource/LocationRepo.h"
#include "resource/ResourceRepo.h"
#include "resource/ResourceStore.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::filesystem::path temp_dir(const std::string& label) {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  auto path = std::filesystem::temp_directory_path() /
              ("holder_asset_" + label + "_" + std::to_string(nonce));
  std::filesystem::create_directories(path);
  return path;
}

void write_pattern(const std::filesystem::path& path, std::size_t size) {
  std::ofstream out(path, std::ios::binary);
  REQUIRE(out.is_open());
  for (std::size_t index = 0; index < size; ++index) {
    out.put(static_cast<char>((index * 37U) & 0xffU));
  }
}

holder::model::Project encrypted_project(const std::filesystem::path& dir) {
  holder::platform::Db db;
  db.open(dir / "keys.db");
  db.exec(
      "CREATE TABLE projects (project_id TEXT PRIMARY KEY, name TEXT NOT NULL, root_path TEXT NOT "
      "NULL, git_remote_url TEXT NULL, git_provider TEXT NULL, privacy_mode TEXT NOT NULL DEFAULT "
      "'plain', project_key_id TEXT NULL, created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL);"
  );
  holder::model::Project project;
  project.project_id = "project-asset-test";
  project.name = "Assets";
  project.root_path = dir.string();
  project.created_at = 1;
  project.updated_at = 1;
  holder::project::ProjectRepo repo(db);
  repo.create(project);
  const auto key_id = holder::privacy::ensure_project_key_material(
      repo, project.project_id, std::nullopt, 2, [] { return "asset-test-key-1234"; }
  );
  project.privacy_mode = "encrypted_git";
  project.project_key_id = key_id;
  return project;
}

std::filesystem::path schema_path() {
#ifdef SCHEMA_SQL_PATH
  return SCHEMA_SQL_PATH;
#else
  throw std::runtime_error("schema path unavailable");
#endif
}

void apply_schema(holder::platform::Db& db) {
  std::ifstream input(schema_path());
  REQUIRE(input.is_open());
  db.exec(std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()));
}

class FileGit final : public holder::git::GitOps {
 public:
  void open_or_init(const std::filesystem::path& root) override {
    root_ = root;
    std::filesystem::create_directories(root_);
  }
  void write_file(const std::filesystem::path& relative, const std::string& content) override {
    const auto path = root_ / relative;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
  }
  void stage_path(const std::filesystem::path& relative) override { staged.push_back(relative.string()); }
  void remove_path(const std::filesystem::path& relative) override {
    std::filesystem::remove(root_ / relative);
    staged.push_back(relative.string());
  }
  void commit(const std::string& message) override { commits.push_back(message); }
  void set_remote(const std::string&, const std::string&) override {}
  void remove_remote(const std::string&) override {}
  void pull_remote_ff_only(const std::string&) override {}
  holder::git::RemoteProbeResult probe_remote(const std::string&) override { return {}; }
  holder::git::PushResult push_branch(const std::string&, const std::string&, bool) override {
    return {};
  }
  std::filesystem::path repo_dir() const override { return root_; }

  std::filesystem::path root_;
  std::vector<std::string> staged;
  std::vector<std::string> commits;
};

} // namespace

TEST_CASE("Plain and encrypted assets stream round-trip", "[asset]") {
  for (const auto size : {std::size_t(0), std::size_t(7), std::size_t(150000)}) {
    const auto dir = temp_dir("roundtrip");
    const auto source = dir / "source.bin";
    write_pattern(source, size);

    holder::model::Project plain;
    plain.project_id = "project-plain";
    plain.privacy_mode = "plain";
    auto staged = holder::resource::stage_asset_file(
        source, dir / "plain.staged", plain, "resource-1234", "asset-1234"
    );
    REQUIRE(staged.encoding == "plain");
    holder::resource::recover_asset_file(
        dir / "plain.staged",
        dir / "plain.recovered",
        plain,
        "resource-1234",
        "asset-1234",
        staged.encoding,
        staged.stored,
        staged.plaintext
    );
    REQUIRE(holder::resource::digest_file(dir / "plain.recovered").sha256 == staged.plaintext.sha256);

    auto encrypted = encrypted_project(dir);
    staged = holder::resource::stage_asset_file(
        source, dir / "encrypted.staged", encrypted, "resource-1234", "asset-1234"
    );
    REQUIRE(staged.encoding == "holder_asset_v1");
    REQUIRE(staged.stored.byte_size > staged.plaintext.byte_size);
    holder::resource::recover_asset_file(
        dir / "encrypted.staged",
        dir / "encrypted.recovered",
        encrypted,
        "resource-1234",
        "asset-1234",
        staged.encoding,
        staged.stored,
        staged.plaintext
    );
    REQUIRE(
        holder::resource::digest_file(dir / "encrypted.recovered").sha256 ==
        staged.plaintext.sha256
    );
  }
}

TEST_CASE("Encrypted assets reject changed identity and bytes", "[asset]") {
  const auto dir = temp_dir("tamper");
  const auto source = dir / "source.bin";
  write_pattern(source, 100000);
  const auto project = encrypted_project(dir);
  const auto staged = holder::resource::stage_asset_file(
      source, dir / "stored.bin", project, "resource-1234", "asset-1234"
  );

  REQUIRE_THROWS(holder::resource::recover_asset_file(
      dir / "stored.bin",
      dir / "wrong.bin",
      project,
      "resource-1234",
      "different-asset",
      staged.encoding,
      staged.stored,
      staged.plaintext
  ));

  std::fstream file(dir / "stored.bin", std::ios::binary | std::ios::in | std::ios::out);
  REQUIRE(file.is_open());
  file.seekp(-1, std::ios::end);
  file.put('\0');
  file.close();
  REQUIRE_THROWS(holder::resource::recover_asset_file(
      dir / "stored.bin",
      dir / "tampered.bin",
      project,
      "resource-1234",
      "asset-1234",
      staged.encoding,
      staged.stored,
      staged.plaintext
  ));
}

TEST_CASE("Local directory provider is atomic and idempotent", "[asset]") {
  const auto dir = temp_dir("local");
  const auto source = dir / "source.bin";
  write_pattern(source, 8192);
  const auto digest = holder::resource::digest_file(source);
  holder::resource::LocalDirectoryProvider provider(dir / "objects");

  provider.put("project/asset.holderasset", source, digest.byte_size, digest.sha256);
  REQUIRE(provider.exists("project/asset.holderasset"));
  REQUIRE_NOTHROW(
      provider.put("project/asset.holderasset", source, digest.byte_size, digest.sha256)
  );
  provider.get("project/asset.holderasset", dir / "download.bin");
  REQUIRE(holder::resource::digest_file(dir / "download.bin").sha256 == digest.sha256);
  REQUIRE_THROWS(provider.exists("../escape"));
  provider.remove("project/asset.holderasset");
  REQUIRE_FALSE(provider.exists("project/asset.holderasset"));
}

TEST_CASE("Location bindings and preferences survive independently of SQLite", "[asset]") {
  const auto dir = temp_dir("bindings");
  auto secrets = holder::privacy::make_encrypted_file_secret_store_for_tests(dir / "server");
  holder::resource::LocationBindingStore bindings(*secrets);
  holder::resource::LocationBinding binding;
  binding.provider = "s3_compatible";
  binding.values = {{"access_key_id", "AKIA_TEST"}, {"secret_access_key", "never-log-this"}};
  bindings.bind("project-1", "location-1", binding, "AKIA…TEST", 10);
  bindings.set_preferred("project-1", "location-1", 10);

  REQUIRE(bindings.get("project-1", "location-1")->values.at("secret_access_key") == "never-log-this");
  REQUIRE(bindings.preferred("project-1") == "location-1");

  auto reopened = holder::privacy::make_encrypted_file_secret_store_for_tests(dir / "server");
  holder::resource::LocationBindingStore recovered(*reopened);
  REQUIRE(recovered.get("project-1", "location-1").has_value());
  REQUIRE(recovered.preferred("project-1") == "location-1");
  recovered.unbind("project-1", "location-1");
  recovered.clear_preferred("project-1");
  REQUIRE_FALSE(recovered.get("project-1", "location-1").has_value());
  REQUIRE_FALSE(recovered.preferred("project-1").has_value());
}

TEST_CASE("Asset import stores, links, deduplicates and retrieves", "[asset]") {
  const auto dir = temp_dir("import");
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

  holder::model::Card card;
  card.card_id = "card-1234";
  card.project_id = project.project_id;
  card.title = "Boiler";
  card.rel_path = holder::core::card_rel_path(card.card_id);
  card.sort_key = 1;
  card.created_at = 1;
  card.updated_at = 1;
  holder::card::CardRepo(db).create(card);
  std::filesystem::create_directories((project_root / card.rel_path).parent_path());
  std::ofstream card_file(project_root / card.rel_path);
  card_file << holder::core::render_card_front_matter(card, {}, {}) << "Check the boiler.\n";
  card_file.close();

  holder::model::Location location;
  location.location_id = "location-1234";
  location.project_id = project.project_id;
  location.name = "Local Assets";
  location.provider = "local_directory";
  location.configuration = {{"prefix", "family"}};
  location.created_at = 1;
  location.updated_at = 1;
  holder::resource::LocationRepo(db).put(location);

  const auto source = dir / "boiler.jpg";
  write_pattern(source, 70000);
  holder::resource::LocalDirectoryProvider provider(dir / "objects");
  FileGit git;
  git.open_or_init(project_root);
  int sequence = 0;
  holder::resource::AssetImportService importer(
      db,
      dir / "staging",
      [&] { return "generated-" + std::to_string(++sequence) + "-1234"; },
      nullptr,
      &git
  );
  holder::resource::AssetImportRequest request;
  request.project_id = project.project_id;
  request.card_id = card.card_id;
  request.location_id = location.location_id;
  request.source_file = source;
  request.now = 100;

  const auto first = importer.import_file(request, provider);
  REQUIRE_FALSE(first.duplicate_reused);
  REQUIRE(git.commits.size() == 1);
  const auto bundle = holder::resource::ResourceRepo(db).get_bundle(first.resource_id);
  REQUIRE(bundle.has_value());
  REQUIRE(bundle->assets[0].byte_size == 70000);
  REQUIRE(holder::card::LinkRepo(db).list_outgoing(project.project_id, card.card_id).size() == 1);

  const auto second = importer.import_file(request, provider);
  REQUIRE(second.duplicate_reused);
  REQUIRE(second.resource_id == first.resource_id);
  REQUIRE(second.asset_id == first.asset_id);
  REQUIRE(git.commits.size() == 1);

  const auto& placement = bundle->assets[0].placements[0];
  importer.retrieve(
      first.resource_id,
      first.asset_id,
      placement.placement_id,
      provider,
      dir / "retrieved.jpg"
  );
  REQUIRE(
      holder::resource::digest_file(dir / "retrieved.jpg").sha256 ==
      bundle->assets[0].plaintext_sha256
  );

  holder::resource::ResourceStore(db, nullptr, &git).remove(first.resource_id);
  REQUIRE_FALSE(holder::resource::ResourceRepo(db).get(first.resource_id).has_value());
  REQUIRE(holder::card::LinkRepo(db).list_outgoing(project.project_id, card.card_id).empty());
  std::ifstream rewritten_file(project_root / card.rel_path, std::ios::binary);
  REQUIRE(rewritten_file.is_open());
  const std::string rewritten_text{
      std::istreambuf_iterator<char>(rewritten_file), std::istreambuf_iterator<char>()
  };
  const auto rewritten = holder::core::parse_card_file(rewritten_text);
  REQUIRE(rewritten.links.empty());
  REQUIRE(rewritten.body == "Check the boiler.\n");
  REQUIRE(git.commits.size() == 2);
}
