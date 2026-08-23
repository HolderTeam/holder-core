#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "card/CardFrontMatter.h"
#include "card/CardPaths.h"
#include "card/CardRepo.h"
#include "card/CardStore.h"
#include "card/TagRepo.h"
#include "git/GitOps.h"
#include "git/GitRepo.h"
#include "core_test_helpers.h"
#include "index/FtsIndexer.h"
#include "model/Card.h"
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
    const std::string& root_path
) {
  holder::project::ProjectRepo repo(db);
  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Project";
  project.root_path = root_path;
  project.privacy_mode = "plain";
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
  REQUIRE_THROWS(store.create(card, "two"));
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

  REQUIRE_THROWS(store.create(card, "one"));
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

  REQUIRE_THROWS(store.create(card, "x"));
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

  REQUIRE_THROWS(store.create(card, "x"));
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

  REQUIRE_THROWS(store.create(card, "x"));
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
  REQUIRE_THROWS(store.create(card, "secret"));

  holder::card::CardRepo card_repo(db);
  holder::model::Card inserted = card;
  inserted.rel_path = holder::core::card_rel_path(inserted.card_id);
  inserted.sort_key = 0.0;
  card_repo.create(inserted);
  holder::git::GitRepo repo;
  repo.open_or_init(project_root);
  repo.write_file(inserted.rel_path, "HolderPriv1\nbad");

  REQUIRE_THROWS((void)store.get_content(inserted));
}

TEST_CASE("CardStore update_content throws when card is missing", "[cardstore]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  create_project(db, "proj-1", (dir / "project_repo").string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore store(db, &fts);
  REQUIRE_THROWS(store.update_content("missing", "x", std::nullopt, 2));
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

  REQUIRE_THROWS(store.move("missing", false, std::nullopt, std::nullopt, 2));

  holder::model::Card bad_rel;
  bad_rel.card_id = "movbad01";
  bad_rel.project_id = "proj-1";
  bad_rel.rel_path = "cards/wrong.md";
  bad_rel.title = "Bad";
  bad_rel.sort_key = 0.0;
  bad_rel.created_at = 1;
  bad_rel.updated_at = 1;
  card_repo.create(bad_rel);
  REQUIRE_THROWS(store.move(bad_rel.card_id, false, std::nullopt, std::nullopt, 2));

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
  REQUIRE_THROWS(
      store.move(missing_body.card_id, true, std::optional<std::string>("parentx"), std::nullopt, 2)
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

  REQUIRE_THROWS(store.update_links("missing", 2));

  holder::model::Card bad_rel;
  bad_rel.card_id = "lnkbad01";
  bad_rel.project_id = "proj-1";
  bad_rel.rel_path = "cards/wrong.md";
  bad_rel.title = "Bad";
  bad_rel.sort_key = 0.0;
  bad_rel.created_at = 1;
  bad_rel.updated_at = 1;
  card_repo.create(bad_rel);
  REQUIRE_THROWS(store.update_links(bad_rel.card_id, 2));

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

  REQUIRE_THROWS(store.trash("missing", 2));
  REQUIRE_THROWS(store.restore("missing", 2));
  REQUIRE_THROWS(store.hard_delete("missing"));

  holder::model::Card active;
  active.card_id = "trashg01";
  active.project_id = "proj-1";
  active.title = "Active";
  active.created_at = 1;
  active.updated_at = 1;
  store.create(active, "body");
  REQUIRE_THROWS(store.restore(active.card_id, 2));
  REQUIRE_THROWS(store.hard_delete(active.card_id));

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
  REQUIRE_THROWS(store.trash(bad_rel.card_id, 2));

  holder::model::Card deleted_bad_rel = bad_rel;
  deleted_bad_rel.card_id = "restbad1";
  deleted_bad_rel.rel_path = "cards/another-wrong.md";
  deleted_bad_rel.deleted_at = 10;
  card_repo.create(deleted_bad_rel);
  REQUIRE_THROWS(store.trash(deleted_bad_rel.card_id, 2));
  REQUIRE_THROWS(store.restore(deleted_bad_rel.card_id, 2));

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
  REQUIRE_THROWS((void)store.get_content(bad_content));

  holder::model::Card no_project = content_missing;
  no_project.card_id = "contprj1";
  no_project.project_id = "missing-project";
  no_project.rel_path = holder::core::card_rel_path(no_project.card_id);
  no_project.title = "NoProject";
  REQUIRE_THROWS((void)store.get_content(no_project));
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
