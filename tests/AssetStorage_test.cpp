#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "model/Project.h"
#include "card/CardFrontMatter.h"
#include "card/CardPaths.h"
#include "card/CardRepo.h"
#include "card/LinkRepo.h"
#include "core_test_helpers.h"
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
#include "resource/ResourcePaths.h"
#include "resource/ResourceRepo.h"
#include "resource/ResourceStore.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <tuple>
#include <vector>

#include <sodium.h>

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

std::string read_binary(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  REQUIRE(input.is_open());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_binary(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  REQUIRE(output.is_open());
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::uint32_t read_be_u32(const std::string& bytes, std::size_t offset) {
  return (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes.at(offset))) << 24U) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes.at(offset + 1))) << 16U) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes.at(offset + 2))) << 8U) |
         static_cast<std::uint32_t>(static_cast<unsigned char>(bytes.at(offset + 3)));
}

void append_be_u32(std::string& bytes, std::uint32_t value) {
  bytes.push_back(static_cast<char>((value >> 24U) & 0xffU));
  bytes.push_back(static_cast<char>((value >> 16U) & 0xffU));
  bytes.push_back(static_cast<char>((value >> 8U) & 0xffU));
  bytes.push_back(static_cast<char>(value & 0xffU));
}

holder::model::Project encrypted_project(const std::filesystem::path& dir) {
  holder::platform::Db db;
  db.open(dir / "keys.db");
  db.exec(
      "CREATE TABLE projects (project_id TEXT PRIMARY KEY, name TEXT NOT NULL, root_path TEXT NOT "
      "NULL, git_remote_url TEXT NULL, git_provider TEXT NULL, privacy_mode TEXT NOT NULL DEFAULT "
      "'plain', project_key_id TEXT NULL, id_scheme TEXT NOT NULL DEFAULT 'uuid4', created_at "
      "INTEGER NOT NULL, updated_at INTEGER NOT NULL);"
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
    if (fail_writes) throw std::runtime_error("injected Git write failure");
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
  bool fail_writes = false;
};

class InvisibleAfterPutProvider final
    : public holder::resource::StorageProvider {
public:
  void put(const std::string &, const std::filesystem::path &, long long,
           const std::string &) override {
    put_called = true;
  }
  void get(const std::string &, const std::filesystem::path &) override {
    throw std::runtime_error("not stored");
  }
  bool exists(const std::string &) override { return available; }
  void remove(const std::string &) override {
    remove_called = true;
    throw std::runtime_error("cleanup unavailable");
  }

  bool put_called = false;
  bool remove_called = false;
  bool available = true;
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

  REQUIRE_THROWS_WITH(
      holder::resource::recover_asset_file(
          dir / "stored.bin",
          dir / "wrong.bin",
          project,
          "resource-1234",
          "different-asset",
          staged.encoding,
          staged.stored,
          staged.plaintext
      ),
      Catch::Matchers::ContainsSubstring("HolderAsset1 identity mismatch")
  );

  auto tampered = read_binary(dir / "stored.bin");
  REQUIRE_FALSE(tampered.empty());
  tampered.back() = static_cast<char>(static_cast<unsigned char>(tampered.back()) ^ 0x01U);
  write_binary(dir / "stored.bin", tampered);
  REQUIRE_THROWS_WITH(
      holder::resource::recover_asset_file(
          dir / "stored.bin",
          dir / "tampered.bin",
          project,
          "resource-1234",
          "asset-1234",
          staged.encoding,
          staged.stored,
          staged.plaintext
      ),
      Catch::Matchers::ContainsSubstring("stored asset integrity check failed")
  );
}

TEST_CASE("Asset envelopes reject malformed structure and invalid file targets", "[asset]") {
  const auto dir = temp_dir("malformed-envelope");
  const auto source = dir / "source.bin";
  write_pattern(source, 100);

  holder::model::Project plain;
  plain.project_id = "project-plain";
  plain.privacy_mode = "plain";
  REQUIRE_THROWS_WITH(
      holder::resource::digest_file(dir / "missing.bin"),
      Catch::Matchers::ContainsSubstring("failed to open asset file")
  );
  REQUIRE_THROWS_WITH(
      holder::resource::stage_asset_file(
          dir / "missing.bin", dir / "missing.staged", plain, "resource-1234", "asset-1234"
      ),
      Catch::Matchers::ContainsSubstring("failed to open asset source")
  );
  std::filesystem::create_directory(dir / "staging-is-directory");
  REQUIRE_THROWS_WITH(
      holder::resource::stage_asset_file(
          source, dir / "staging-is-directory", plain, "resource-1234", "asset-1234"
      ),
      Catch::Matchers::ContainsSubstring("failed to open asset staging file")
  );

  auto missing_key = plain;
  missing_key.privacy_mode = "encrypted_git";
  REQUIRE_THROWS_WITH(
      holder::resource::stage_asset_file(
          source, dir / "missing-key.staged", missing_key, "resource-1234", "asset-1234"
      ),
      Catch::Matchers::ContainsSubstring("encrypted project missing project_key_id")
  );

  const auto project = encrypted_project(dir);
  const auto stored_path = dir / "valid.stored";
  const auto staged = holder::resource::stage_asset_file(
      source, stored_path, project, "resource-1234", "asset-1234"
  );
  const auto valid = read_binary(stored_path);
  constexpr std::size_t magic_size = sizeof("HolderAsset1\n") - 1;
  const auto header_size = read_be_u32(valid, magic_size);
  const auto stream_header_offset = magic_size + 4 + header_size;
  const auto first_chunk_size_offset = stream_header_offset +
                                       crypto_secretstream_xchacha20poly1305_HEADERBYTES;

  auto expect_malformed = [&](const std::string& name, const std::string& bytes) {
    const auto malformed = dir / name;
    write_binary(malformed, bytes);
    const auto digest = holder::resource::digest_file(malformed);
    REQUIRE_THROWS_WITH(
        holder::resource::recover_asset_file(
            malformed, dir / (name + ".out"), project, "resource-1234", "asset-1234",
            "holder_asset_v1", digest, staged.plaintext
        ),
        Catch::Matchers::ContainsSubstring("HolderAsset1 authentication failed") ||
            Catch::Matchers::ContainsSubstring("invalid HolderAsset1 chunk size") ||
            Catch::Matchers::ContainsSubstring("invalid HolderAsset1 metadata size") ||
            Catch::Matchers::ContainsSubstring("trailing data after HolderAsset1 final chunk") ||
            Catch::Matchers::ContainsSubstring("truncated HolderAsset1 chunk") ||
            Catch::Matchers::ContainsSubstring("truncated HolderAsset1 length") ||
            Catch::Matchers::ContainsSubstring("truncated HolderAsset1 metadata") ||
            Catch::Matchers::ContainsSubstring("truncated HolderAsset1 stream header")
    );
  };

  expect_malformed("truncated-length", std::string("HolderAsset1\n\0\0", magic_size + 2));
  std::string zero_metadata("HolderAsset1\n", magic_size);
  append_be_u32(zero_metadata, 0);
  expect_malformed("zero-metadata", zero_metadata);
  std::string truncated_metadata("HolderAsset1\n", magic_size);
  append_be_u32(truncated_metadata, 10);
  truncated_metadata += "short";
  expect_malformed("truncated-metadata", truncated_metadata);
  expect_malformed("truncated-stream-header", valid.substr(0, stream_header_offset + 3));

  auto invalid_chunk_size = valid;
  invalid_chunk_size.replace(first_chunk_size_offset, 4, std::string(4, '\0'));
  expect_malformed("invalid-chunk-size", invalid_chunk_size);

  auto truncated_chunk = valid.substr(0, first_chunk_size_offset);
  append_be_u32(truncated_chunk, crypto_secretstream_xchacha20poly1305_ABYTES);
  truncated_chunk += "short";
  expect_malformed("truncated-chunk", truncated_chunk);

  auto unauthenticated = valid;
  unauthenticated.at(first_chunk_size_offset + 4) ^= 1;
  expect_malformed("authentication", unauthenticated);

  auto trailing = valid;
  trailing.push_back('x');
  expect_malformed("trailing-data", trailing);

  REQUIRE_THROWS_WITH(
      holder::resource::recover_asset_file(
          stored_path, dir / "unsupported.out", project, "resource-1234", "asset-1234",
          "future-encoding", staged.stored, staged.plaintext
      ),
      Catch::Matchers::ContainsSubstring("unsupported asset encoding: future-encoding")
  );
  auto wrong_plaintext = staged.plaintext;
  ++wrong_plaintext.byte_size;
  REQUIRE_THROWS_WITH(
      holder::resource::recover_asset_file(
          stored_path, dir / "wrong-plaintext.out", project, "resource-1234", "asset-1234",
          staged.encoding, staged.stored, wrong_plaintext
      ),
      Catch::Matchers::ContainsSubstring("plaintext asset integrity check failed")
  );
  std::filesystem::create_directory(dir / "recovered-is-directory");
  REQUIRE_THROWS_WITH(
      holder::resource::recover_asset_file(
          stored_path, dir / "recovered-is-directory", project, "resource-1234", "asset-1234",
          staged.encoding, staged.stored, staged.plaintext
      ),
      Catch::Matchers::ContainsSubstring("failed to open recovered asset")
  );
}

TEST_CASE("Local directory provider is atomic and idempotent", "[asset]") {
  const auto dir = temp_dir("local");
  const auto source = dir / "source.bin";
  write_pattern(source, 8192);
  const auto digest = holder::resource::digest_file(source);
  holder::resource::LocalDirectoryProvider provider(dir / "objects");

  REQUIRE_THROWS_WITH(provider.exists(""), Catch::Matchers::ContainsSubstring("invalid storage object key"));
  REQUIRE_THROWS_WITH(
      provider.exists("/absolute/path"),
      Catch::Matchers::ContainsSubstring("invalid storage object key")
  );
  REQUIRE_THROWS_WITH(
      provider.exists("./relative"),
      Catch::Matchers::ContainsSubstring("unsafe storage object key")
  );
  REQUIRE_THROWS_WITH(
      provider.put("project/bad-size", source, digest.byte_size + 1, digest.sha256),
      Catch::Matchers::ContainsSubstring("staged object integrity mismatch")
  );
  REQUIRE_THROWS_WITH(
      provider.get("project/missing", dir / "missing.bin"),
      Catch::Matchers::ContainsSubstring("storage object not found")
  );

  provider.put("project/asset.holderasset", source, digest.byte_size, digest.sha256);
  REQUIRE(provider.exists("project/asset.holderasset"));
  REQUIRE_NOTHROW(
      provider.put("project/asset.holderasset", source, digest.byte_size, digest.sha256)
  );

  const auto conflicting_source = dir / "conflicting.bin";
  write_pattern(conflicting_source, 8193);
  const auto conflicting_digest = holder::resource::digest_file(conflicting_source);
  REQUIRE_THROWS_WITH(
      provider.put(
          "project/asset.holderasset",
          conflicting_source,
          conflicting_digest.byte_size,
          conflicting_digest.sha256
      )
  , Catch::Matchers::ContainsSubstring("object key already contains different bytes"));
  provider.get("project/asset.holderasset", dir / "download.bin");
  REQUIRE(holder::resource::digest_file(dir / "download.bin").sha256 ==
          digest.sha256);
  const auto directory_destination = dir / "directory-destination";
  std::filesystem::create_directory(directory_destination);
  REQUIRE_THROWS_AS(
      provider.get("project/asset.holderasset", directory_destination), std::filesystem::filesystem_error
  );
  REQUIRE_THROWS_WITH(
      provider.exists("../escape"),
      Catch::Matchers::ContainsSubstring("unsafe storage object key")
  );
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
  REQUIRE(bindings.preview("project-1", "location-1") == "AKIA…TEST");
  REQUIRE_FALSE(bindings.preview("project-1", "missing-location").has_value());
  REQUIRE(bindings.preferred("project-1") == "location-1");

  secrets->set("org.holder.StorageLocation", "project-1:unsupported-location",
               R"({"version":2,"provider":"s3_compatible","values":{}})",
               "unsupported", 10, 10);
  REQUIRE_THROWS_WITH(
      bindings.get("project-1", "unsupported-location"),
      Catch::Matchers::ContainsSubstring("unsupported location binding")
  );

  auto reopened = holder::privacy::make_encrypted_file_secret_store_for_tests(
      dir / "server");
  holder::resource::LocationBindingStore recovered(*reopened);
  REQUIRE(recovered.get("project-1", "location-1").has_value());
  REQUIRE(recovered.preferred("project-1") == "location-1");
  recovered.unbind("project-1", "location-1");
  recovered.clear_preferred("project-1");
  REQUIRE_FALSE(recovered.get("project-1", "location-1").has_value());
  REQUIRE_FALSE(recovered.preview("project-1", "location-1").has_value());
  REQUIRE_FALSE(recovered.preferred("project-1").has_value());

  holder::resource::LocationBinding invalid;
  REQUIRE_THROWS_WITH(
      bindings.bind("project-1", "location-1", invalid, "", 20),
      Catch::Matchers::ContainsSubstring("invalid location binding")
  );
  REQUIRE_THROWS_WITH(
      bindings.bind("", "location-1", binding, "", 20),
      Catch::Matchers::ContainsSubstring("binding ids required")
  );
  REQUIRE_THROWS_WITH(
      bindings.set_preferred("project-1", "", 20),
      Catch::Matchers::ContainsSubstring("preferred ids required")
  );
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
  const std::string card_body = "Check the boiler.\n";
  std::ofstream card_file(project_root / card.rel_path, std::ios::binary);
  card_file << holder::core::render_card_front_matter(card, {}, {}) << card_body;
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
  REQUIRE(first.link_created);
  REQUIRE(git.commits.size() == 1);
  const auto bundle = holder::resource::ResourceRepo(db).get_bundle(first.resource_id);
  REQUIRE(bundle.has_value());
  REQUIRE(bundle->assets[0].byte_size == 70000);
  REQUIRE(holder::resource::ResourceStore(db, nullptr, &git).get(first.resource_id).has_value());
  REQUIRE_FALSE(holder::resource::ResourceStore(db, nullptr, &git).get("missing-resource").has_value());
  REQUIRE(holder::card::LinkRepo(db).list_outgoing(project.project_id, card.card_id).size() == 1);

  const auto second = importer.import_file(request, provider);
  REQUIRE(second.duplicate_reused);
  REQUIRE_FALSE(second.link_created);
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

  write_binary(dir / "objects" / placement.object_key,
               "corrupt provider object");
  const auto failed_destination = dir / "failed-retrieval.jpg";
  REQUIRE_THROWS_WITH(
      importer.retrieve(first.resource_id, first.asset_id,
                                       placement.placement_id, provider,
                                       failed_destination),
      Catch::Matchers::ContainsSubstring("stored asset integrity check failed")
  );
  REQUIRE_FALSE(std::filesystem::exists(failed_destination));
  REQUIRE_FALSE(std::filesystem::exists(
      dir / "staging" / (placement.placement_id + ".download")));

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
  REQUIRE(rewritten.body == card_body);
  REQUIRE(git.commits.size() == 2);

  const std::vector<std::tuple<std::string, std::string, std::string>> formats = {
      {"picture.png", "image", "image/png"},
      {"animation.gif", "image", "image/gif"},
      {"picture.webp", "image", "image/webp"},
      {"drawing.svg", "image", "image/svg+xml"},
      {"manual.pdf", "document", "application/pdf"},
      {"notes.TXT", "document", "text/plain"},
      {"archive.bin", "thing", "application/octet-stream"},
  };
  std::size_t format_size = 101;
  for (const auto& [filename, resource_type, media_type] : formats) {
    request.source_file = dir / filename;
    write_pattern(request.source_file, format_size++);
    const auto imported = importer.import_file(request, provider);
    const auto imported_bundle = holder::resource::ResourceRepo(db).get_bundle(imported.resource_id);
    REQUIRE(imported_bundle.has_value());
    REQUIRE(imported_bundle->resource.type == resource_type);
    REQUIRE(imported_bundle->assets.front().media_type == media_type);
  }

  auto invalid_request = request;
  invalid_request.source_file = dir / "missing.bin";
  REQUIRE_THROWS_WITH(
      importer.import_file(invalid_request, provider),
      Catch::Matchers::ContainsSubstring("asset source must be a readable regular file")
  );

  invalid_request = request;
  invalid_request.project_id = "missing-project";
  REQUIRE_THROWS_WITH(
      importer.import_file(invalid_request, provider),
      Catch::Matchers::ContainsSubstring("project not found: missing-project")
  );

  invalid_request = request;
  invalid_request.card_id = "missing-card";
  REQUIRE_THROWS_WITH(
      importer.import_file(invalid_request, provider),
      Catch::Matchers::ContainsSubstring("live card not found in project")
  );

  invalid_request = request;
  invalid_request.location_id = "missing-location";
  REQUIRE_THROWS_WITH(
      importer.import_file(invalid_request, provider),
      Catch::Matchers::ContainsSubstring("storage location not found in project")
  );

  request.source_file = dir / "invisible-object.bin";
  write_pattern(request.source_file, 333);
  InvisibleAfterPutProvider unavailable;
  unavailable.available = false;
  REQUIRE_THROWS_WITH(
      importer.import_file(request, unavailable),
      Catch::Matchers::ContainsSubstring("stored object did not become available")
  );
  REQUIRE(unavailable.put_called);
  REQUIRE_FALSE(unavailable.remove_called);

  InvisibleAfterPutProvider invisible;
  git.fail_writes = true;
  REQUIRE_THROWS_WITH(
      importer.import_file(request, invisible),
      Catch::Matchers::ContainsSubstring("injected Git write failure")
  );
  git.fail_writes = false;
  REQUIRE(invisible.put_called);
  REQUIRE(invisible.remove_called);

  db.exec("UPDATE cards SET rel_path = 'cards/wrong.md' WHERE card_id = "
          "'card-1234';");
  REQUIRE_THROWS_WITH(
      importer.import_file(request, provider),
      Catch::Matchers::ContainsSubstring("card rel_path does not match card_id")
  );

  db.exec("UPDATE cards SET rel_path = '" + card.rel_path +
          "' WHERE card_id = 'card-1234';");
  db.exec("CREATE TRIGGER block_import_projection BEFORE INSERT ON resources "
          "BEGIN SELECT RAISE(ABORT, 'blocked import projection'); END;");
  request.source_file = dir / "post-commit-projection-failure.bin";
  write_pattern(request.source_file, 337);
  REQUIRE_THROWS_WITH(
      importer.import_file(request, provider),
      Catch::Matchers::ContainsSubstring("placement refers to unknown location location-")
  );
}

TEST_CASE("Asset import encrypts durable manifests and card updates",
          "[asset][privacy]") {
  const auto dir = temp_dir("encrypted-import");
  const auto project_root = dir / "project";
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR",
                                      (dir / "keystore").string());
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  holder::model::Project project;
  project.project_id = "project-1234";
  project.name = "Encrypted project";
  project.root_path = project_root.string();
  project.privacy_mode = "plain";
  project.created_at = 1;
  project.updated_at = 1;
  holder::project::ProjectRepo projects(db);
  projects.create(project);
  project.project_key_id = holder::privacy::ensure_project_key_material(
      projects, project.project_id, std::nullopt, 2,
      [] { return "encrypted-import-key"; });
  project.privacy_mode = "encrypted_git";
  projects.update_privacy_mode(project.project_id, project.privacy_mode, 2);

  holder::model::Card card;
  card.card_id = "card-1234";
  card.project_id = project.project_id;
  card.title = "Encrypted card";
  card.rel_path = holder::core::card_rel_path(card.card_id);
  card.created_at = 1;
  card.updated_at = 1;
  holder::card::CardRepo(db).create(card);

  holder::git::RealGitOps git;
  git.open_or_init(project_root);
  git.write_file(card.rel_path,
                 holder::privacy::encrypt_project_blob(
                     project.project_id, *project.project_key_id,
                     holder::core::render_card_front_matter(card, {}, {}) +
                         "Secret body\n"));

  holder::model::Location location;
  location.location_id = "location-1234";
  location.project_id = project.project_id;
  location.name = "Encrypted assets";
  location.provider = "local_directory";
  location.created_at = 1;
  location.updated_at = 1;
  holder::resource::LocationRepo(db).put(location);

  const auto source = dir / "secret.pdf";
  write_pattern(source, 1'024);
  int sequence = 0;
  holder::resource::AssetImportService importer(
      db, dir / "staging",
      [&] { return "generated-" + std::to_string(++sequence); }, nullptr, &git);
  holder::resource::LocalDirectoryProvider provider(dir / "objects");
  holder::resource::AssetImportRequest request;
  request.project_id = project.project_id;
  request.card_id = card.card_id;
  request.location_id = location.location_id;
  request.source_file = source;
  request.now = 10;

  const auto imported = importer.import_file(request, provider);
  const auto resource_path =
      project_root / holder::resource::resource_rel_path(imported.resource_id);
  CHECK(read_binary(resource_path).rfind("HolderPriv1\n", 0) == 0);
  CHECK(read_binary(project_root / card.rel_path).rfind("HolderPriv1\n", 0) ==
        0);

  const auto bundle =
      holder::resource::ResourceRepo(db).get_bundle(imported.resource_id);
  REQUIRE(bundle.has_value());
  const auto &placement = bundle->assets[0].placements[0];
  const auto retrieved = dir / "retrieved.pdf";
  importer.retrieve(imported.resource_id, imported.asset_id,
                    placement.placement_id, provider, retrieved);
  CHECK(holder::resource::digest_file(retrieved).sha256 ==
        bundle->assets[0].plaintext_sha256);
}
