#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "core_test_helpers.h"
#include "git/GitRepo.h"
#include "git/RevisionReferenceResolver.h"
#include "history/CardHistory.h"
#include "model/Project.h"

#include <git2.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace {

using holder::git::RevisionReferenceResolver;
using holder::git::RevisionReferenceResult;
using holder::git::RevisionReferenceStatus;

void require_resolved(const RevisionReferenceResult& result, const std::string& oid) {
  REQUIRE(result.status == RevisionReferenceStatus::Resolved);
  REQUIRE(result.oid == oid);
}

void require_not_found(const RevisionReferenceResult& result) {
  REQUIRE(result.status == RevisionReferenceStatus::NotFound);
  REQUIRE_FALSE(result.oid.has_value());
}

std::string uppercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
    if (character >= 'a' && character <= 'f') {
      return static_cast<char>(character - 'a' + 'A');
    }
    return static_cast<char>(character);
  });
  return value;
}

std::string commit_content(const std::string& tree_oid, std::uint32_t nonce) {
  return "tree " + tree_oid +
         "\n"
         "author Holder <holder@example.invalid> 1 +0000\n"
         "committer Holder <holder@example.invalid> 1 +0000\n\n"
         "revision collision " +
         std::to_string(nonce) + "\n";
}

std::pair<std::string, std::string> write_ambiguous_commit_prefix(
    const std::filesystem::path& root,
    const std::string& tree_oid
) {
  git_repository* raw = nullptr;
  REQUIRE(git_repository_open(&raw, root.string().c_str()) == 0);
  git_odb* odb = nullptr;
  REQUIRE(git_repository_odb(&odb, raw) == 0);

  std::unordered_map<std::uint32_t, std::uint32_t> seen;
  std::optional<std::pair<std::uint32_t, std::uint32_t>> collision;
  for (std::uint32_t nonce = 0; nonce < 500'000 && !collision.has_value(); ++nonce) {
    const auto content = commit_content(tree_oid, nonce);
    git_oid oid{};
    const int hash_rc = git_odb_hash(&oid, content.data(), content.size(), GIT_OBJECT_COMMIT);
    if (hash_rc != 0) {
      FAIL("git_odb_hash failed while constructing ambiguous commit prefixes");
    }
    const auto prefix = (static_cast<std::uint32_t>(oid.id[0]) << 24U) |
                        (static_cast<std::uint32_t>(oid.id[1]) << 16U) |
                        (static_cast<std::uint32_t>(oid.id[2]) << 8U) |
                        static_cast<std::uint32_t>(oid.id[3]);
    const auto [position, inserted] = seen.emplace(prefix, nonce);
    if (!inserted) collision = std::pair{position->second, nonce};
  }
  REQUIRE(collision.has_value());

  std::pair<std::string, std::string> oids;
  const std::uint32_t nonces[] = {collision->first, collision->second};
  std::string* outputs[] = {&oids.first, &oids.second};
  for (std::size_t index = 0; index < 2; ++index) {
    const auto content = commit_content(tree_oid, nonces[index]);
    git_oid oid{};
    REQUIRE(git_odb_write(&oid, odb, content.data(), content.size(), GIT_OBJECT_COMMIT) == 0);
    *outputs[index] = git_oid_tostr_s(&oid);
  }

  git_odb_free(odb);
  git_repository_free(raw);
  REQUIRE(
      oids.first.substr(0, RevisionReferenceResolver::kMinimumPrefixLength) ==
      oids.second.substr(0, RevisionReferenceResolver::kMinimumPrefixLength)
  );
  REQUIRE(oids.first != oids.second);
  return oids;
}

} // namespace

TEST_CASE(
    "RevisionReferenceResolver resolves exact and unique abbreviated commit IDs",
    "[git][revision-reference]"
) {
  const auto root = holder::test::make_temp_dir() / "repo";
  holder::git::GitRepo repo;
  repo.open_or_init(root);
  repo.write_file("first.txt", "first");
  repo.stage_path("first.txt");
  repo.commit("first");
  const auto first_oid = repo.head_oid().value();
  repo.write_file("second.txt", "second");
  repo.stage_path("second.txt");
  repo.commit("second");

  RevisionReferenceResolver resolver(repo);
  require_resolved(resolver.resolve(first_oid), first_oid);
  require_resolved(resolver.resolve(uppercase(first_oid)), first_oid);
  const auto shortest_prefix = first_oid.substr(0, RevisionReferenceResolver::kMinimumPrefixLength);
  require_resolved(resolver.resolve(shortest_prefix), first_oid);
  require_resolved(resolver.resolve(uppercase(shortest_prefix)), first_oid);
}

TEST_CASE(
    "RevisionReferenceResolver enforces its own conservative hex-prefix policy",
    "[git][revision-reference]"
) {
  STATIC_REQUIRE(RevisionReferenceResolver::kMinimumPrefixLength == 8);

  const auto root = holder::test::make_temp_dir() / "repo";
  holder::git::GitRepo repo;
  repo.open_or_init(root);
  repo.write_file("card.md", "body");
  repo.stage_path("card.md");
  repo.commit("seed");
  const auto oid = repo.head_oid().value();
  RevisionReferenceResolver resolver(repo);

  require_not_found(
      resolver.resolve(oid.substr(0, RevisionReferenceResolver::kMinimumPrefixLength - 1))
  );
  require_not_found(resolver.resolve("HEAD"));
  require_not_found(resolver.resolve("1234567g"));
  require_not_found(resolver.resolve(std::string(GIT_OID_HEXSZ + 1, 'a')));
  require_not_found(resolver.resolve(std::string(GIT_OID_HEXSZ, '0')));
}

TEST_CASE(
    "RevisionReferenceResolver reports ambiguous repository-native commit prefixes",
    "[git][revision-reference]"
) {
  const auto root = holder::test::make_temp_dir() / "repo";
  holder::git::GitRepo repo;
  repo.open_or_init(root);
  repo.write_file("seed.txt", "seed");
  repo.stage_path("seed.txt");
  repo.commit("seed");

  git_repository* raw = nullptr;
  REQUIRE(git_repository_open(&raw, root.string().c_str()) == 0);
  git_commit* head = nullptr;
  git_oid head_oid{};
  REQUIRE(git_reference_name_to_id(&head_oid, raw, "HEAD") == 0);
  REQUIRE(git_commit_lookup(&head, raw, &head_oid) == 0);
  const std::string tree_oid = git_oid_tostr_s(git_commit_tree_id(head));
  git_commit_free(head);
  git_repository_free(raw);

  const auto [first_oid, second_oid] = write_ambiguous_commit_prefix(root, tree_oid);
  REQUIRE(repo.head_oid() != first_oid);
  REQUIRE(repo.head_oid() != second_oid);
  RevisionReferenceResolver resolver(repo);
  const auto ambiguous = resolver.resolve(
      first_oid.substr(0, RevisionReferenceResolver::kMinimumPrefixLength)
  );
  REQUIRE(ambiguous.status == RevisionReferenceStatus::Ambiguous);
  REQUIRE_FALSE(ambiguous.oid.has_value());

  holder::model::Project project;
  project.project_id = "project-history";
  project.root_path = root.string();
  project.privacy_mode = "plain";
  const auto change = holder::history::CardHistoryService().compare_change(
      project,
      "abcd-ambiguous-revision",
      first_oid.substr(0, RevisionReferenceResolver::kMinimumPrefixLength)
  );
  REQUIRE(change.revision.status == RevisionReferenceStatus::Ambiguous);
  REQUIRE_FALSE(change.revision.oid.has_value());
  REQUIRE_FALSE(change.comparison.has_value());

  require_resolved(resolver.resolve(first_oid), first_oid);
  require_resolved(resolver.resolve(second_oid), second_oid);
}

TEST_CASE("RevisionReferenceResolver requires an opened repository", "[git][revision-reference]") {
  holder::git::GitRepo repo;
  RevisionReferenceResolver resolver(repo);
  REQUIRE_THROWS(resolver.resolve("12345678"));
}
