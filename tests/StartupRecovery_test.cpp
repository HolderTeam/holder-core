#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "card/CardRepo.h"
#include "card/CardStore.h"
#include "git/GitOps.h"
#include "core_test_helpers.h"
#include "index/FtsIndexer.h"
#include "model/Card.h"
#include "model/Project.h"
#include "privacy/ProjectPrivacy.h"
#include "project/Rebuilder.h"
#include "project/ProjectRepo.h"
#include "project/StartupRecovery.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <sstream>

namespace {

std::vector<std::string> split_lines3_sr(const std::string& envelope) {
  std::istringstream in(envelope);
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) {
    lines.push_back(line);
  }
  REQUIRE(lines.size() >= 3);
  return {lines[0], lines[1], lines[2]};
}

std::string join_lines3_sr(const std::vector<std::string>& lines) {
  REQUIRE(lines.size() == 3);
  return lines[0] + "\n" + lines[1] + "\n" + lines[2] + "\n";
}

} // namespace

TEST_CASE("Startup recovery returns empty when projects root is missing", "[startup][recovery]") {
  const auto dir = holder::test::make_temp_dir();
  const auto recovered_db_path = dir / "recovered.db";
  const auto projects_root = dir / "does-not-exist";

  auto recovered_db = holder::test::open_db_with_schema(recovered_db_path);
  holder::index::FtsIndexer recovered_fts(recovered_db);
  const auto recovered = holder::project::recover_projects_from_disk(
      recovered_db,
      &recovered_fts,
      projects_root,
      []() {
        return std::string("unused");
      }
  );

  REQUIRE(recovered.empty());
}

TEST_CASE("Rebuilder throws when project root is missing", "[startup][recovery]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::store::Rebuilder rebuilder(db, nullptr, nullptr, false);

  holder::model::Project project;
  project.project_id = "proj-missing-root";
  project.name = "Missing";
  project.root_path = (dir / "missing").string();
  project.created_at = 1;
  project.updated_at = 1;

  REQUIRE_THROWS(rebuilder.rebuild_project(project));
}

TEST_CASE("Rebuilder rejects card front matter without card id", "[startup][recovery]") {
  const auto dir = holder::test::make_temp_dir();
  const auto root = dir / "project";
  std::filesystem::create_directories(root / "cards" / "ab" / "cd");
  {
    std::ofstream out(
        root / "cards" / "ab" / "cd" / "abcd1234.md",
        std::ios::binary | std::ios::trunc
    );
    REQUIRE(out.is_open());
    out << "---\ntitle: Missing Id\n---\n# Missing Id\n";
  }

  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::store::Rebuilder rebuilder(db, nullptr, nullptr, false);
  holder::model::Project project;
  project.project_id = "proj-card-missing-id";
  project.name = "Missing Card Id";
  project.root_path = root.string();
  project.created_at = 1;
  project.updated_at = 1;

  REQUIRE_THROWS(rebuilder.rebuild_project(project));
}

TEST_CASE("Rebuilder rejects ai message front matter without message id", "[startup][recovery]") {
  const auto dir = holder::test::make_temp_dir();
  const auto root = dir / "project";
  std::filesystem::create_directories(root / "ai_messages" / "ab" / "cd");
  {
    std::ofstream out(
        root / "ai_messages" / "ab" / "cd" / "abcd1234.md",
        std::ios::binary | std::ios::trunc
    );
    REQUIRE(out.is_open());
    out << "---\nthread_id: thread-1\nrole: assistant\nsource: manual\n---\nhello\n";
  }

  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::store::Rebuilder rebuilder(db, nullptr, nullptr, false);
  holder::model::Project project;
  project.project_id = "proj-message-missing-id";
  project.name = "Missing Message Id";
  project.root_path = root.string();
  project.created_at = 1;
  project.updated_at = 1;

  REQUIRE_THROWS(rebuilder.rebuild_project(project));
}

TEST_CASE(
    "Startup recovery rebuilds projects and cards from existing project roots",
    "[startup][recovery]"
) {
  const auto dir = holder::test::make_temp_dir();
  const auto projects_root = dir / "projects";
  const auto original_db_path = dir / "original.db";
  const auto recovered_db_path = dir / "recovered.db";
  const auto keystore_dir = dir / "keystore";
  std::filesystem::create_directories(projects_root);
  std::filesystem::create_directories(keystore_dir);
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", keystore_dir.string());

  auto original_db = holder::test::open_db_with_schema(original_db_path);
  holder::project::ProjectRepo original_project_repo(original_db);
  holder::model::Project project;
  project.project_id = "proj-1";
  project.name = "Home";
  project.root_path = (projects_root / "home").string();
  project.privacy_mode = "encrypted_git";
  project.created_at = 1;
  project.updated_at = 1;
  original_project_repo.create(project);

  holder::git::RealGitOps git;
  holder::privacy::ensure_encrypted_project_ready(
      git,
      original_project_repo,
      project.project_id,
      project.root_path,
      project.project_key_id,
      project.updated_at,
      []() {
        return std::string("generated-key");
      }
  );

  holder::index::FtsIndexer original_fts(original_db);
  holder::card::CardStore original_store(original_db, &original_fts);
  holder::model::Card card;
  card.card_id = "card-1";
  card.project_id = project.project_id;
  card.title = "Welcome to Holder";
  card.created_at = 1;
  card.updated_at = 1;
  original_store.create(card, "# Welcome to Holder\n\nRecovered body.\n");

  holder::model::Card child;
  child.card_id = "card-2";
  child.project_id = project.project_id;
  child.title = "Child";
  child.parent_card_id = card.card_id;
  child.created_at = 2;
  child.updated_at = 2;
  original_store.create(child, "# Child\n\nNested body.\n");

  auto recovered_db = holder::test::open_db_with_schema(recovered_db_path);
  holder::index::FtsIndexer recovered_fts(recovered_db);
  const auto recovered = holder::project::recover_projects_from_disk(
      recovered_db,
      &recovered_fts,
      projects_root,
      []() {
        return std::string("recovered-proj");
      }
  );

  REQUIRE(recovered.size() == 1);
  REQUIRE(recovered[0].project_id == "proj-1");
  REQUIRE(recovered[0].name == "Home");
  REQUIRE(recovered[0].root_path == (projects_root / "home").string());
  REQUIRE(recovered[0].privacy_mode == "encrypted_git");
  REQUIRE(recovered[0].project_key_id.has_value());

  holder::project::ProjectRepo recovered_project_repo(recovered_db);
  holder::card::CardRepo recovered_card_repo(recovered_db);
  const auto projects = recovered_project_repo.list();
  REQUIRE(projects.size() == 1);
  REQUIRE(projects[0].project_id == "proj-1");
  const auto cards = recovered_card_repo.list_all(projects[0].project_id);
  REQUIRE(cards.size() == 2);

  holder::card::CardStore recovered_store(recovered_db, &recovered_fts);
  bool saw_parent = false;
  bool saw_child = false;
  for (const auto& rebuilt : cards) {
    const auto content = recovered_store.get_content(rebuilt);
    REQUIRE(content.has_value());
    if (rebuilt.card_id == "card-1") {
      saw_parent = true;
      REQUIRE(rebuilt.title == "Welcome to Holder");
      REQUIRE(content.value() == "# Welcome to Holder\n\nRecovered body.\n");
    }
    if (rebuilt.card_id == "card-2") {
      saw_child = true;
      REQUIRE(rebuilt.parent_card_id.has_value());
      REQUIRE(rebuilt.parent_card_id.value() == "card-1");
      REQUIRE(content.value() == "# Child\n\nNested body.\n");
    }
  }
  REQUIRE(saw_parent);
  REQUIRE(saw_child);
}

TEST_CASE(
    "Startup recovery defaults privacy metadata branches when files are missing fields",
    "[startup][recovery]"
) {
  const auto dir = holder::test::make_temp_dir();
  const auto projects_root = dir / "projects";
  const auto recovered_db_path = dir / "recovered.db";
  const auto plain_root = projects_root / "plain-no-metadata";
  const auto no_key_root = projects_root / "encrypted-no-key-id";

  std::filesystem::create_directories(plain_root / "cards" / "ab" / "cd");
  std::filesystem::create_directories(no_key_root / "cards" / "de" / "f0");
  std::filesystem::create_directories(no_key_root / ".holder");

  {
    std::ofstream card(
        plain_root / "cards" / "ab" / "cd" / "abcd1234.md",
        std::ios::binary | std::ios::trunc
    );
    REQUIRE(card.is_open());
    card << "# Plain no metadata\n\nRecovered.\n";
  }
  {
    std::ofstream privacy(
        no_key_root / ".holder" / "privacy.json",
        std::ios::binary | std::ios::trunc
    );
    REQUIRE(privacy.is_open());
    privacy << R"({"version":1,"project_id":"proj-nokey","mode":"encrypted_git"})";
  }
  {
    std::ofstream card(
        no_key_root / "cards" / "de" / "f0" / "def01234.md",
        std::ios::binary | std::ios::trunc
    );
    REQUIRE(card.is_open());
    card << "# Plain from encrypted metadata\n\nRecovered.\n";
  }

  auto recovered_db = holder::test::open_db_with_schema(recovered_db_path);
  holder::index::FtsIndexer recovered_fts(recovered_db);
  const auto recovered = holder::project::recover_projects_from_disk(
      recovered_db,
      &recovered_fts,
      projects_root,
      []() {
        return std::string("generated-recovery-id");
      }
  );

  REQUIRE(recovered.size() == 2);

  holder::project::ProjectRepo recovered_project_repo(recovered_db);
  const auto projects = recovered_project_repo.list();
  REQUIRE(projects.size() == 2);

  const auto plain = std::find_if(projects.begin(), projects.end(), [&](const auto& p) {
    return p.root_path == plain_root.string();
  });
  REQUIRE(plain != projects.end());
  REQUIRE(plain->privacy_mode == "plain");
  REQUIRE_FALSE(plain->project_key_id.has_value());

  const auto no_key = std::find_if(projects.begin(), projects.end(), [&](const auto& p) {
    return p.root_path == no_key_root.string();
  });
  REQUIRE(no_key != projects.end());
  REQUIRE(no_key->project_id == "proj-nokey");
  REQUIRE(no_key->privacy_mode == "encrypted_git");
  REQUIRE_FALSE(no_key->project_key_id.has_value());
}

TEST_CASE(
    "Startup recovery falls back to plain when privacy metadata is stale",
    "[startup][recovery]"
) {
  const auto dir = holder::test::make_temp_dir();
  const auto projects_root = dir / "projects";
  const auto recovered_db_path = dir / "recovered.db";
  const auto project_root = projects_root / "plain-project";
  std::filesystem::create_directories(project_root / "cards" / "ab" / "cd");
  std::filesystem::create_directories(project_root / ".holder");

  {
    std::ofstream privacy(
        project_root / ".holder" / "privacy.json",
        std::ios::binary | std::ios::trunc
    );
    REQUIRE(privacy.is_open());
    privacy
        << R"({"version":1,"project_id":"proj-plain","key_id":"stale-key","mode":"encrypted_git"})";
  }
  {
    std::ofstream card(
        project_root / "cards" / "ab" / "cd" / "abcd1234.md",
        std::ios::binary | std::ios::trunc
    );
    REQUIRE(card.is_open());
    card << "# Plain card\n\nRecovered as plain.\n";
  }

  auto recovered_db = holder::test::open_db_with_schema(recovered_db_path);
  holder::index::FtsIndexer recovered_fts(recovered_db);
  const auto recovered = holder::project::recover_projects_from_disk(
      recovered_db,
      &recovered_fts,
      projects_root,
      []() {
        return std::string("generated");
      }
  );

  REQUIRE(recovered.size() == 1);
  REQUIRE(recovered[0].project_id == "proj-plain");
  REQUIRE(recovered[0].privacy_mode == "plain");
  REQUIRE_FALSE(recovered[0].project_key_id.has_value());

  holder::project::ProjectRepo recovered_project_repo(recovered_db);
  holder::card::CardRepo recovered_card_repo(recovered_db);
  const auto projects = recovered_project_repo.list();
  REQUIRE(projects.size() == 1);
  const auto cards = recovered_card_repo.list_all("proj-plain");
  REQUIRE(cards.size() == 1);
  REQUIRE(cards[0].title == "Plain card");

  holder::card::CardStore recovered_store(recovered_db, &recovered_fts);
  const auto content = recovered_store.get_content(cards[0]);
  REQUIRE(content.has_value());
  REQUIRE(content.value() == "# Plain card\n\nRecovered as plain.\n");
}

TEST_CASE(
    "Startup recovery skips encrypted project when privacy error is not retryable",
    "[startup][recovery]"
) {
  const auto dir = holder::test::make_temp_dir();
  const auto projects_root = dir / "projects";
  const auto original_db_path = dir / "original.db";
  const auto recovered_db_path = dir / "recovered.db";
  const auto keystore_dir = dir / "keystore";
  std::filesystem::create_directories(projects_root);
  std::filesystem::create_directories(keystore_dir);
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", keystore_dir.string());

  auto original_db = holder::test::open_db_with_schema(original_db_path);
  holder::project::ProjectRepo original_project_repo(original_db);
  holder::model::Project project;
  project.project_id = "proj-mismatch";
  project.name = "Mismatch";
  project.root_path = (projects_root / "mismatch").string();
  project.privacy_mode = "encrypted_git";
  project.created_at = 1;
  project.updated_at = 1;
  original_project_repo.create(project);

  holder::git::RealGitOps git;
  holder::privacy::ensure_encrypted_project_ready(
      git,
      original_project_repo,
      project.project_id,
      project.root_path,
      project.project_key_id,
      project.updated_at,
      []() {
        return std::string("generated-key");
      }
  );

  holder::index::FtsIndexer original_fts(original_db);
  holder::card::CardStore original_store(original_db, &original_fts);
  holder::model::Card card;
  card.card_id = "card-1";
  card.project_id = project.project_id;
  card.title = "Encrypted card";
  card.created_at = 1;
  card.updated_at = 1;
  original_store.create(card, "# Encrypted card\n\nRecovered body.\n");

  const auto card_path = projects_root / "mismatch" / "cards" / "ca" / "rd" / "card-1.md";
  std::ifstream in(card_path, std::ios::binary);
  REQUIRE(in.is_open());
  const std::string envelope(
      (std::istreambuf_iterator<char>(in)),
      std::istreambuf_iterator<char>()
  );
  auto lines = split_lines3_sr(envelope);
  auto meta = nlohmann::json::parse(lines[1]);
  meta["key_id"] = "wrong-key-id";
  lines[1] = meta.dump();
  {
    std::ofstream out(card_path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open());
    out << join_lines3_sr(lines);
  }

  auto recovered_db = holder::test::open_db_with_schema(recovered_db_path);
  holder::index::FtsIndexer recovered_fts(recovered_db);
  const auto recovered = holder::project::recover_projects_from_disk(
      recovered_db,
      &recovered_fts,
      projects_root,
      []() {
        return std::string("recovered-proj");
      }
  );

  REQUIRE(recovered.empty());

  holder::project::ProjectRepo recovered_project_repo(recovered_db);
  REQUIRE(recovered_project_repo.list().empty());
}

TEST_CASE(
    "Startup recovery keeps encrypted projects encrypted when ai message files are malformed",
    "[startup][recovery]"
) {
  const auto dir = holder::test::make_temp_dir();
  const auto projects_root = dir / "projects";
  const auto original_db_path = dir / "original.db";
  const auto recovered_db_path = dir / "recovered.db";
  const auto keystore_dir = dir / "keystore";
  std::filesystem::create_directories(projects_root);
  std::filesystem::create_directories(keystore_dir);
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", keystore_dir.string());

  auto original_db = holder::test::open_db_with_schema(original_db_path);
  holder::project::ProjectRepo original_project_repo(original_db);
  holder::model::Project project;
  project.project_id = "proj-enc";
  project.name = "Encrypted";
  project.root_path = (projects_root / "encrypted").string();
  project.privacy_mode = "encrypted_git";
  project.created_at = 1;
  project.updated_at = 1;
  original_project_repo.create(project);

  holder::git::RealGitOps git;
  holder::privacy::ensure_encrypted_project_ready(
      git,
      original_project_repo,
      project.project_id,
      project.root_path,
      project.project_key_id,
      project.updated_at,
      []() {
        return std::string("generated-key");
      }
  );

  holder::index::FtsIndexer original_fts(original_db);
  holder::card::CardStore original_store(original_db, &original_fts);
  holder::model::Card card;
  card.card_id = "card-1";
  card.project_id = project.project_id;
  card.title = "Encrypted card";
  card.created_at = 1;
  card.updated_at = 1;
  original_store.create(card, "# Encrypted card\n\nRecovered body.\n");

  std::filesystem::create_directories(projects_root / "encrypted" / "ai_messages" / "aa" / "bb");
  {
    std::ofstream bad_message(
        projects_root / "encrypted" / "ai_messages" / "aa" / "bb" /
            "aabbccdd-eeff-0011-2233-445566778899.md",
        std::ios::binary | std::ios::trunc
    );
    REQUIRE(bad_message.is_open());
    bad_message << "";
  }

  auto recovered_db = holder::test::open_db_with_schema(recovered_db_path);
  holder::index::FtsIndexer recovered_fts(recovered_db);
  const auto recovered = holder::project::recover_projects_from_disk(
      recovered_db,
      &recovered_fts,
      projects_root,
      []() {
        return std::string("recovered-proj");
      }
  );

  REQUIRE(recovered.size() == 1);
  REQUIRE(recovered[0].project_id == "proj-enc");
  REQUIRE(recovered[0].privacy_mode == "encrypted_git");
  REQUIRE(recovered[0].project_key_id.has_value());

  holder::card::CardRepo recovered_card_repo(recovered_db);
  const auto cards = recovered_card_repo.list_all("proj-enc");
  REQUIRE(cards.size() == 1);
  REQUIRE(cards[0].title == "Encrypted card");

  holder::card::CardStore recovered_store(recovered_db, &recovered_fts);
  const auto content = recovered_store.get_content(cards[0]);
  REQUIRE(content.has_value());
  REQUIRE(content.value() == "# Encrypted card\n\nRecovered body.\n");
}

TEST_CASE(
    "Startup recovery skips project when fallback plain rebuild also fails",
    "[startup][recovery]"
) {
  const auto dir = holder::test::make_temp_dir();
  const auto projects_root = dir / "projects";
  const auto original_db_path = dir / "original.db";
  const auto recovered_db_path = dir / "recovered.db";
  const auto keystore_dir = dir / "keystore";
  std::filesystem::create_directories(projects_root);
  std::filesystem::create_directories(keystore_dir);
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", keystore_dir.string());

  auto original_db = holder::test::open_db_with_schema(original_db_path);
  holder::project::ProjectRepo original_project_repo(original_db);
  holder::model::Project project;
  project.project_id = "proj-fallback-fail";
  project.name = "Fallback Fail";
  project.root_path = (projects_root / "fallback-fail").string();
  project.privacy_mode = "encrypted_git";
  project.created_at = 1;
  project.updated_at = 1;
  original_project_repo.create(project);

  holder::git::RealGitOps git;
  holder::privacy::ensure_encrypted_project_ready(
      git,
      original_project_repo,
      project.project_id,
      project.root_path,
      project.project_key_id,
      project.updated_at,
      []() {
        return std::string("generated-key");
      }
  );

  holder::index::FtsIndexer original_fts(original_db);
  holder::card::CardStore original_store(original_db, &original_fts);
  holder::model::Card card;
  card.card_id = "card-1";
  card.project_id = project.project_id;
  card.title = "Encrypted card";
  card.created_at = 1;
  card.updated_at = 1;
  original_store.create(card, "# Encrypted card\n\nRecovered body.\n");

  const auto card_path = projects_root / "fallback-fail" / "cards" / "ca" / "rd" / "card-1.md";
  std::ifstream in(card_path, std::ios::binary);
  REQUIRE(in.is_open());
  const std::string envelope(
      (std::istreambuf_iterator<char>(in)),
      std::istreambuf_iterator<char>()
  );
  auto lines = split_lines3_sr(envelope);
  auto meta = nlohmann::json::parse(lines[1]);
  meta["key_id"] = "wrong-key-id";
  lines[1] = meta.dump();
  {
    std::ofstream out(card_path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open());
    out << join_lines3_sr(lines);
  }

  std::filesystem::create_directories(projects_root / "fallback-fail" / "cards" / "zz" / "zz");
  {
    std::ofstream bad_card(
        projects_root / "fallback-fail" / "cards" / "zz" / "zz" / "bad00001.md",
        std::ios::binary | std::ios::trunc
    );
    REQUIRE(bad_card.is_open());
    bad_card << "# not decryptable as plain\n";
  }

  auto recovered_db = holder::test::open_db_with_schema(recovered_db_path);
  holder::index::FtsIndexer recovered_fts(recovered_db);
  const auto recovered = holder::project::recover_projects_from_disk(
      recovered_db,
      &recovered_fts,
      projects_root,
      []() {
        return std::string("recovered-proj");
      }
  );

  REQUIRE(recovered.empty());

  holder::project::ProjectRepo recovered_project_repo(recovered_db);
  REQUIRE(recovered_project_repo.list().empty());
}

TEST_CASE("Startup recovery skips malformed plain projects", "[startup][recovery]") {
  const auto dir = holder::test::make_temp_dir();
  const auto projects_root = dir / "projects";
  const auto recovered_db_path = dir / "recovered.db";
  const auto project_root = projects_root / "bad-plain";
  std::filesystem::create_directories(project_root / "cards" / "xx" / "yy");

  {
    std::ofstream card(
        project_root / "cards" / "xx" / "yy" / "abcd1234.md",
        std::ios::binary | std::ios::trunc
    );
    REQUIRE(card.is_open());
    card << "# wrong path for card id\n";
  }

  auto recovered_db = holder::test::open_db_with_schema(recovered_db_path);
  holder::index::FtsIndexer recovered_fts(recovered_db);
  const auto recovered = holder::project::recover_projects_from_disk(
      recovered_db,
      &recovered_fts,
      projects_root,
      []() {
        return std::string("generated");
      }
  );

  REQUIRE(recovered.empty());

  holder::project::ProjectRepo recovered_project_repo(recovered_db);
  REQUIRE(recovered_project_repo.list().empty());
}
