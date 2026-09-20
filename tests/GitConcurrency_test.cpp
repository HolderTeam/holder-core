#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "card/CardPaths.h"
#include "card/CardRepo.h"
#include "card/CardStore.h"
#include "core_test_helpers.h"
#include "git/GitRepo.h"
#include "index/FtsIndexer.h"
#include "model/Card.h"
#include "model/Project.h"
#include "platform/Db.h"
#include "project/ProjectRepo.h"

#include <git2.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Regression coverage for the RealGitOps/GitRepo crash: concurrent C-API-shaped
// operations used to share one process-global RealGitOps (and therefore one
// mutable git_repository*), so two simultaneous open_or_init() calls could free
// a handle the other thread was using. These tests drive CardStore exactly the
// way the C API does -- a fresh, git-argument-less store per operation, so each
// operation owns its own RealGitOps -- and hammer overlapping operations hard
// enough to surface a lifetime/race regression. Run under ThreadSanitizer with
// -DHOLDER_CORE_SANITIZE=thread.

namespace {

namespace fs = std::filesystem;

fs::path find_schema_sql() {
#ifdef SCHEMA_SQL_PATH
  fs::path p = SCHEMA_SQL_PATH;
  if (fs::exists(p)) return p;
#endif
  fs::path p1 = fs::current_path() / "schema" / "schema.sql";
  if (fs::exists(p1)) return p1;
  fs::path p2 = fs::current_path().parent_path() / "schema" / "schema.sql";
  if (fs::exists(p2)) return p2;
  throw std::runtime_error("schema.sql not found for tests");
}

std::string schema_sql() {
  std::ifstream in(find_schema_sql());
  REQUIRE(in.is_open());
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

fs::path make_temp_dir() {
  const auto base = fs::temp_directory_path();
  const auto suffix = std::to_string(
      static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())
  );
  static std::atomic<unsigned long long> counter{0};
  auto dir = base /
             ("holder_git_concurrency_" + suffix + "_" + std::to_string(counter.fetch_add(1)));
  fs::create_directories(dir);
  return dir;
}

std::unique_ptr<holder::platform::Db> open_db(const fs::path& db_path, const std::string& sql) {
  auto db = std::make_unique<holder::platform::Db>();
  db->open(db_path);
  db->exec(sql);
  return db;
}

void seed_project(holder::platform::Db& db, const std::string& project_id, const fs::path& root) {
  holder::project::ProjectRepo repo(db);
  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Concurrency";
  project.root_path = root.string();
  project.privacy_mode = "plain";
  project.created_at = 1;
  project.updated_at = 1;
  repo.create(project);
}

holder::model::Card make_card(const std::string& card_id, const std::string& project_id, int n) {
  holder::model::Card card;
  card.card_id = card_id;
  card.project_id = project_id;
  card.title = "Card " + std::to_string(n);
  card.created_at = 10 + n;
  card.updated_at = 10 + n;
  return card;
}

// Fails the test from any thread; Catch2's REQUIRE is not thread-safe.
struct ErrorSink {
  std::mutex mutex;
  std::vector<std::string> errors;

  void record(const std::string& what) {
    std::lock_guard<std::mutex> lock(mutex);
    errors.push_back(what);
  }

  void require_empty() {
    std::lock_guard<std::mutex> lock(mutex);
    if (!errors.empty()) {
      FAIL("worker thread error: " << errors.front() << " (" << errors.size() << " total)");
    }
  }
};

} // namespace

TEST_CASE(
    "concurrent CardStore operations on one project keep the Git repo intact",
    "[concurrency][git]"
) {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  const auto project_root = dir / "repo";
  const std::string project_id = "proj-shared";
  const std::string sql = schema_sql();

  constexpr int kCards = 6;
  constexpr int kThreads = 8;
  constexpr int kIterations = 120;

  {
    auto setup_db = open_db(db_path, sql);
    seed_project(*setup_db, project_id, project_root);
    holder::index::FtsIndexer fts(*setup_db);
    holder::card::CardStore store(*setup_db, &fts);
    for (int i = 0; i < kCards; ++i) {
      store.create(
          make_card("cccard" + std::to_string(i), project_id, i),
          "seed body " + std::to_string(i)
      );
    }
  }

  // One connection per worker, all against the same project + repo.
  std::vector<std::unique_ptr<holder::platform::Db>> dbs;
  std::vector<std::unique_ptr<holder::index::FtsIndexer>> ftses;
  for (int t = 0; t < kThreads; ++t) {
    dbs.push_back(open_db(db_path, sql));
    ftses.push_back(std::make_unique<holder::index::FtsIndexer>(*dbs.back()));
  }

  ErrorSink sink;
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t]() {
      while (!go.load()) {
        std::this_thread::yield();
      }
      try {
        for (int i = 0; i < kIterations; ++i) {
          holder::platform::Db& db = *dbs[t];
          holder::index::FtsIndexer& fts = *ftses[t];
          const std::string card_id = "cccard" + std::to_string((t + i) % kCards);
          if (t % 2 == 0) {
            // Reader: the get_content path implicated in the Android crashes.
            holder::card::CardStore store(db, &fts);
            holder::card::CardRepo repo(db);
            const auto card = repo.get(card_id);
            if (!card.has_value()) {
              sink.record("card vanished: " + card_id);
              return;
            }
            const auto body = store.get_content(card.value());
            if (!body.has_value() || body->empty()) {
              sink.record("empty content for " + card_id);
              return;
            }
          } else {
            // Writer: a full write + Git commit on the shared repo.
            holder::card::CardStore store(db, &fts);
            store.update_content(
                card_id,
                "body t" + std::to_string(t) + " i" + std::to_string(i),
                std::nullopt,
                1000 + t * 1000 + i
            );
          }
        }
      } catch (const std::exception& e) {
        sink.record(e.what());
      } catch (...) {
        sink.record("non-std exception");
      }
    });
  }
  go.store(true);
  for (auto& thread : threads) {
    thread.join();
  }
  sink.require_empty();

  // The repository must still open, have a valid HEAD, and every card must still
  // parse back to a non-empty body.
  holder::git::GitRepo repo;
  repo.open_existing(project_root);
  REQUIRE(repo.head_oid().has_value());

  auto verify_db = open_db(db_path, sql);
  holder::index::FtsIndexer verify_fts(*verify_db);
  holder::card::CardStore verify_store(*verify_db, &verify_fts);
  holder::card::CardRepo verify_repo(*verify_db);
  for (int i = 0; i < kCards; ++i) {
    const auto card = verify_repo.get("cccard" + std::to_string(i));
    REQUIRE(card.has_value());
    const auto body = verify_store.get_content(card.value());
    REQUIRE(body.has_value());
    REQUIRE_FALSE(body->empty());
  }
}

TEST_CASE(
    "concurrent get_content and card writes on the same card never crash or throw",
    "[concurrency][git]"
) {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  const auto project_root = dir / "repo";
  const std::string project_id = "proj-hot";
  const std::string sql = schema_sql();
  const std::string card_id = "hotcard1";

  {
    auto setup_db = open_db(db_path, sql);
    seed_project(*setup_db, project_id, project_root);
    holder::index::FtsIndexer fts(*setup_db);
    holder::card::CardStore store(*setup_db, &fts);
    store.create(make_card(card_id, project_id, 0), "initial body");
  }

  constexpr int kReaders = 6;
  constexpr int kWriters = 2;
  constexpr int kReaderIters = 300;
  constexpr int kWriterIters = 120;

  std::vector<std::unique_ptr<holder::platform::Db>> dbs;
  std::vector<std::unique_ptr<holder::index::FtsIndexer>> ftses;
  for (int t = 0; t < kReaders + kWriters; ++t) {
    dbs.push_back(open_db(db_path, sql));
    ftses.push_back(std::make_unique<holder::index::FtsIndexer>(*dbs.back()));
  }

  ErrorSink sink;
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  for (int t = 0; t < kReaders + kWriters; ++t) {
    const bool is_writer = t >= kReaders;
    threads.emplace_back([&, t, is_writer]() {
      while (!go.load()) {
        std::this_thread::yield();
      }
      try {
        const int iters = is_writer ? kWriterIters : kReaderIters;
        for (int i = 0; i < iters; ++i) {
          holder::card::CardStore store(*dbs[t], ftses[t].get());
          if (is_writer) {
            store.update_content(
                card_id,
                "rewrite " + std::to_string(t) + "/" + std::to_string(i),
                std::nullopt,
                5000 + t * 1000 + i
            );
          } else {
            holder::card::CardRepo repo(*dbs[t]);
            const auto card = repo.get(card_id);
            if (!card.has_value()) {
              sink.record("hot card vanished");
              return;
            }
            const auto body = store.get_content(card.value());
            const bool recognizable = body.has_value() &&
                                      (body->find("body") != std::string::npos ||
                                       body->find("rewrite") != std::string::npos);
            if (!recognizable) {
              sink.record("hot card body torn: '" + body.value_or("<null>") + "'");
              return;
            }
          }
        }
      } catch (const std::exception& e) {
        sink.record(e.what());
      } catch (...) {
        sink.record("non-std exception");
      }
    });
  }
  go.store(true);
  for (auto& thread : threads) {
    thread.join();
  }
  sink.require_empty();
}

TEST_CASE(
    "per-project locks let independent projects run Git operations concurrently",
    "[concurrency][git]"
) {
  // If holder-core serialized all Git work behind one process-wide lock, only
  // one thread could hold a project open at a time. Each thread opens its own
  // project (which acquires that project's lock for the CardStore's lifetime)
  // and then waits on a barrier; the barrier can only complete if the locks are
  // independent.
  // spdlog's system-library singleton uses a C++ static guard, whose fast path
  // is invisible to TSan in an uninstrumented shared library. Initialize it on
  // this thread so worker creation establishes an observable happens-before
  // edge even when this is the first test in a randomized run.
  REQUIRE(spdlog::default_logger() != nullptr);
  constexpr int kProjects = 6;
  const std::string sql = schema_sql();

  struct Fixture {
    fs::path dir;
    std::unique_ptr<holder::platform::Db> db;
    std::unique_ptr<holder::index::FtsIndexer> fts;
    std::string project_id;
    fs::path project_root;
  };
  std::vector<Fixture> fixtures(kProjects);
  for (int i = 0; i < kProjects; ++i) {
    auto& f = fixtures[i];
    f.dir = make_temp_dir();
    f.db = open_db(f.dir / "holder.db", sql);
    f.fts = std::make_unique<holder::index::FtsIndexer>(*f.db);
    f.project_id = "proj-" + std::to_string(i);
    f.project_root = f.dir / "repo";
    seed_project(*f.db, f.project_id, f.project_root);
  }

  std::mutex mutex;
  std::condition_variable cv;
  int inside = 0;
  bool released = false;

  ErrorSink sink;
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  for (int i = 0; i < kProjects; ++i) {
    threads.emplace_back([&, i]() {
      while (!go.load()) {
        std::this_thread::yield();
      }
      try {
        auto& f = fixtures[i];
        // Constructing the store and creating a card takes this project's lock
        // and holds it while the store is alive.
        holder::card::CardStore store(*f.db, f.fts.get());
        store.create(make_card("prjcard" + std::to_string(i), f.project_id, i), "body");

        std::unique_lock<std::mutex> lock(mutex);
        if (++inside == kProjects) {
          released = true;
          cv.notify_all();
        } else {
          const bool ok = cv.wait_for(lock, std::chrono::seconds(10), [&]() {
            return released;
          });
          if (!ok) {
            sink.record("barrier timed out -- Git operations appear globally serialized");
          }
        }
      } catch (const std::exception& e) {
        sink.record(e.what());
      } catch (...) {
        sink.record("non-std exception");
      }
    });
  }
  go.store(true);
  for (auto& thread : threads) {
    thread.join();
  }
  sink.require_empty();
  REQUIRE(inside == kProjects);
  REQUIRE(released);
}
