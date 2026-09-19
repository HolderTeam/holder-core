#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "card/CardFrontMatter.h"
#include "card/CardPaths.h"
#include "card/CardRepo.h"
#include "card/CardStore.h"
#include "card/LinkRepo.h"
#include "card/MilestoneRepo.h"
#include "card/TagRepo.h"
#include "git/GitOps.h"
#include "git/GitRepo.h"
#include "core_test_helpers.h"
#include "index/FtsIndexer.h"
#include "model/Card.h"
#include "model/CardLink.h"
#include "model/Project.h"
#include "platform/Db.h"
#include "privacy/ProjectPrivacy.h"
#include "project/ProjectRepo.h"

#include <git2.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

std::filesystem::path find_schema_sql() {
#ifdef SCHEMA_SQL_PATH
  std::filesystem::path p = SCHEMA_SQL_PATH;
  if (std::filesystem::exists(p)) return p;
#endif
  namespace fs = std::filesystem;
  fs::path p1 = fs::current_path() / "schema" / "schema.sql";
  if (fs::exists(p1)) return p1;
  fs::path p2 = fs::current_path().parent_path() / "schema" / "schema.sql";
  if (fs::exists(p2)) return p2;
  throw std::runtime_error("schema.sql not found for tests");
}

std::filesystem::path make_temp_dir() {
  const auto base = std::filesystem::temp_directory_path();
  const auto suffix = std::to_string(
      static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())
  );
  auto dir = base / ("holder_card_store_test_" + suffix);
  std::filesystem::create_directories(dir);
  return dir;
}

void apply_schema(holder::platform::Db& db) {
  const auto schema_path = find_schema_sql();
  std::ifstream in(schema_path);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);
}

void create_project(
    holder::platform::Db& db,
    const std::string& project_id,
    const std::string& root_path,
    holder::model::IdScheme id_scheme = holder::model::IdScheme::Uuid4
) {
  holder::project::ProjectRepo repo(db);
  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Project";
  project.root_path = root_path;
  project.privacy_mode = "plain";
  project.id_scheme = id_scheme;
  project.project_key_id.reset();
  project.created_at = 1;
  project.updated_at = 1;
  repo.create(project);
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.is_open());
  std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return data;
}

holder::model::Milestone make_milestone_for(
    const std::string& milestone_id,
    const std::string& project_id,
    const std::string& card_id
) {
  holder::model::Milestone milestone;
  milestone.milestone_id = milestone_id;
  milestone.project_id = project_id;
  milestone.card_id = card_id;
  milestone.start_at = 100;
  milestone.created_at = 1;
  milestone.updated_at = 1;
  return milestone;
}

std::string make_large_body(std::size_t bytes) {
  const std::string chunk =
      "TOP_SECRET_MARKER_12345 lorem ipsum dolor sit amet, consectetur adipiscing elit.\n";
  std::string out;
  out.reserve(bytes);
  while (out.size() + chunk.size() <= bytes) {
    out += chunk;
  }
  if (out.size() < bytes) {
    out.append(bytes - out.size(), 'x');
  }
  return out;
}

int count_commits(const std::filesystem::path& repo_dir) {
  git_repository* repo = nullptr;
  if (git_repository_open(&repo, repo_dir.string().c_str()) != 0) {
    return -1;
  }

  git_revwalk* walk = nullptr;
  if (git_revwalk_new(&walk, repo) != 0) {
    git_repository_free(repo);
    return -1;
  }

  int count = 0;
  if (git_revwalk_push_head(walk) == 0) {
    git_oid oid{};
    while (git_revwalk_next(&oid, walk) == 0) {
      ++count;
    }
  }

  git_revwalk_free(walk);
  git_repository_free(repo);
  return count;
}

class PlaintextForcingGitOps final : public holder::git::GitOps {
 public:
  void open_or_init(const std::filesystem::path& repo_dir) override {
    real_.open_or_init(repo_dir);
  }
  void write_file(const std::filesystem::path& relative_path, const std::string&) override {
    // Intentionally wrong: stage plaintext regardless of requested content.
    real_.write_file(relative_path, "# forced-plaintext\n");
  }
  void stage_path(const std::filesystem::path& relative_path) override {
    real_.stage_path(relative_path);
  }
  void remove_path(const std::filesystem::path& relative_path) override {
    real_.remove_path(relative_path);
  }
  void commit(const std::string& message) override { real_.commit(message); }
  void set_remote(const std::string& name, const std::string& url) override {
    real_.set_remote(name, url);
  }
  void remove_remote(const std::string& name) override { real_.remove_remote(name); }
  void pull_remote_ff_only(const std::string& name) override { real_.pull_remote_ff_only(name); }
  holder::git::RemoteProbeResult probe_remote(const std::string& name) override {
    return real_.probe_remote(name);
  }
  holder::git::PushResult push_branch(
      const std::string& name,
      const std::string& branch,
      bool set_upstream
  ) override {
    return real_.push_branch(name, branch, set_upstream);
  }
  std::filesystem::path repo_dir() const override { return real_.repo_dir(); }

 private:
  holder::git::RealGitOps real_;
};

class CloseDbOnStageGitOps final : public holder::git::GitOps {
 public:
  explicit CloseDbOnStageGitOps(holder::platform::Db& db)
      : db_(db) {}

  void open_or_init(const std::filesystem::path& repo_dir) override {
    real_.open_or_init(repo_dir);
  }
  void write_file(const std::filesystem::path& relative_path, const std::string& content) override {
    real_.write_file(relative_path, content);
  }
  void stage_path(const std::filesystem::path& relative_path) override {
    real_.stage_path(relative_path);
    db_.close();
  }
  void remove_path(const std::filesystem::path& relative_path) override {
    real_.remove_path(relative_path);
  }
  void commit(const std::string& message) override { real_.commit(message); }
  void set_remote(const std::string& name, const std::string& url) override {
    real_.set_remote(name, url);
  }
  void remove_remote(const std::string& name) override { real_.remove_remote(name); }
  void pull_remote_ff_only(const std::string& name) override { real_.pull_remote_ff_only(name); }
  holder::git::RemoteProbeResult probe_remote(const std::string& name) override {
    return real_.probe_remote(name);
  }
  holder::git::PushResult push_branch(
      const std::string& name,
      const std::string& branch,
      bool set_upstream
  ) override {
    return real_.push_branch(name, branch, set_upstream);
  }
  std::filesystem::path repo_dir() const override { return real_.repo_dir(); }

 private:
  holder::platform::Db& db_;
  holder::git::RealGitOps real_;
};

class BulkTrackingGitOps final : public holder::git::GitOps {
 public:
  void open_or_init(const std::filesystem::path& repo_dir) override {
    real_.open_or_init(repo_dir);
  }
  void write_file(
      const std::filesystem::path& relative_path,
      const std::string& content
  ) override {
    real_.write_file(relative_path, content);
  }
  void stage_path(const std::filesystem::path& relative_path) override {
    ++stage_path_calls;
    real_.stage_path(relative_path);
  }
  void stage_paths(const std::vector<std::filesystem::path>& relative_paths) override {
    ++stage_paths_calls;
    staged_path_count += relative_paths.size();
    real_.stage_paths(relative_paths);
  }
  void remove_path(const std::filesystem::path& relative_path) override {
    real_.remove_path(relative_path);
  }
  void commit(const std::string& message) override {
    ++commit_calls;
    real_.commit(message);
  }
  void set_remote(const std::string& name, const std::string& url) override {
    real_.set_remote(name, url);
  }
  void remove_remote(const std::string& name) override { real_.remove_remote(name); }
  void pull_remote_ff_only(const std::string& name) override { real_.pull_remote_ff_only(name); }
  holder::git::RemoteProbeResult probe_remote(const std::string& name) override {
    return real_.probe_remote(name);
  }
  holder::git::PushResult push_branch(
      const std::string& name,
      const std::string& branch,
      bool set_upstream
  ) override {
    return real_.push_branch(name, branch, set_upstream);
  }
  std::filesystem::path repo_dir() const override { return real_.repo_dir(); }

  int stage_path_calls = 0;
  int stage_paths_calls = 0;
  std::size_t staged_path_count = 0;
  int commit_calls = 0;

 private:
  holder::git::RealGitOps real_;
};

} // namespace

TEST_CASE("CardStore create writes file and DB", "[cardstore]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);
  holder::model::Card card;
  card.card_id = "abcd1234";
  card.project_id = "proj-1";
  card.title = "First";
  card.created_at = 10;
  card.updated_at = 10;

  store.create(card, "hello");

  const auto rel_path = holder::core::card_rel_path(card.card_id);
  const auto full_path = project_root / rel_path;
  REQUIRE(std::filesystem::exists(full_path));
  const auto raw = read_file(full_path);
  REQUIRE(raw.find("---\n") == 0);
  REQUIRE(raw.find("card_id: abcd1234") != std::string::npos);
  REQUIRE(raw.find("project_id: proj-1") != std::string::npos);
  REQUIRE(raw.rfind("hello") != std::string::npos);
  const auto parsed = holder::core::parse_card_file(raw);
  REQUIRE(parsed.has_front_matter);
  REQUIRE(parsed.card.card_id == "abcd1234");
  REQUIRE(parsed.card.project_id == "proj-1");
  REQUIRE(parsed.card.title == "First");
  REQUIRE(parsed.body == "hello");

  holder::card::CardRepo card_repo(db);
  const auto fetched = card_repo.get("abcd1234");
  REQUIRE(fetched.has_value());
  REQUIRE(fetched->rel_path == rel_path);
}

TEST_CASE("CardStore create_batch stages and commits the batch once", "[cardstore][backup]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(
      db,
      "proj-1",
      project_root.string(),
      holder::model::IdScheme::Uuid7
  );

  BulkTrackingGitOps git;
  holder::card::CardStore store(db, nullptr, nullptr, &git);
  std::vector<holder::card::BatchCardInput> items;
  for (int i = 0; i < 3; ++i) {
    holder::card::BatchCardInput item;
    item.card_id = "old-card-" + std::to_string(i);
    item.title = "Card " + std::to_string(i);
    item.content = "body";
    item.created_at = i + 1;
    item.updated_at = i + 1;
    items.push_back(std::move(item));
  }
  holder::model::CardLink valid_link;
  valid_link.to_card_id = "old-card-1";
  valid_link.to_type = "card";
  valid_link.kind = "wiki";
  items[0].links.push_back(valid_link);
  auto non_card_link = valid_link;
  non_card_link.to_type = "resource";
  items[0].links.push_back(non_card_link);
  auto missing_link = valid_link;
  missing_link.to_card_id = "outside-snapshot";
  items[0].links.push_back(missing_link);
  store.create_batch(
      "proj-1",
      items,
      []() { return "new-milestone-id"; },
      "Restore batch"
  );

  CHECK(git.stage_path_calls == 0);
  CHECK(git.stage_paths_calls == 1);
  CHECK(git.staged_path_count == items.size());
  CHECK(git.commit_calls == 1);
  CHECK(count_commits(project_root) == 1);
  const auto cards = holder::card::CardRepo(db).list_all("proj-1");
  REQUIRE(cards.size() == items.size());
  const auto card_0 = std::find_if(cards.begin(), cards.end(), [](const auto& card) {
    return card.title == "Card 0";
  });
  const auto card_1 = std::find_if(cards.begin(), cards.end(), [](const auto& card) {
    return card.title == "Card 1";
  });
  REQUIRE(card_0 != cards.end());
  REQUIRE(card_1 != cards.end());
  REQUIRE(card_0->card_id.size() == 36);
  REQUIRE(card_1->card_id.size() == 36);
  CHECK(card_0->card_id[14] == '7');
  CHECK(card_1->card_id[14] == '7');
  const auto links = holder::card::LinkRepo(db).list_outgoing("proj-1", card_0->card_id);
  REQUIRE(links.size() == 1);
  CHECK(links[0].to_card_id == card_1->card_id);
}

TEST_CASE("CardStore update writes file and updates metadata", "[cardstore]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);
  holder::model::Card card;
  card.card_id = "abcd5678";
  card.project_id = "proj-1";
  card.title = "First";
  card.created_at = 10;
  card.updated_at = 10;

  store.create(card, "hello");
  store.update_content(card.card_id, "updated", std::optional<std::string>("Renamed"), 20);

  const auto rel_path = holder::core::card_rel_path(card.card_id);
  const auto full_path = project_root / rel_path;
  REQUIRE(std::filesystem::exists(full_path));
  const auto raw = read_file(full_path);
  REQUIRE(raw.find("---\n") == 0);
  REQUIRE(raw.find("title: Renamed") != std::string::npos);
  REQUIRE(raw.rfind("updated") != std::string::npos);
  const auto parsed = holder::core::parse_card_file(raw);
  REQUIRE(parsed.has_front_matter);
  REQUIRE(parsed.card.title == "Renamed");
  REQUIRE(parsed.card.updated_at == 20);
  REQUIRE(parsed.body == "updated");

  holder::card::CardRepo card_repo(db);
  const auto fetched = card_repo.get(card.card_id);
  REQUIRE(fetched.has_value());
  REQUIRE(fetched->title == "Renamed");
  REQUIRE(fetched->updated_at == 20);
}

TEST_CASE("CardStore update skips commit when content unchanged", "[cardstore]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);
  holder::model::Card card;
  card.card_id = "abcd9999";
  card.project_id = "proj-1";
  card.title = "First";
  card.created_at = 10;
  card.updated_at = 10;

  store.create(card, "same");
  const int before = count_commits(project_root);

  store.update_content(card.card_id, "same", std::nullopt, 20);
  const int after = count_commits(project_root);

  REQUIRE(before == after);

  const auto rel_path = holder::core::card_rel_path(card.card_id);
  const auto full_path = project_root / rel_path;
  const auto raw = read_file(full_path);
  const auto parsed = holder::core::parse_card_file(raw);
  REQUIRE(parsed.has_front_matter);
  REQUIRE(parsed.card.title == "First");
  REQUIRE(parsed.card.updated_at == 10);
  REQUIRE(parsed.body == "same");
}

TEST_CASE("CardStore update creates commit when content changes", "[cardstore]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);
  holder::model::Card card;
  card.card_id = "abcf0000";
  card.project_id = "proj-1";
  card.title = "First";
  card.created_at = 10;
  card.updated_at = 10;

  store.create(card, "one");
  const int before = count_commits(project_root);

  store.update_content(card.card_id, "two", std::nullopt, 20);
  const int after = count_commits(project_root);

  REQUIRE(after == before + 1);
}

TEST_CASE("CardStore encrypted project rejects staged plaintext blobs", "[cardstore]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  const auto project_root = dir / "project_repo";

  holder::project::ProjectRepo project_repo(db);
  holder::model::Project project;
  project.project_id = "proj-enc";
  project.name = "Encrypted";
  project.root_path = project_root.string();
  project.privacy_mode = "encrypted_git";
  project.project_key_id.reset();
  project.created_at = 1;
  project.updated_at = 1;
  project_repo.create(project);

  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  holder::git::RealGitOps bootstrap_git;
  holder::privacy::ensure_encrypted_project_ready(
      bootstrap_git,
      project_repo,
      project.project_id,
      project.root_path,
      std::nullopt,
      2,
      []() {
        return std::string("key-enc");
      }
  );

  holder::index::FtsIndexer fts(db);
  PlaintextForcingGitOps broken_git;
  holder::card::CardStore store(db, &fts, nullptr, &broken_git);

  holder::model::Card card;
  card.card_id = "abcd1111";
  card.project_id = "proj-enc";
  card.title = "Should fail";
  card.created_at = 10;
  card.updated_at = 10;

  REQUIRE_THROWS_AS(store.create(card, "secret"), holder::privacy::PrivacyError);
}

TEST_CASE("CardStore encrypted project round-trips 5MB content", "[cardstore]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  const auto project_root = dir / "project_repo";

  holder::project::ProjectRepo project_repo(db);
  holder::model::Project project;
  project.project_id = "proj-large";
  project.name = "Encrypted Large";
  project.root_path = project_root.string();
  project.privacy_mode = "encrypted_git";
  project.project_key_id.reset();
  project.created_at = 1;
  project.updated_at = 1;
  project_repo.create(project);

  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  holder::git::RealGitOps bootstrap_git;
  holder::privacy::ensure_encrypted_project_ready(
      bootstrap_git,
      project_repo,
      project.project_id,
      project.root_path,
      std::nullopt,
      2,
      []() {
        return std::string("key-large");
      }
  );

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);

  holder::model::Card card;
  card.card_id = "lgcd0001";
  card.project_id = "proj-large";
  card.title = "Large";
  card.created_at = 10;
  card.updated_at = 10;

  const auto body = make_large_body(std::size_t{5} * 1024 * 1024);
  store.create(card, body);

  const auto saved = store.get(card.card_id);
  REQUIRE(saved.has_value());
  const auto loaded = store.get_content(saved.value());
  REQUIRE(loaded.has_value());
  REQUIRE(loaded.value().size() == body.size());
  REQUIRE(loaded.value() == body);

  const auto full_path = project_root / holder::core::card_rel_path(card.card_id);
  const auto raw = read_file(full_path);
  REQUIRE(raw.rfind("HolderPriv1\n", 0) == 0);
  REQUIRE(raw.find("TOP_SECRET_MARKER_12345") == std::string::npos);
}

TEST_CASE("CardStore encrypted project rejects tampered envelope", "[cardstore]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  const auto project_root = dir / "project_repo";

  holder::project::ProjectRepo project_repo(db);
  holder::model::Project project;
  project.project_id = "proj-tamper";
  project.name = "Encrypted Tamper";
  project.root_path = project_root.string();
  project.privacy_mode = "encrypted_git";
  project.project_key_id.reset();
  project.created_at = 1;
  project.updated_at = 1;
  project_repo.create(project);

  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  holder::git::RealGitOps bootstrap_git;
  holder::privacy::ensure_encrypted_project_ready(
      bootstrap_git,
      project_repo,
      project.project_id,
      project.root_path,
      std::nullopt,
      2,
      []() {
        return std::string("key-tamper");
      }
  );

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);

  holder::model::Card card;
  card.card_id = "tmpr0001";
  card.project_id = "proj-tamper";
  card.title = "Tamper";
  card.created_at = 10;
  card.updated_at = 10;
  store.create(card, "original body");

  const auto full_path = project_root / holder::core::card_rel_path(card.card_id);
  auto raw = read_file(full_path);
  REQUIRE(raw.size() > 32);
  raw[raw.size() - 1] = (raw.back() == 'A') ? 'B' : 'A';
  {
    std::ofstream out(full_path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open());
    out << raw;
  }

  const auto saved = store.get(card.card_id);
  REQUIRE(saved.has_value());
  REQUIRE_THROWS_AS((void)store.get_content(saved.value()), holder::privacy::PrivacyError);
}

TEST_CASE("CardStore encrypted project perf profile (manual)", "[perf][.]") {
  struct PerfRow {
    std::size_t bytes = 0;
    double create_ms = 0.0;
    double update_ms = 0.0;
    double read_ms = 0.0;
  };

  const std::vector<std::size_t> sizes = {
      std::size_t{10} * 1024,
      std::size_t{100} * 1024,
      std::size_t{1024} * 1024,
      std::size_t{5} * 1024 * 1024,
  };
  std::vector<PerfRow> rows;
  rows.reserve(sizes.size());

  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  const auto project_root = dir / "project_repo";

  holder::project::ProjectRepo project_repo(db);
  holder::model::Project project;
  project.project_id = "proj-perf";
  project.name = "Encrypted Perf";
  project.root_path = project_root.string();
  project.privacy_mode = "encrypted_git";
  project.project_key_id.reset();
  project.created_at = 1;
  project.updated_at = 1;
  project_repo.create(project);

  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  holder::git::RealGitOps bootstrap_git;
  holder::privacy::ensure_encrypted_project_ready(
      bootstrap_git,
      project_repo,
      project.project_id,
      project.root_path,
      std::nullopt,
      2,
      []() {
        return std::string("key-perf");
      }
  );

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);

  for (std::size_t i = 0; i < sizes.size(); ++i) {
    const auto bytes = sizes[i];
    const auto body = make_large_body(bytes);
    auto body2 = body;
    body2.push_back('\n');
    body2 += "updated";

    holder::model::Card card;
    card.card_id = "pf" + std::to_string(1000000 + i);
    card.project_id = "proj-perf";
    card.title = "Perf";
    card.created_at = 10 + static_cast<long long>(i);
    card.updated_at = 10 + static_cast<long long>(i);

    const auto t0 = std::chrono::steady_clock::now();
    store.create(card, body);
    const auto t1 = std::chrono::steady_clock::now();
    store.update_content(card.card_id, body2, std::nullopt, 20 + static_cast<long long>(i));
    const auto t2 = std::chrono::steady_clock::now();
    const auto saved = store.get(card.card_id);
    REQUIRE(saved.has_value());
    const auto loaded = store.get_content(saved.value());
    const auto t3 = std::chrono::steady_clock::now();
    REQUIRE(loaded.has_value());
    REQUIRE(loaded.value() == body2);

    PerfRow row;
    row.bytes = bytes;
    row.create_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    row.update_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
    row.read_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();
    rows.push_back(row);
  }

  std::cout << "\nprivacy-card-perf\n";
  std::cout << std::left << std::setw(10) << "size_kb" << std::right << std::setw(12) << "create_ms"
            << std::setw(12) << "update_ms" << std::setw(10) << "read_ms" << "\n";
  for (const auto& row : rows) {
    std::cout << std::left << std::setw(10) << (row.bytes / 1024) << std::right << std::setw(12)
              << std::fixed << std::setprecision(2) << row.create_ms << std::setw(12)
              << row.update_ms << std::setw(10) << row.read_ms << "\n";
  }
}

TEST_CASE("CardStore move updates parent and sort metadata", "[cardstore]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);

  holder::model::Card parent;
  parent.card_id = "parent01";
  parent.project_id = "proj-1";
  parent.title = "Parent";
  parent.created_at = 10;
  parent.updated_at = 10;
  store.create(parent, "parent body");

  holder::model::Card child;
  child.card_id = "child001";
  child.project_id = "proj-1";
  child.title = "Child";
  child.created_at = 11;
  child.updated_at = 11;
  store.create(child, "child body");

  const int before = count_commits(project_root);
  store.move(
      child.card_id,
      true,
      std::optional<std::string>(parent.card_id),
      std::optional<double>(42.5),
      20
  );
  const int after = count_commits(project_root);
  REQUIRE(after == before + 1);

  holder::card::CardRepo card_repo(db);
  const auto moved = card_repo.get(child.card_id);
  REQUIRE(moved.has_value());
  REQUIRE(moved->parent_card_id.has_value());
  REQUIRE(moved->parent_card_id.value() == parent.card_id);
  REQUIRE(moved->sort_key == 42.5);
  REQUIRE(moved->updated_at == 20);

  const auto rel_path = holder::core::card_rel_path(child.card_id);
  const auto full_path = project_root / rel_path;
  const auto raw = read_file(full_path);
  const auto parsed = holder::core::parse_card_file(raw);
  REQUIRE(parsed.has_front_matter);
  REQUIRE(parsed.card.parent_card_id.has_value());
  REQUIRE(parsed.card.parent_card_id.value() == parent.card_id);
  REQUIRE(parsed.card.sort_key == 42.5);
  REQUIRE(parsed.card.updated_at == 20);
  REQUIRE(parsed.body == "child body");
}

TEST_CASE("CardStore create rejects duplicate card_id", "[cardstore]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);
  holder::model::Card card;
  card.card_id = "abcd1111";
  card.project_id = "proj-1";
  card.title = "First";
  card.created_at = 10;
  card.updated_at = 10;

  store.create(card, "one");
  REQUIRE_THROWS_WITH(
      store.create(card, "two"),
      Catch::Matchers::ContainsSubstring("conflict: card_id already exists")
  );
}

TEST_CASE("CardStore create rejects existing file without DB row", "[cardstore]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);
  holder::model::Card card;
  card.card_id = "abca2222";
  card.project_id = "proj-1";
  card.title = "First";
  card.created_at = 10;
  card.updated_at = 10;

  const auto rel_path = holder::core::card_rel_path(card.card_id);
  holder::git::GitRepo repo;
  repo.open_or_init(project_root);
  repo.write_file(rel_path, "manual");

  REQUIRE_THROWS_WITH(
      store.create(card, "one"),
      Catch::Matchers::ContainsSubstring("conflict: card file already exists")
  );
}

TEST_CASE("CardStore create appends to end of sibling scope when sort omitted", "[cardstore]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);

  holder::model::Card root_a;
  root_a.card_id = "root-a";
  root_a.project_id = "proj-1";
  root_a.title = "Root A";
  root_a.created_at = 10;
  root_a.updated_at = 10;
  store.create(root_a, "a");

  holder::model::Card root_b;
  root_b.card_id = "root-b";
  root_b.project_id = "proj-1";
  root_b.title = "Root B";
  root_b.created_at = 11;
  root_b.updated_at = 11;
  store.create(root_b, "b");

  holder::model::Card parent;
  parent.card_id = "parent-a";
  parent.project_id = "proj-1";
  parent.title = "Parent";
  parent.created_at = 12;
  parent.updated_at = 12;
  store.create(parent, "p");

  holder::model::Card child_a;
  child_a.card_id = "child-a";
  child_a.project_id = "proj-1";
  child_a.parent_card_id = parent.card_id;
  child_a.title = "Child A";
  child_a.created_at = 13;
  child_a.updated_at = 13;
  store.create(child_a, "ca");

  holder::model::Card child_b;
  child_b.card_id = "child-b";
  child_b.project_id = "proj-1";
  child_b.parent_card_id = parent.card_id;
  child_b.title = "Child B";
  child_b.created_at = 14;
  child_b.updated_at = 14;
  store.create(child_b, "cb");

  holder::card::CardRepo card_repo(db);
  const auto got_root_a = card_repo.get("root-a");
  const auto got_root_b = card_repo.get("root-b");
  const auto got_parent = card_repo.get("parent-a");
  const auto got_child_a = card_repo.get("child-a");
  const auto got_child_b = card_repo.get("child-b");
  REQUIRE(got_root_a.has_value());
  REQUIRE(got_root_b.has_value());
  REQUIRE(got_parent.has_value());
  REQUIRE(got_child_a.has_value());
  REQUIRE(got_child_b.has_value());

  REQUIRE(got_root_a->sort_key == 0.0);
  REQUIRE(got_root_b->sort_key == 1.0);
  REQUIRE(got_parent->sort_key == 2.0);
  REQUIRE(got_child_a->sort_key == 0.0);
  REQUIRE(got_child_b->sort_key == 1.0);
}

TEST_CASE("CardStore create preserves explicit sort_key", "[cardstore]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);

  holder::model::Card card;
  card.card_id = "explicit-1";
  card.project_id = "proj-1";
  card.title = "Explicit";
  card.created_at = 10;
  card.updated_at = 10;

  store.create(card, "body", std::optional<double>(42.5));

  holder::card::CardRepo card_repo(db);
  const auto fetched = card_repo.get(card.card_id);
  REQUIRE(fetched.has_value());
  REQUIRE(fetched->sort_key == 42.5);
}

TEST_CASE("CardStore move across parent appends when sort omitted", "[cardstore]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);

  holder::model::Card parent_a;
  parent_a.card_id = "parent-a";
  parent_a.project_id = "proj-1";
  parent_a.title = "Parent A";
  parent_a.created_at = 10;
  parent_a.updated_at = 10;
  store.create(parent_a, "pa");

  holder::model::Card parent_b;
  parent_b.card_id = "parent-b";
  parent_b.project_id = "proj-1";
  parent_b.title = "Parent B";
  parent_b.created_at = 11;
  parent_b.updated_at = 11;
  store.create(parent_b, "pb");

  holder::model::Card b_child_1;
  b_child_1.card_id = "b-child-1";
  b_child_1.project_id = "proj-1";
  b_child_1.parent_card_id = parent_b.card_id;
  b_child_1.title = "B Child 1";
  b_child_1.created_at = 12;
  b_child_1.updated_at = 12;
  store.create(b_child_1, "b1");

  holder::model::Card moving;
  moving.card_id = "moving-1";
  moving.project_id = "proj-1";
  moving.parent_card_id = parent_a.card_id;
  moving.title = "Moving";
  moving.created_at = 13;
  moving.updated_at = 13;
  store.create(moving, "m1");

  store.move(moving.card_id, true, std::optional<std::string>(parent_b.card_id), std::nullopt, 20);

  holder::card::CardRepo card_repo(db);
  const auto moved = card_repo.get(moving.card_id);
  REQUIRE(moved.has_value());
  REQUIRE(moved->parent_card_id.has_value());
  REQUIRE(moved->parent_card_id.value() == parent_b.card_id);
  REQUIRE(moved->sort_key == 1.0);
}

TEST_CASE("CardStore create throws when project is missing", "[cardstore]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);
  holder::model::Card card;
  card.card_id = "missprj1";
  card.project_id = "no-such-project";
  card.title = "Missing";
  card.created_at = 1;
  card.updated_at = 1;

  REQUIRE_THROWS_WITH(
      store.create(card, "x"),
      Catch::Matchers::ContainsSubstring("project not found: no-such-project")
  );
}

TEST_CASE("CardStore create throws on rel_path mismatch", "[cardstore]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  create_project(db, "proj-1", (dir / "project_repo").string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);
  holder::model::Card card;
  card.card_id = "relmis01";
  card.project_id = "proj-1";
  card.rel_path = "cards/wrong.md";
  card.title = "BadRel";
  card.created_at = 1;
  card.updated_at = 1;

  REQUIRE_THROWS_WITH(
      store.create(card, "x"),
      Catch::Matchers::ContainsSubstring("card rel_path does not match card_id")
  );
}

TEST_CASE("CardStore create cleanup removes file when DB write fails", "[cardstore]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  CloseDbOnStageGitOps git(db);
  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts, nullptr, &git);

  holder::model::Card card;
  card.card_id = "dbfail01";
  card.project_id = "proj-1";
  card.title = "DBFail";
  card.created_at = 1;
  card.updated_at = 1;

  REQUIRE_THROWS_WITH(
      store.create(card, "x"),
      Catch::Matchers::ContainsSubstring("prepare insert card failed: unknown sqlite error")
  );
  REQUIRE_FALSE(std::filesystem::exists(project_root / holder::core::card_rel_path(card.card_id)));
}

TEST_CASE("CardStore encrypted create/get_content throw without key_id", "[cardstore]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";

  holder::project::ProjectRepo project_repo(db);
  holder::model::Project project;
  project.project_id = "proj-enc-missing-key";
  project.name = "Encrypted";
  project.root_path = project_root.string();
  project.privacy_mode = "encrypted_git";
  project.project_key_id = std::nullopt;
  project.created_at = 1;
  project.updated_at = 1;
  project_repo.create(project);

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);

  holder::model::Card card;
  card.card_id = "enckey01";
  card.project_id = project.project_id;
  card.title = "Encrypted";
  card.created_at = 1;
  card.updated_at = 1;
  REQUIRE_THROWS_WITH(
      store.create(card, "secret"),
      Catch::Matchers::ContainsSubstring("encrypted project missing project_key_id")
  );

  holder::card::CardRepo card_repo(db);
  holder::model::Card inserted = card;
  inserted.rel_path = holder::core::card_rel_path(inserted.card_id);
  inserted.sort_key = 0.0;
  card_repo.create(inserted);
  holder::git::GitRepo repo;
  repo.open_or_init(project_root);
  repo.write_file(inserted.rel_path, "HolderPriv1\nbad");

  REQUIRE_THROWS_WITH(
      (void)store.get_content(inserted),
      Catch::Matchers::ContainsSubstring("encrypted project missing project_key_id")
  );
}

TEST_CASE("CardStore update_content throws when card is missing", "[cardstore]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  create_project(db, "proj-1", (dir / "project_repo").string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);
  REQUIRE_THROWS_WITH(
      store.update_content("missing", "x", std::nullopt, 2),
      Catch::Matchers::ContainsSubstring("card not found: missing")
  );
}

TEST_CASE("CardStore move exercises error and no-op branches", "[cardstore]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);
  holder::card::CardRepo card_repo(db);

  REQUIRE_THROWS_WITH(
      store.move("missing", false, std::nullopt, std::nullopt, 2),
      Catch::Matchers::ContainsSubstring("card not found: missing")
  );

  holder::model::Card bad_rel;
  bad_rel.card_id = "movbad01";
  bad_rel.project_id = "proj-1";
  bad_rel.rel_path = "cards/wrong.md";
  bad_rel.title = "Bad";
  bad_rel.sort_key = 0.0;
  bad_rel.created_at = 1;
  bad_rel.updated_at = 1;
  card_repo.create(bad_rel);
  REQUIRE_THROWS_WITH(
      store.move(bad_rel.card_id, false, std::nullopt, std::nullopt, 2),
      "card rel_path does not match card_id"
  );

  holder::model::Card noop;
  noop.card_id = "movnop01";
  noop.project_id = "proj-1";
  noop.title = "Noop";
  noop.created_at = 1;
  noop.updated_at = 1;
  store.create(noop, "body");
  const int before_noop = count_commits(project_root);
  store.move(noop.card_id, false, std::nullopt, std::nullopt, 2);
  REQUIRE(count_commits(project_root) == before_noop);

  holder::model::Card missing_body;
  missing_body.card_id = "movmis01";
  missing_body.project_id = "proj-1";
  missing_body.title = "MissingBody";
  missing_body.created_at = 1;
  missing_body.updated_at = 1;
  store.create(missing_body, "body");
  std::filesystem::remove(project_root / holder::core::card_rel_path(missing_body.card_id));
  REQUIRE_THROWS_WITH(
      store.move(missing_body.card_id, true, std::optional<std::string>("parentx"), std::nullopt, 2),
      "card content missing"
  );

  // Make file front matter already match target move while DB still has old values.
  // This forces changed=true but updated_raw==raw, exercising the no-write branch.
  holder::model::Card stale_db;
  stale_db.card_id = "movraw01";
  stale_db.project_id = "proj-1";
  stale_db.title = "RawNoop";
  stale_db.created_at = 1;
  stale_db.updated_at = 1;
  store.create(stale_db, "body", std::optional<double>(1.0));

  const auto stale_db_opt = card_repo.get(stale_db.card_id);
  REQUIRE(stale_db_opt.has_value());
  const auto stale_rel = holder::core::card_rel_path(stale_db.card_id);
  const auto stale_path = project_root / stale_rel;
  const auto stale_raw = read_file(stale_path);
  const auto stale_parsed = holder::core::parse_card_file(stale_raw);
  REQUIRE(stale_parsed.has_front_matter);

  auto file_card = stale_db_opt.value();
  file_card.parent_card_id = std::optional<std::string>("parent-target");
  file_card.sort_key = 7.0;
  file_card.updated_at = 77;
  const auto prewritten_raw = holder::core::render_card_front_matter(file_card, {}, {}) +
                              stale_parsed.body;
  holder::git::GitRepo stale_repo;
  stale_repo.open_or_init(project_root);
  stale_repo.write_file(stale_rel, prewritten_raw);

  const int before_raw_noop = count_commits(project_root);
  store.move(
      stale_db.card_id,
      true,
      std::optional<std::string>("parent-target"),
      std::optional<double>(7.0),
      77
  );
  REQUIRE(count_commits(project_root) == before_raw_noop);
}

TEST_CASE("CardStore move encrypted branch updates and commits", "[cardstore]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";

  holder::project::ProjectRepo project_repo(db);
  holder::model::Project project;
  project.project_id = "proj-enc-move";
  project.name = "Encrypted Move";
  project.root_path = project_root.string();
  project.privacy_mode = "encrypted_git";
  project.project_key_id.reset();
  project.created_at = 1;
  project.updated_at = 1;
  project_repo.create(project);

  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  holder::git::RealGitOps bootstrap_git;
  holder::privacy::ensure_encrypted_project_ready(
      bootstrap_git,
      project_repo,
      project.project_id,
      project.root_path,
      std::nullopt,
      2,
      []() {
        return std::string("key-move");
      }
  );

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);

  holder::model::Card parent;
  parent.card_id = "encpar01";
  parent.project_id = project.project_id;
  parent.title = "P";
  parent.created_at = 1;
  parent.updated_at = 1;
  store.create(parent, "p");

  holder::model::Card card;
  card.card_id = "encmov01";
  card.project_id = project.project_id;
  card.parent_card_id = parent.card_id;
  card.title = "C";
  card.created_at = 1;
  card.updated_at = 1;
  store.create(card, "c", std::optional<double>(7.0));

  const int before = count_commits(project_root);
  store.move(card.card_id, false, std::nullopt, std::optional<double>(8.0), 2);
  REQUIRE(count_commits(project_root) == before + 1);
}

TEST_CASE("CardStore update_links exercises error, encrypted, and no-op branches", "[cardstore]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);
  holder::card::CardRepo card_repo(db);

  REQUIRE_THROWS_WITH(
      store.update_links("missing", 2),
      Catch::Matchers::ContainsSubstring("card not found: missing")
  );

  holder::model::Card bad_rel;
  bad_rel.card_id = "lnkbad01";
  bad_rel.project_id = "proj-1";
  bad_rel.rel_path = "cards/wrong.md";
  bad_rel.title = "Bad";
  bad_rel.sort_key = 0.0;
  bad_rel.created_at = 1;
  bad_rel.updated_at = 1;
  card_repo.create(bad_rel);
  REQUIRE_THROWS_WITH(
      store.update_links(bad_rel.card_id, 2), "card rel_path does not match card_id"
  );

  holder::model::Card noop;
  noop.card_id = "lnknop01";
  noop.project_id = "proj-1";
  noop.title = "Noop";
  noop.created_at = 1;
  noop.updated_at = 10;
  store.create(noop, "body");
  const int before = count_commits(project_root);
  store.update_links(noop.card_id, 10);
  REQUIRE(count_commits(project_root) == before);

  // Encrypted update_links path
  holder::project::ProjectRepo project_repo(db);
  holder::model::Project enc;
  enc.project_id = "proj-enc-links";
  enc.name = "Encrypted Links";
  enc.root_path = (dir / "project_repo_enc").string();
  enc.privacy_mode = "encrypted_git";
  enc.project_key_id.reset();
  enc.created_at = 1;
  enc.updated_at = 1;
  project_repo.create(enc);
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  holder::git::RealGitOps bootstrap_git;
  holder::privacy::ensure_encrypted_project_ready(
      bootstrap_git,
      project_repo,
      enc.project_id,
      enc.root_path,
      std::nullopt,
      2,
      []() {
        return std::string("key-links");
      }
  );

  holder::model::Card enc_card;
  enc_card.card_id = "enclnk01";
  enc_card.project_id = enc.project_id;
  enc_card.title = "Enc";
  enc_card.created_at = 1;
  enc_card.updated_at = 1;
  store.create(enc_card, "body");
  const int before_enc = count_commits(enc.root_path);
  store.update_links(enc_card.card_id, 2);
  REQUIRE(count_commits(enc.root_path) == before_enc + 1);
}

TEST_CASE("CardStore trash/restore/hard_delete and get_content guards", "[cardstore]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);
  holder::card::CardRepo card_repo(db);

  REQUIRE_THROWS_WITH(
      store.trash("missing", 2),
      Catch::Matchers::ContainsSubstring("card not found: missing")
  );
  REQUIRE_THROWS_WITH(
      store.restore("missing", 2),
      Catch::Matchers::ContainsSubstring("card not found: missing")
  );
  REQUIRE_THROWS_WITH(
      store.hard_delete("missing"),
      Catch::Matchers::ContainsSubstring("card not found: missing")
  );

  holder::model::Card active;
  active.card_id = "trashg01";
  active.project_id = "proj-1";
  active.title = "Active";
  active.created_at = 1;
  active.updated_at = 1;
  store.create(active, "body");
  REQUIRE_THROWS_WITH(
      store.restore(active.card_id, 2),
      Catch::Matchers::ContainsSubstring("card is not deleted")
  );
  REQUIRE_THROWS_WITH(
      store.hard_delete(active.card_id),
      Catch::Matchers::ContainsSubstring("card is not deleted")
  );

  holder::model::Card removable;
  removable.card_id = "trashok1";
  removable.project_id = "proj-1";
  removable.title = "Trash OK";
  removable.created_at = 1;
  removable.updated_at = 1;
  store.create(removable, "restorable body");

  const auto live_rel = holder::core::card_rel_path(removable.card_id);
  const auto trash_rel = holder::core::card_trash_rel_path(removable.card_id);

  store.trash(removable.card_id, 20);
  REQUIRE_FALSE(std::filesystem::exists(project_root / live_rel));
  REQUIRE(std::filesystem::exists(project_root / trash_rel));

  store.restore(removable.card_id, 21);
  const auto restored = store.get(removable.card_id);
  REQUIRE(restored.has_value());
  REQUIRE_FALSE(restored->deleted_at.has_value());
  REQUIRE(std::filesystem::exists(project_root / live_rel));
  REQUIRE_FALSE(std::filesystem::exists(project_root / trash_rel));

  store.trash(removable.card_id, 22);
  store.hard_delete(removable.card_id);
  REQUIRE_FALSE(store.get(removable.card_id).has_value());
  REQUIRE_FALSE(std::filesystem::exists(project_root / live_rel));
  REQUIRE_FALSE(std::filesystem::exists(project_root / trash_rel));

  holder::model::Card bad_rel;
  bad_rel.card_id = "trashb01";
  bad_rel.project_id = "proj-1";
  bad_rel.rel_path = "cards/wrong.md";
  bad_rel.title = "BadRel";
  bad_rel.sort_key = 0.0;
  bad_rel.created_at = 1;
  bad_rel.updated_at = 1;
  card_repo.create(bad_rel);
  REQUIRE_THROWS_WITH(
      store.trash(bad_rel.card_id, 2),
      Catch::Matchers::ContainsSubstring("card rel_path does not match card_id")
  );

  holder::model::Card deleted_bad_rel = bad_rel;
  deleted_bad_rel.card_id = "restbad1";
  deleted_bad_rel.rel_path = "cards/another-wrong.md";
  deleted_bad_rel.deleted_at = 10;
  card_repo.create(deleted_bad_rel);
  REQUIRE_THROWS_WITH(
      store.trash(deleted_bad_rel.card_id, 2),
      Catch::Matchers::ContainsSubstring("card already deleted")
  );
  REQUIRE_THROWS_WITH(
      store.restore(deleted_bad_rel.card_id, 2),
      Catch::Matchers::ContainsSubstring("card rel_path does not match card_id")
  );

  holder::model::Card content_missing;
  content_missing.card_id = "contmiss";
  content_missing.project_id = "proj-1";
  content_missing.title = "Missing";
  content_missing.created_at = 1;
  content_missing.updated_at = 1;
  content_missing.rel_path = holder::core::card_rel_path(content_missing.card_id);
  content_missing.sort_key = 0.0;
  card_repo.create(content_missing);
  const auto maybe = store.get_content(content_missing);
  REQUIRE_FALSE(maybe.has_value());

  holder::model::Card bad_content = content_missing;
  bad_content.card_id = "contbad1";
  bad_content.rel_path = "cards/wrong-content.md";
  card_repo.create(bad_content);
  REQUIRE_THROWS_WITH(
      (void)store.get_content(bad_content),
      Catch::Matchers::ContainsSubstring("card rel_path does not match card_id")
  );

  holder::model::Card no_project = content_missing;
  no_project.card_id = "contprj1";
  no_project.project_id = "missing-project";
  no_project.rel_path = holder::core::card_rel_path(no_project.card_id);
  no_project.title = "NoProject";
  REQUIRE_THROWS_WITH(
      (void)store.get_content(no_project),
      Catch::Matchers::ContainsSubstring("project not found: missing-project")
  );
}

TEST_CASE(
    "CardStore stamps the real deleted_at into the durable file, not just SQLite",
    "[cardstore]"
) {
  // Regression test: trash() used to only update the SQLite row, leaving the trash file's own
  // front matter with deleted_at still unset. A later rebuild-from-source had nothing but the
  // file's filesystem mtime to reconstruct deleted_at from -- and mtime isn't preserved across
  // git clone/checkout/backup-restore, so the true trash time silently drifted or was lost.
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);

  holder::model::Card card;
  card.card_id = "stampat1";
  card.project_id = "proj-1";
  card.title = "Stamped";
  card.created_at = 1;
  card.updated_at = 1;
  store.create(card, "body");

  store.trash(card.card_id, 999);

  // read_file() closes its stream before returning. Holding the trash file open across restore()
  // would make the rename fail on Windows, which refuses to rename a file that has an open handle.
  const auto trash_rel = holder::core::card_trash_rel_path(card.card_id);
  const auto trash_raw = read_file(project_root / trash_rel);
  const auto trash_parsed = holder::core::parse_card_file(trash_raw);
  REQUIRE(trash_parsed.card.deleted_at.has_value());
  REQUIRE(trash_parsed.card.deleted_at.value() == 999);
  REQUIRE(trash_parsed.card.updated_at == 999);
  REQUIRE(trash_parsed.body == "body");

  store.restore(card.card_id, 1000);

  const auto live_rel = holder::core::card_rel_path(card.card_id);
  const auto live_raw = read_file(project_root / live_rel);
  const auto live_parsed = holder::core::parse_card_file(live_raw);
  REQUIRE_FALSE(live_parsed.card.deleted_at.has_value());
  REQUIRE(live_parsed.card.updated_at == 1000);
  REQUIRE(live_parsed.body == "body");
}

TEST_CASE("CardStore restores a historical card snapshot as a new commit", "[cardstore][history]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-restore-version", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);
  holder::model::Card card;
  card.card_id = "restore01";
  card.project_id = "proj-restore-version";
  card.title = "Original title";
  card.created_at = 1;
  card.updated_at = 1;
  store.create(card, "# Original\n#old\n");

  holder::git::GitRepo repo;
  repo.open_existing(project_root);
  const auto historical_oid = repo.head_oid();
  REQUIRE(historical_oid.has_value());
  store.update_content(card.card_id, "# Current\n#new\n", std::string("Current title"), 2);
  const int commits_before_restore = count_commits(project_root);

  store.restore_version(card.card_id, *historical_oid, 50);

  const auto restored = store.get(card.card_id);
  REQUIRE(restored.has_value());
  CHECK(restored->title == "Original title");
  CHECK(restored->created_at == 1);
  CHECK(restored->updated_at == 50);
  CHECK_FALSE(restored->deleted_at.has_value());
  REQUIRE(store.get_content(*restored).value() == "# Original\n#old\n");
  CHECK(count_commits(project_root) == commits_before_restore + 1);
  repo.open_existing(project_root);
  CHECK(repo.head_oid() != historical_oid);
}

TEST_CASE("CardStore restores an encrypted historical card snapshot", "[cardstore][history]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  holder::project::ProjectRepo project_repo(db);
  holder::model::Project project;
  project.project_id = "proj-encrypted-restore";
  project.name = "Encrypted restore";
  project.root_path = (dir / "project_repo").string();
  project.privacy_mode = "encrypted_git";
  project.created_at = 1;
  project.updated_at = 1;
  project_repo.create(project);
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  holder::git::RealGitOps bootstrap_git;
  holder::privacy::ensure_encrypted_project_ready(
      bootstrap_git, project_repo, project.project_id, project.root_path, std::nullopt, 2,
      []() { return std::string("key-history-restore"); }
  );

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);
  holder::model::Card card;
  card.card_id = "encrest01";
  card.project_id = project.project_id;
  card.title = "Encrypted original";
  card.created_at = 1;
  card.updated_at = 1;
  store.create(card, "original encrypted body");
  holder::git::GitRepo repo;
  repo.open_existing(project.root_path);
  const auto historical_oid = repo.head_oid();
  REQUIRE(historical_oid.has_value());
  store.update_content(card.card_id, "changed encrypted body", std::nullopt, 2);

  store.restore_version(card.card_id, *historical_oid, 50);

  const auto restored = store.get(card.card_id);
  REQUIRE(restored.has_value());
  CHECK(restored->title == "Encrypted original");
  CHECK(restored->updated_at == 50);
  REQUIRE(store.get_content(*restored).value() == "original encrypted body");
  const auto raw = read_file(
      std::filesystem::path(project.root_path) / holder::core::card_rel_path(card.card_id)
  );
  CHECK(raw.find("original encrypted body") == std::string::npos);
}

TEST_CASE("CardStore leaves the current version untouched when historical metadata restore fails", "[cardstore][history]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-restore-rollback", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);
  holder::card::LinkRepo links(db);
  holder::model::Card card;
  card.card_id = "rollback01";
  card.project_id = "proj-restore-rollback";
  card.title = "Historical title";
  card.created_at = 1;
  card.updated_at = 1;
  store.create(card, "historical body");

  holder::model::CardLink historical_link;
  historical_link.project_id = card.project_id;
  historical_link.from_card_id = card.card_id;
  historical_link.to_card_id = "linked-card";
  historical_link.to_type = "card";
  historical_link.kind = "wiki";
  historical_link.created_at = 1;
  links.upsert_links(card.project_id, card.card_id, {historical_link});
  store.update_links(card.card_id, 2);

  holder::git::GitRepo repo;
  repo.open_existing(project_root);
  const auto historical_oid = repo.head_oid();
  REQUIRE(historical_oid.has_value());
  store.update_content(card.card_id, "current body", std::string("Current title"), 3);
  repo.open_existing(project_root);
  const auto head_before = repo.head_oid();
  REQUIRE(head_before.has_value());
  const auto current_raw = read_file(project_root / holder::core::card_rel_path(card.card_id));

  db.exec("CREATE TRIGGER block_history_restore_link "
          "BEFORE INSERT ON card_links "
          "BEGIN SELECT RAISE(ABORT, 'blocked historical link'); END;");

  REQUIRE_THROWS_WITH(
      store.restore_version(card.card_id, *historical_oid, 50),
      Catch::Matchers::ContainsSubstring("upsert link failed: blocked historical link")
  );

  const auto current = store.get(card.card_id);
  REQUIRE(current.has_value());
  CHECK(current->title == "Current title");
  REQUIRE(store.get_content(*current).value() == "current body");
  CHECK(read_file(project_root / holder::core::card_rel_path(card.card_id)) == current_raw);
  repo.open_existing(project_root);
  CHECK(repo.head_oid() == head_before);
  const auto current_links = links.list_outgoing(card.project_id, card.card_id);
  REQUIRE(current_links.size() == 1);
  CHECK(current_links[0].to_card_id == "linked-card");
}

TEST_CASE("CardStore keeps card_tags in sync across create/update/trash/restore/hard_delete", "[cardstore]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);
  holder::card::TagRepo tags(db);

  holder::model::Card card;
  card.card_id = "tagsync1";
  card.project_id = "proj-1";
  card.title = "Tagged card";
  card.created_at = 1;
  card.updated_at = 1;
  store.create(card, "Body mentioning #todo and #android.");

  REQUIRE(
      tags.list_tags_for_card("proj-1", card.card_id) == std::vector<std::string>{"android", "todo"}
  );
  REQUIRE(tags.list_card_ids_with_tag("proj-1", "todo") == std::vector<std::string>{card.card_id});

  store.update_content(card.card_id, "Now only #urgent remains.", std::nullopt, 2);
  REQUIRE(tags.list_tags_for_card("proj-1", card.card_id) == std::vector<std::string>{"urgent"});
  REQUIRE(tags.list_card_ids_with_tag("proj-1", "todo").empty());

  store.trash(card.card_id, 3);
  REQUIRE(tags.list_tags_for_card("proj-1", card.card_id).empty());
  REQUIRE(tags.list_card_ids_with_tag("proj-1", "urgent").empty());

  store.restore(card.card_id, 4);
  REQUIRE(tags.list_tags_for_card("proj-1", card.card_id) == std::vector<std::string>{"urgent"});
  REQUIRE(tags.list_card_ids_with_tag("proj-1", "urgent") == std::vector<std::string>{card.card_id});

  store.trash(card.card_id, 5);
  store.hard_delete(card.card_id);
  REQUIRE(tags.list_tags_for_card("proj-1", card.card_id).empty());
  REQUIRE(tags.list_card_ids_with_tag("proj-1", "urgent").empty());
}

TEST_CASE("CardStore update_milestones exercises error, encrypted, and no-op branches", "[cardstore]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);
  holder::card::CardRepo card_repo(db);
  holder::card::MilestoneRepo milestones(db);

  REQUIRE_THROWS_WITH(
      store.update_milestones("missing", 2),
      Catch::Matchers::ContainsSubstring("card not found: missing")
  );

  holder::model::Card bad_rel;
  bad_rel.card_id = "milbad01";
  bad_rel.project_id = "proj-1";
  bad_rel.rel_path = "cards/wrong.md";
  bad_rel.title = "Bad";
  bad_rel.sort_key = 0.0;
  bad_rel.created_at = 1;
  bad_rel.updated_at = 1;
  card_repo.create(bad_rel);
  REQUIRE_THROWS_WITH(
      store.update_milestones(bad_rel.card_id, 2), "card rel_path does not match card_id"
  );

  holder::model::Card noop;
  noop.card_id = "milnop01";
  noop.project_id = "proj-1";
  noop.title = "Noop";
  noop.created_at = 1;
  noop.updated_at = 10;
  store.create(noop, "body");
  const int before = count_commits(project_root);
  store.update_milestones(noop.card_id, 10);
  REQUIRE(count_commits(project_root) == before);

  holder::model::Milestone milestone;
  milestone.milestone_id = "mile-1";
  milestone.project_id = "proj-1";
  milestone.card_id = noop.card_id;
  milestone.start_at = 100;
  milestone.kind = "Renewal";
  milestone.created_at = 1;
  milestone.updated_at = 1;
  milestones.replace_for_card("proj-1", noop.card_id, {milestone});

  store.update_milestones(noop.card_id, 11);
  REQUIRE(count_commits(project_root) == before + 1);

  const auto raw = read_file(project_root / holder::core::card_rel_path(noop.card_id));
  const auto parsed = holder::core::parse_card_file(raw);
  REQUIRE(parsed.milestones.size() == 1);
  REQUIRE(parsed.milestones[0].milestone_id == "mile-1");

  // Encrypted update_milestones path
  holder::project::ProjectRepo project_repo(db);
  holder::model::Project enc;
  enc.project_id = "proj-enc-milestones";
  enc.name = "Encrypted Milestones";
  enc.root_path = (dir / "project_repo_enc").string();
  enc.privacy_mode = "encrypted_git";
  enc.project_key_id.reset();
  enc.created_at = 1;
  enc.updated_at = 1;
  project_repo.create(enc);
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  holder::git::RealGitOps bootstrap_git;
  holder::privacy::ensure_encrypted_project_ready(
      bootstrap_git,
      project_repo,
      enc.project_id,
      enc.root_path,
      std::nullopt,
      2,
      []() {
        return std::string("key-milestones");
      }
  );

  holder::model::Card enc_card;
  enc_card.card_id = "encmil01";
  enc_card.project_id = enc.project_id;
  enc_card.title = "Enc";
  enc_card.created_at = 1;
  enc_card.updated_at = 1;
  store.create(enc_card, "body");
  milestones.replace_for_card(
      enc.project_id, enc_card.card_id, {make_milestone_for("mile-enc", enc.project_id, enc_card.card_id)}
  );
  const int before_enc = count_commits(enc.root_path);
  store.update_milestones(enc_card.card_id, 2);
  REQUIRE(count_commits(enc.root_path) == before_enc + 1);
}

TEST_CASE("CardStore restores historical live and Trash lifecycle snapshots", "[cardstore][history]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-restore-lifecycle", project_root.string());
  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);
  holder::card::LinkRepo links(db);
  holder::card::MilestoneRepo milestones(db);

  holder::model::Card card;
  card.card_id = "restlife";
  card.project_id = "proj-restore-lifecycle";
  card.title = "Original name";
  card.created_at = 1;
  card.updated_at = 1;
  store.create(card, "original body");

  holder::model::CardLink original_link;
  original_link.project_id = card.project_id;
  original_link.from_card_id = card.card_id;
  original_link.to_card_id = "original-target";
  original_link.to_type = "card";
  original_link.kind = "wiki";
  original_link.label = "Original link";
  original_link.created_at = 1;
  links.upsert_links(card.project_id, card.card_id, {original_link});
  store.update_links(card.card_id, 2);

  auto original_milestone = make_milestone_for("original-milestone", card.project_id, card.card_id);
  original_milestone.end_at = 200;
  original_milestone.kind = "Review";
  original_milestone.description = "Original milestone";
  milestones.replace_for_card(card.project_id, card.card_id, {original_milestone});
  store.update_milestones(card.card_id, 3);

  holder::git::GitRepo git;
  git.open_existing(project_root);
  const auto live_oid = git.head_oid();
  REQUIRE(live_oid.has_value());

  holder::model::CardLink current_link;
  current_link.project_id = card.project_id;
  current_link.from_card_id = card.card_id;
  current_link.to_card_id = "current-target";
  current_link.to_type = "resource";
  current_link.kind = "ref";
  current_link.label = "Current link";
  current_link.created_at = 4;
  links.delete_links_from(card.project_id, card.card_id);
  links.upsert_links(card.project_id, card.card_id, {current_link});
  store.update_links(card.card_id, 4);

  auto current_milestone = make_milestone_for("current-milestone", card.project_id, card.card_id);
  current_milestone.start_at = 300;
  current_milestone.kind = "Current";
  milestones.replace_for_card(card.project_id, card.card_id, {current_milestone});
  store.update_milestones(card.card_id, 5);

  store.update_content(card.card_id, "renamed body", std::string("Renamed card"), 6);
  store.trash(card.card_id, 7);
  git.open_existing(project_root);
  const auto trash_oid = git.head_oid();
  REQUIRE(trash_oid.has_value());

  store.restore_version(card.card_id, *live_oid, 10);
  const auto live = store.get(card.card_id);
  REQUIRE(live.has_value());
  CHECK(live->title == "Original name");
  CHECK_FALSE(live->deleted_at.has_value());
  REQUIRE(store.get_content(*live).value() == "original body");
  const auto restored_links = links.list_outgoing(card.project_id, card.card_id);
  REQUIRE(restored_links.size() == 1);
  CHECK(restored_links[0].to_card_id == "original-target");
  CHECK(restored_links[0].to_type == "card");
  CHECK(restored_links[0].kind == "wiki");
  CHECK(restored_links[0].label == std::optional<std::string>("Original link"));
  const auto restored_milestones = milestones.list_for_card(card.project_id, card.card_id);
  REQUIRE(restored_milestones.size() == 1);
  CHECK(restored_milestones[0].milestone_id == "original-milestone");
  CHECK(restored_milestones[0].end_at == std::optional<long long>(200));
  CHECK(restored_milestones[0].kind == std::optional<std::string>("Review"));
  CHECK(restored_milestones[0].description == std::optional<std::string>("Original milestone"));
  CHECK(std::filesystem::exists(project_root / holder::core::card_rel_path(card.card_id)));
  CHECK_FALSE(std::filesystem::exists(project_root / holder::core::card_trash_rel_path(card.card_id)));

  store.restore_version(card.card_id, *trash_oid, 11);
  const auto trashed = store.get(card.card_id);
  REQUIRE(trashed.has_value());
  CHECK(trashed->title == "Renamed card");
  CHECK(trashed->deleted_at.has_value());
  CHECK_FALSE(std::filesystem::exists(project_root / holder::core::card_rel_path(card.card_id)));
  CHECK(std::filesystem::exists(project_root / holder::core::card_trash_rel_path(card.card_id)));
}

TEST_CASE("CardStore keeps milestones in sync across trash/restore/hard_delete", "[cardstore]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);
  holder::card::MilestoneRepo milestones(db);

  holder::model::Card card;
  card.card_id = "milsync1";
  card.project_id = "proj-1";
  card.title = "Milestone card";
  card.created_at = 1;
  card.updated_at = 1;
  store.create(card, "Body");

  milestones.replace_for_card(
      "proj-1", card.card_id, {make_milestone_for("mile-1", "proj-1", card.card_id)}
  );
  store.update_milestones(card.card_id, 2);
  REQUIRE(milestones.list_for_card("proj-1", card.card_id).size() == 1);

  store.trash(card.card_id, 3);
  REQUIRE(milestones.list_for_card("proj-1", card.card_id).empty());

  store.restore(card.card_id, 4);
  const auto after_restore = milestones.list_for_card("proj-1", card.card_id);
  REQUIRE(after_restore.size() == 1);
  REQUIRE(after_restore[0].milestone_id == "mile-1");

  store.trash(card.card_id, 5);
  store.hard_delete(card.card_id);
  REQUIRE(milestones.list_for_card("proj-1", card.card_id).empty());
}

TEST_CASE("CardStore add_tag writes to the trailing tag line and reindexes", "[cardstore][tags]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);
  holder::card::TagRepo tags(db);

  holder::model::Card card;
  card.card_id = "addtag1";
  card.project_id = "proj-1";
  card.title = "Card";
  card.created_at = 1;
  card.updated_at = 1;
  store.create(card, "Some useful thing");

  REQUIRE(store.add_tag(card.card_id, "Work", 2) == holder::card::AddTagResult::Added);
  REQUIRE(store.get_content(store.get(card.card_id).value()).value() == "Some useful thing\n\n#work");
  REQUIRE(tags.list_tags_for_card("proj-1", card.card_id) == std::vector<std::string>{"work"});

  REQUIRE(store.add_tag(card.card_id, "android", 3) == holder::card::AddTagResult::Added);
  REQUIRE(
      store.get_content(store.get(card.card_id).value()).value() == "Some useful thing\n\n#work #android"
  );
}

TEST_CASE("CardStore add_tag is idempotent for a tag already present anywhere", "[cardstore][tags]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);

  holder::model::Card card;
  card.card_id = "addtag2";
  card.project_id = "proj-1";
  card.title = "Card";
  card.created_at = 1;
  card.updated_at = 1;
  store.create(card, "Mentions #android in prose.");

  REQUIRE(store.add_tag(card.card_id, "Android", 2) == holder::card::AddTagResult::AlreadyPresent);
  REQUIRE(store.get_content(store.get(card.card_id).value()).value() == "Mentions #android in prose.");
}

TEST_CASE("CardStore add_tag rejects an invalid tag without changing the card", "[cardstore][tags]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);

  holder::model::Card card;
  card.card_id = "addtag3";
  card.project_id = "proj-1";
  card.title = "Card";
  card.created_at = 1;
  card.updated_at = 1;
  store.create(card, "Body");

  REQUIRE(store.add_tag(card.card_id, "123issue", 2) == holder::card::AddTagResult::InvalidTag);
  REQUIRE(store.get_content(store.get(card.card_id).value()).value() == "Body");
}

TEST_CASE("CardStore remove_tag removes from the trailing tag line and reindexes", "[cardstore][tags]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);
  holder::card::TagRepo tags(db);

  holder::model::Card card;
  card.card_id = "removetag1";
  card.project_id = "proj-1";
  card.title = "Card";
  card.created_at = 1;
  card.updated_at = 1;
  store.create(card, "Some useful thing\n\n#work #android");

  REQUIRE(store.remove_tag(card.card_id, "Work", 2) == holder::card::RemoveTagResult::Removed);
  REQUIRE(store.get_content(store.get(card.card_id).value()).value() == "Some useful thing\n\n#android");
  REQUIRE(tags.list_tags_for_card("proj-1", card.card_id) == std::vector<std::string>{"android"});

  REQUIRE(store.remove_tag(card.card_id, "android", 3) == holder::card::RemoveTagResult::Removed);
  REQUIRE(store.get_content(store.get(card.card_id).value()).value() == "Some useful thing");
  REQUIRE(tags.list_tags_for_card("proj-1", card.card_id).empty());
}

TEST_CASE("CardStore remove_tag distinguishes not-present from prose-only", "[cardstore][tags]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);

  holder::model::Card card;
  card.card_id = "removetag2";
  card.project_id = "proj-1";
  card.title = "Card";
  card.created_at = 1;
  card.updated_at = 1;
  store.create(card, "Mentions #android only in prose.");

  REQUIRE(
      store.remove_tag(card.card_id, "android", 2) ==
      holder::card::RemoveTagResult::PresentOutsideEditableTagLine
  );
  REQUIRE(
      store.get_content(store.get(card.card_id).value()).value() == "Mentions #android only in prose."
  );

  REQUIRE(store.remove_tag(card.card_id, "nonexistent", 3) == holder::card::RemoveTagResult::NotPresent);
}

TEST_CASE("CardStore remove_tag rejects an invalid tag without changing the card", "[cardstore][tags]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);

  holder::model::Card card;
  card.card_id = "removetag3";
  card.project_id = "proj-1";
  card.title = "Card";
  card.created_at = 1;
  card.updated_at = 1;
  store.create(card, "Body\n\n#work");

  REQUIRE(store.remove_tag(card.card_id, "123issue", 2) == holder::card::RemoveTagResult::InvalidTag);
  REQUIRE(store.get_content(store.get(card.card_id).value()).value() == "Body\n\n#work");
}

TEST_CASE("CardStore list_editable_tags returns only the trailing-line tags", "[cardstore][tags]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);

  holder::model::Card card;
  card.card_id = "editabletags1";
  card.project_id = "proj-1";
  card.title = "Card";
  card.created_at = 1;
  card.updated_at = 1;
  store.create(card, "Mentions #prose here.\n\n#work #android");

  REQUIRE(store.list_editable_tags(card.card_id) == std::vector<std::string>{"work", "android"});
}

TEST_CASE("CardStore list_editable_tags is empty when there's no trailing tag line", "[cardstore][tags]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);

  holder::model::Card card;
  card.card_id = "editabletags2";
  card.project_id = "proj-1";
  card.title = "Card";
  card.created_at = 1;
  card.updated_at = 1;
  store.create(card, "Mentions #prose here, nothing else.");

  REQUIRE(store.list_editable_tags(card.card_id).empty());
}

TEST_CASE("CardStore list_editable_tags reflects add_tag/remove_tag", "[cardstore][tags]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);

  holder::model::Card card;
  card.card_id = "editabletags3";
  card.project_id = "proj-1";
  card.title = "Card";
  card.created_at = 1;
  card.updated_at = 1;
  store.create(card, "Body");

  REQUIRE(store.list_editable_tags(card.card_id).empty());

  store.add_tag(card.card_id, "android", 2);
  REQUIRE(store.list_editable_tags(card.card_id) == std::vector<std::string>{"android"});

  store.remove_tag(card.card_id, "android", 3);
  REQUIRE(store.list_editable_tags(card.card_id).empty());
}

TEST_CASE("CardStore tag and historical restore methods reject missing cards",
          "[cardstore]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);

  REQUIRE_THROWS_WITH(
      store.restore_version("missing-card", "historical-oid", 1),
      Catch::Matchers::ContainsSubstring("card not found: missing-card")
  );
  REQUIRE_THROWS_WITH(
      store.add_tag("missing-card", "todo", 1),
      Catch::Matchers::ContainsSubstring("card not found: missing-card")
  );
  REQUIRE_THROWS_WITH(
      store.remove_tag("missing-card", "todo", 1),
      Catch::Matchers::ContainsSubstring("card not found: missing-card")
  );
  REQUIRE_THROWS_WITH(
      store.list_editable_tags("missing-card"),
      Catch::Matchers::ContainsSubstring("card not found: missing-card")
  );
}

TEST_CASE("CardStore rejects missing milestone content and malformed historical blobs",
          "[cardstore][history]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-history-errors", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);
  holder::card::CardRepo cards(db);

  holder::model::Card missing_content;
  missing_content.card_id = "missingcontent01";
  missing_content.project_id = "proj-history-errors";
  missing_content.title = "Missing";
  missing_content.rel_path = holder::core::card_rel_path(missing_content.card_id);
  missing_content.created_at = 1;
  missing_content.updated_at = 1;
  cards.create(missing_content);
  REQUIRE_THROWS_WITH(store.update_milestones(missing_content.card_id, 2), "card content missing");

  holder::model::Card bad_path = missing_content;
  bad_path.card_id = "badrestore01";
  bad_path.rel_path = "cards/wrong.md";
  cards.create(bad_path);
  REQUIRE_THROWS_WITH(
      store.restore_version(bad_path.card_id, "unused-oid", 2),
      Catch::Matchers::ContainsSubstring("card rel_path does not match card_id")
  );

  holder::model::Card card;
  card.card_id = "badblob01";
  card.project_id = "proj-history-errors";
  card.title = "Current";
  card.created_at = 1;
  card.updated_at = 1;
  store.create(card, "current body");

  holder::git::GitRepo repo;
  repo.open_existing(project_root);
  const auto path = holder::core::card_rel_path(card.card_id);
  const std::string binary("binary\0body", 11);
  repo.write_file(path, binary);
  repo.stage_path(path);
  repo.commit("Binary historical blob");
  const auto binary_oid = repo.head_oid();
  REQUIRE(binary_oid.has_value());

  repo.write_file(path, "not front matter");
  repo.stage_path(path);
  repo.commit("Malformed historical blob");
  const auto malformed_oid = repo.head_oid();
  REQUIRE(malformed_oid.has_value());

  const auto current = cards.get(card.card_id);
  REQUIRE(current.has_value());
  repo.write_file(path, holder::core::render_card_front_matter(*current, {}, {}) + "current body");
  repo.stage_path(path);
  repo.commit("Restore valid current blob");

  REQUIRE_THROWS_WITH(
      store.restore_version(card.card_id, *binary_oid, 3),
      Catch::Matchers::ContainsSubstring("historical card content is binary")
  );
  REQUIRE_THROWS_WITH(
      store.restore_version(card.card_id, *malformed_oid, 3),
      Catch::Matchers::ContainsSubstring("historical card content is malformed")
  );
}

TEST_CASE("CardStore mutations reject a bad rel_path or a missing durable file",
          "[cardstore]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);
  holder::card::CardRepo cards(db);
  holder::card::MilestoneRepo milestones(db);
  constexpr auto kBadRel = "card rel_path does not match card_id";
  constexpr auto kMissing = "card content missing";

  // A card row whose rel_path disagrees with its card_id, carrying a milestone so the
  // update passes every earlier check and reaches the rel_path guard.
  holder::model::Card bad_rel;
  bad_rel.card_id = "msbad01";
  bad_rel.project_id = "proj-1";
  bad_rel.rel_path = "cards/wrong.md";
  bad_rel.title = "BadRel";
  bad_rel.created_at = 1;
  bad_rel.updated_at = 1;
  cards.create(bad_rel);

  holder::model::Card missing_file;
  missing_file.card_id = "msmis01";
  missing_file.project_id = "proj-1";
  missing_file.title = "MissingFile";
  missing_file.created_at = 1;
  missing_file.updated_at = 1;
  store.create(missing_file, "body");

  holder::model::Milestone bad_milestone;
  bad_milestone.milestone_id = "ms-bad";
  bad_milestone.card_id = bad_rel.card_id;
  bad_milestone.project_id = "proj-1";
  bad_milestone.start_at = 100;
  bad_milestone.created_at = 1;
  bad_milestone.updated_at = 1;
  milestones.replace_for_card("proj-1", bad_rel.card_id, {bad_milestone});

  holder::model::Milestone missing_milestone = bad_milestone;
  missing_milestone.milestone_id = "ms-missing";
  missing_milestone.card_id = missing_file.card_id;
  milestones.replace_for_card("proj-1", missing_file.card_id, {missing_milestone});

  holder::card::MilestoneUpdate update;
  update.start_at = 200;

  REQUIRE_THROWS_WITH(
      store.update_milestone("proj-1", bad_rel.card_id, bad_milestone.milestone_id, update, 2), kBadRel
  );

  // Removing the durable file behind a live row (an external delete) must fail loudly rather
  // than recreate or silently skip the card.
  std::filesystem::remove(project_root / holder::core::card_rel_path(missing_file.card_id));
  REQUIRE_THROWS_WITH(
      store.update_milestone("proj-1", missing_file.card_id, missing_milestone.milestone_id, update, 2),
      kMissing
  );
  REQUIRE_THROWS_WITH(store.update_links(missing_file.card_id, 2), kMissing);
}
