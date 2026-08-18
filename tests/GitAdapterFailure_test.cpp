#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "ai/AiMessageRepo.h"
#include "ai/AiThreadRepo.h"
#include "card/CardRepo.h"
#include "card/CardStore.h"
#include "git/GitOps.h"
#include "index/FtsIndexer.h"
#include "model/AiMessage.h"
#include "model/AiThread.h"
#include "model/Card.h"
#include "model/Project.h"
#include "platform/Db.h"
#include "project/ProjectRepo.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

std::filesystem::path make_temp_dir() {
  const auto base = std::filesystem::temp_directory_path();
  const auto suffix = std::to_string(
      static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())
  );
  auto dir = base / ("holder_git_adapter_test_" + suffix);
  std::filesystem::create_directories(dir);
  return dir;
}

void apply_schema(holder::platform::Db& db) {
  std::filesystem::path schema_path = SCHEMA_SQL_PATH;
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

class FailingGitOps final : public holder::git::GitOps {
 public:
  std::filesystem::path repo_dir_;
  bool fail_commit = false;
  bool fail_stage = false;
  bool fail_set_remote = false;
  bool fail_remove_remote = false;
  bool fail_open = false;

  void open_or_init(const std::filesystem::path& repo_dir) override {
    if (fail_open) throw std::runtime_error("open failed");
    repo_dir_ = repo_dir;
    std::filesystem::create_directories(repo_dir_);
  }
  void write_file(const std::filesystem::path& relative_path, const std::string& content) override {
    std::filesystem::path full = repo_dir_ / relative_path;
    std::filesystem::create_directories(full.parent_path());
    std::ofstream out(full, std::ios::binary);
    if (!out.is_open()) {
      throw std::runtime_error("write failed");
    }
    out << content;
  }
  void stage_path(const std::filesystem::path&) override {
    if (fail_stage) throw std::runtime_error("stage failed");
  }
  void remove_path(const std::filesystem::path&) override {}
  void commit(const std::string&) override {
    if (fail_commit) throw std::runtime_error("commit failed");
  }
  void set_remote(const std::string&, const std::string&) override {
    if (fail_set_remote) throw std::runtime_error("set remote failed");
  }
  void remove_remote(const std::string&) override {
    if (fail_remove_remote) throw std::runtime_error("remove remote failed");
  }
  void pull_remote_ff_only(const std::string&) override {}
  holder::git::RemoteProbeResult probe_remote(const std::string&) override {
    return {
        .status = holder::git::RemoteProbeStatus::Reachable,
        .remote_has_head = true,
        .error_message = {}
    };
  }
  holder::git::PushResult push_branch(const std::string&, const std::string&, bool) override {
    return {
        .status = holder::git::PushStatus::Pushed,
        .ahead_count = 0,
        .behind_count = 0,
        .error_message = {}
    };
  }
  std::filesystem::path repo_dir() const override { return repo_dir_; }
};

} // namespace

TEST_CASE("CardStore create propagates git commit failure after DB write", "[git]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  const auto project_root = dir / "repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  FailingGitOps git;
  git.fail_commit = true;
  holder::card::CardStore store(db, &fts, nullptr, &git);

  holder::model::Card card;
  card.card_id = "card-1";
  card.project_id = "proj-1";
  card.title = "Title";
  card.created_at = 1;
  card.updated_at = 1;

  REQUIRE_THROWS(store.create(card, "body"));

  holder::card::CardRepo repo(db);
  REQUIRE(repo.get("card-1").has_value());
}

TEST_CASE("AiMessageRepo append propagates git commit failure", "[git]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  const auto project_root = dir / "repo";
  create_project(db, "proj-1", project_root.string());

  holder::model::AiThread thread;
  thread.thread_id = "thread-1";
  thread.project_id = "proj-1";
  thread.title = "Thread";
  thread.created_at = 1;
  thread.updated_at = 1;
  holder::ai::AiThreadRepo thread_repo(db);
  thread_repo.create(thread);

  holder::index::FtsIndexer fts(db);
  FailingGitOps git;
  git.fail_commit = true;
  holder::ai::AiMessageRepo repo(db, &fts, nullptr, &git);

  holder::model::AiMessage msg;
  msg.message_id = "msg-1";
  msg.thread_id = "thread-1";
  msg.role = "user";
  msg.source = "manual";
  msg.content = "hello";
  msg.created_at = 2;

  REQUIRE_THROWS(repo.append(msg));
}

TEST_CASE("CardStore create propagates set_remote failure", "[git]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  const auto project_root = dir / "repo";
  create_project(db, "proj-1", project_root.string());
  holder::project::ProjectRepo project_repo(db);
  project_repo
      .update_git_remote("proj-1", std::optional<std::string>("git@example.com:repo.git"), 2);

  holder::index::FtsIndexer fts(db);
  FailingGitOps git;
  git.fail_set_remote = true;
  holder::card::CardStore store(db, &fts, nullptr, &git);

  holder::model::Card card;
  card.card_id = "card-1";
  card.project_id = "proj-1";
  card.title = "Title";
  card.created_at = 1;
  card.updated_at = 1;

  REQUIRE_THROWS(store.create(card, "body"));
}

TEST_CASE("CardStore update propagates git stage failure", "[git]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  const auto project_root = dir / "repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  FailingGitOps git;
  holder::card::CardStore store(db, &fts, nullptr, &git);

  holder::model::Card card;
  card.card_id = "card-2";
  card.project_id = "proj-1";
  card.title = "Title";
  card.created_at = 1;
  card.updated_at = 1;
  store.create(card, "body");

  git.fail_stage = true;
  REQUIRE_THROWS(store.update_content(card.card_id, "updated", std::nullopt, 2));
}

TEST_CASE("AiMessageRepo update propagates git stage failure", "[git]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);

  const auto project_root = dir / "repo";
  create_project(db, "proj-1", project_root.string());

  holder::model::AiThread thread;
  thread.thread_id = "thread-1";
  thread.project_id = "proj-1";
  thread.title = "Thread";
  thread.created_at = 1;
  thread.updated_at = 1;
  holder::ai::AiThreadRepo thread_repo(db);
  thread_repo.create(thread);

  holder::index::FtsIndexer fts(db);
  FailingGitOps git;
  holder::ai::AiMessageRepo repo(db, &fts, nullptr, &git);

  holder::model::AiMessage msg;
  msg.message_id = "msg-stage";
  msg.thread_id = "thread-1";
  msg.role = "user";
  msg.source = "manual";
  msg.content = "hello";
  msg.created_at = 2;
  repo.append(msg);

  msg.content = "changed";
  git.fail_stage = true;
  REQUIRE_THROWS(repo.update(msg));
}

// NOTE: Project git remote updates use the real GitRepo directly in Session,
// so we can't inject FailingGitOps there without adding another seam.
