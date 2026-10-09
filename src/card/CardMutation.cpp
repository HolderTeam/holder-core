#include "card/CardMutation.h"

#include <git2.h>
#include <nlohmann/json.hpp>

#include <stdexcept>

namespace holder::card {
namespace {
using Json = nlohmann::json;
template <typename T, void (*Free)(T*)> using GitPtr = std::unique_ptr<T, decltype(Free)>;

void check(int result) {
  if (result < 0) {
    const auto* error = git_error_last();
    throw std::runtime_error(
        std::string("card mutation Git failure: ") + (error ? error->message : "unknown error")
    );
  }
}

struct Repository {
  GitPtr<git_repository, git_repository_free> repo{nullptr, git_repository_free};
  explicit Repository(const std::filesystem::path& root, bool strict = true) {
    git_libgit2_init();
    git_repository* raw = nullptr;
    const int result = git_repository_open_ext(
        &raw,
        root.string().c_str(),
        GIT_REPOSITORY_OPEN_NO_SEARCH,
        nullptr
    );
    if (strict && result != GIT_ENOTFOUND && result < 0) {
      git_libgit2_shutdown();
      check(result);
    }
    repo.reset(raw);
  }
  ~Repository() {
    repo.reset();
    git_libgit2_shutdown();
  }
  std::filesystem::path journal(const std::filesystem::path& root) const {
    return (repo ? std::filesystem::path(git_repository_path(repo.get())) : root) /
           "holder-card-mutation";
  }
  std::filesystem::path index_path() const {
    git_index* raw = nullptr;
    check(git_repository_index(&raw, repo.get()));
    GitPtr<git_index, git_index_free> index(raw, git_index_free);
    return git_index_path(index.get());
  }
};

// HEAD's branch name, which libgit2 can report even before the first commit.
std::string head_reference_name(git_repository* repo) {
  git_reference* raw = nullptr;
  check(git_reference_lookup(&raw, repo, "HEAD"));
  GitPtr<git_reference, git_reference_free> head(raw, git_reference_free);
  const char* target = git_reference_symbolic_target(head.get());
  if (!target) throw std::runtime_error("card lifecycle changes require HEAD to name a branch");
  return target;
}

Json snapshot(holder::core::Fs& fs, const std::filesystem::path& path) {
  if (!fs.exists(path)) return nullptr;
  const auto bytes = fs.read_file(path);
  // Byte arrays also work with the project's minimum nlohmann-json version and
  // never interpret encrypted blobs or the Git index as UTF-8 text.
  return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

void restore(holder::core::Fs& fs, const std::filesystem::path& path, const Json& data) {
  if (data.is_null()) {
    if (fs.exists(path)) fs.remove(path);
  } else {
    const auto bytes = data.get<std::vector<std::uint8_t>>();
    fs.create_directories(path.parent_path());
    const std::string content(bytes.begin(), bytes.end());
    fs.write_file(path, content);
    if (fs.read_file(path) != content) throw std::runtime_error("incomplete card recovery write");
  }
}

void restore_state(
    holder::core::Fs& fs,
    const std::filesystem::path& root,
    Repository& repository,
    const Json& saved
) {
  if (saved.at("version") != 1) throw std::runtime_error("unsupported card mutation journal");
  if (repository.repo && saved.contains("reference") && saved.at("head").is_null()) {
    // The branch was unborn. Undo at most the one root commit the mutation made.
    git_reference* raw = nullptr;
    const int result = git_repository_head(&raw, repository.repo.get());
    if (result != GIT_EUNBORNBRANCH) {
      check(result);
      GitPtr<git_reference, git_reference_free> current(raw, git_reference_free);
      if (saved.at("reference") != git_reference_name(current.get())) {
        throw std::runtime_error("card mutation recovery requires the original Git branch");
      }
      git_commit* commit_raw = nullptr;
      check(
          git_commit_lookup(&commit_raw, repository.repo.get(), git_reference_target(current.get()))
      );
      GitPtr<git_commit, git_commit_free> commit(commit_raw, git_commit_free);
      if (git_commit_parentcount(commit.get()) != 0) {
        throw std::runtime_error("card mutation recovery requires unchanged Git history");
      }
      check(git_reference_delete(current.get()));
    } else if (saved.at("reference") != head_reference_name(repository.repo.get())) {
      throw std::runtime_error("card mutation recovery requires the original Git branch");
    }
  } else if (repository.repo && !saved.at("head").is_null()) {
    git_reference* raw = nullptr;
    check(git_repository_head(&raw, repository.repo.get()));
    GitPtr<git_reference, git_reference_free> current(raw, git_reference_free);
    if (saved.at("reference") != git_reference_name(current.get())) {
      throw std::runtime_error("card mutation recovery requires the original Git branch");
    }
    git_oid before{};
    check(git_oid_fromstr(&before, saved.at("head").get<std::string>().c_str()));
    if (!git_oid_equal(&before, git_reference_target(current.get()))) {
      git_commit* commit_raw = nullptr;
      check(
          git_commit_lookup(&commit_raw, repository.repo.get(), git_reference_target(current.get()))
      );
      GitPtr<git_commit, git_commit_free> commit(commit_raw, git_commit_free);
      if (git_commit_parentcount(commit.get()) != 1 ||
          !git_oid_equal(git_commit_parent_id(commit.get(), 0), &before)) {
        throw std::runtime_error("card mutation recovery requires unchanged Git history");
      }
      git_reference* reset = nullptr;
      check(git_reference_set_target(
          &reset,
          current.get(),
          &before,
          "Recover interrupted card mutation"
      ));
      git_reference_free(reset);
    }
  }
  for (const auto& entry : saved.at("files").items()) {
    const std::filesystem::path path(entry.key());
    if (path.is_absolute() || path.lexically_normal() != path ||
        entry.key().find("..") != std::string::npos ||
        (entry.key().rfind("cards/", 0) != 0 && entry.key().rfind("trash/cards/", 0) != 0)) {
      throw std::runtime_error("invalid card mutation recovery path");
    }
    restore(fs, root / path, entry.value());
  }
  if (repository.repo) restore(fs, repository.index_path(), saved.at("index"));
}
} // namespace

struct CardMutation::State {
  holder::core::Fs& fs;
  std::filesystem::path root;
  Repository repository;
  Json saved;
  State(holder::core::Fs& fs_, const std::filesystem::path& root_)
      : fs(fs_),
        root(root_),
        repository(root) {}
};

CardMutation::CardMutation(
    holder::core::Fs& fs,
    const std::filesystem::path& root,
    const std::vector<std::string>& paths
)
    : state_(std::make_unique<State>(fs, root)) {
  auto& s = *state_;
  if (fs.exists(s.repository.journal(root)))
    throw std::runtime_error("card mutation recovery required");
  s.saved = {{"version", 1}, {"files", Json::object()}, {"head", nullptr}, {"index", nullptr}};
  for (const auto& path : paths)
    s.saved["files"][path] = snapshot(fs, root / path);
  if (s.repository.repo) {
    s.saved["index"] = snapshot(fs, s.repository.index_path());
    git_reference* raw = nullptr;
    const int result = git_repository_head(&raw, s.repository.repo.get());
    if (result == GIT_EUNBORNBRANCH) {
      // A rebuilt or new project may have no commits yet; "head" stays null.
      s.saved["reference"] = head_reference_name(s.repository.repo.get());
    } else {
      check(result);
      GitPtr<git_reference, git_reference_free> head(raw, git_reference_free);
      s.saved["head"] = git_oid_tostr_s(git_reference_target(head.get()));
      s.saved["reference"] = git_reference_name(head.get());
    }
    git_index* index_raw = nullptr;
    check(git_repository_index(&index_raw, s.repository.repo.get()));
    GitPtr<git_index, git_index_free> index(index_raw, git_index_free);
    if (git_index_has_conflicts(index.get()))
      throw std::runtime_error("resolve Git conflicts before changing card lifecycle");
  }
}

CardMutation::~CardMutation() = default;

void CardMutation::begin() {
  auto& s = *state_;
  const auto path = s.repository.journal(s.root);
  const auto temporary = path.string() + ".tmp";
  const auto bytes = Json::to_msgpack(s.saved);
  const std::string content(bytes.begin(), bytes.end());
  s.fs.write_file(temporary, content);
  if (s.fs.read_file(temporary) != content)
    throw std::runtime_error("incomplete card recovery journal");
  s.fs.rename(temporary, path);
  if (s.repository.repo) {
    git_index* index_raw = nullptr;
    check(git_repository_index(&index_raw, s.repository.repo.get()));
    GitPtr<git_index, git_index_free> index(index_raw, git_index_free);
    if (s.saved.at("head").is_null()) {
      // An unborn branch commits against the empty tree.
      check(git_index_clear(index.get()));
    } else {
      git_object* tree_raw = nullptr;
      check(git_revparse_single(&tree_raw, s.repository.repo.get(), "HEAD^{tree}"));
      GitPtr<git_object, git_object_free> tree(tree_raw, git_object_free);
      check(git_index_read_tree(index.get(), reinterpret_cast<git_tree*>(tree.get())));
    }
    check(git_index_write(index.get()));
  }
}

void CardMutation::preserve_unrelated_index() {
  auto& s = *state_;
  if (!s.repository.repo) return;
  git_index* committed_raw = nullptr;
  check(git_repository_index(&committed_raw, s.repository.repo.get()));
  GitPtr<git_index, git_index_free> committed(committed_raw, git_index_free);
  check(git_index_read(committed.get(), 1));
  const auto path = s.repository.index_path();
  restore(s.fs, path, s.saved.at("index"));
  git_index* original_raw = nullptr;
  check(git_index_open(&original_raw, path.string().c_str()));
  GitPtr<git_index, git_index_free> original(original_raw, git_index_free);
  for (const auto& entry : s.saved.at("files").items()) {
    const auto* updated = git_index_get_bypath(committed.get(), entry.key().c_str(), 0);
    if (updated)
      check(git_index_add(original.get(), updated));
    else {
      const int result = git_index_remove_bypath(original.get(), entry.key().c_str());
      if (result != GIT_ENOTFOUND) check(result);
    }
  }
  check(git_index_write(original.get()));
}

void CardMutation::rollback() {
  auto& s = *state_;
  restore_state(s.fs, s.root, s.repository, s.saved);
  finish();
}

void CardMutation::finish() {
  auto& s = *state_;
  const auto path = s.repository.journal(s.root);
  if (s.fs.exists(path)) s.fs.remove(path);
}

bool CardMutation::pending(const std::filesystem::path& root) {
  Repository repository(root, false);
  return std::filesystem::exists(repository.journal(root));
}

void CardMutation::recover_files(holder::core::Fs& fs, const std::filesystem::path& root) {
  Repository repository(root, false);
  const auto path = repository.journal(root);
  if (!fs.exists(path)) return;
  const auto raw = fs.read_file(path);
  const auto saved = Json::from_msgpack(raw.begin(), raw.end());
  restore_state(fs, root, repository, saved);
}

void CardMutation::finish_recovery(holder::core::Fs& fs, const std::filesystem::path& root) {
  Repository repository(root, false);
  const auto path = repository.journal(root);
  if (fs.exists(path)) fs.remove(path);
}
} // namespace holder::card
