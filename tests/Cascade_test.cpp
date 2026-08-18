#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "ai/AiMessageRepo.h"
#include "ai/AiThreadRepo.h"
#include "card/CardRepo.h"
#include "card/LinkRepo.h"
#include "index/FtsIndexer.h"
#include "model/AiMessage.h"
#include "model/AiThread.h"
#include "model/Card.h"
#include "model/CardLink.h"
#include "model/Project.h"
#include "model/Resource.h"
#include "platform/Db.h"
#include "project/ProjectRepo.h"
#include "resource/ResourceRepo.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

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
  auto dir = base / ("holder_cascade_test_" + suffix);
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

} // namespace

TEST_CASE("Deleting project cascades to dependent rows", "[cascade]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);

  holder::project::ProjectRepo project_repo(db);
  holder::card::CardRepo card_repo(db);
  holder::card::LinkRepo link_repo(db);
  holder::resource::ResourceRepo resource_repo(db);
  holder::ai::AiThreadRepo thread_repo(db);
  holder::index::FtsIndexer fts(db);
  holder::ai::AiMessageRepo message_repo(db, &fts);

  holder::model::Project project;
  project.project_id = "proj-1";
  project.name = "Project";
  project.root_path = (dir / "project_repo").string();
  project.created_at = 1;
  project.updated_at = 1;
  project_repo.create(project);

  holder::model::Card card_a;
  card_a.card_id = "card-a";
  card_a.project_id = "proj-1";
  card_a.title = "A";
  card_a.rel_path = "cards/a.md";
  card_a.sort_key = 0.0;
  card_a.created_at = 2;
  card_a.updated_at = 2;
  card_repo.create(card_a);

  holder::model::Card card_b = card_a;
  card_b.card_id = "card-b";
  card_b.title = "B";
  card_b.rel_path = "cards/b.md";
  card_repo.create(card_b);

  holder::model::CardLink link;
  link.project_id = "proj-1";
  link.from_card_id = "card-a";
  link.to_card_id = "card-b";
  link.to_type = "card";
  link.kind = "wiki";
  link.created_at = 3;
  link_repo.upsert_links("proj-1", "card-a", {link});

  holder::model::Resource resource;
  resource.resource_id = "res-1";
  resource.project_id = "proj-1";
  resource.kind = "url";
  resource.uri = "https://example.com";
  resource.label = "Example";
  resource.created_at = 4;
  resource.updated_at = 4;
  resource_repo.add(resource);

  holder::model::AiThread thread;
  thread.thread_id = "thread-1";
  thread.project_id = "proj-1";
  thread.title = "Thread";
  thread.created_at = 5;
  thread.updated_at = 5;
  thread_repo.create(thread);

  holder::model::AiMessage msg;
  msg.message_id = "msg-1";
  msg.thread_id = "thread-1";
  msg.role = "user";
  msg.source = "manual";
  msg.content = "Hello";
  msg.created_at = 6;
  message_repo.append(msg);

  project_repo.remove("proj-1");

  REQUIRE(card_repo.list_all("proj-1").empty());
  REQUIRE(link_repo.list_outgoing("proj-1", "card-a").empty());
  REQUIRE(resource_repo.list("proj-1").empty());
  REQUIRE(thread_repo.list("proj-1").empty());
  REQUIRE(message_repo.list_by_thread("thread-1").empty());
}
