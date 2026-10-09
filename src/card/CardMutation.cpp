#include "card/CardMutation.h"

#include <git2.h>
#include <nlohmann/json.hpp>

#include <spdlog/spdlog.h>

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

Json bytes_json(const std::optional<std::string>& bytes) {
  if (!bytes) return nullptr;
  return std::vector<std::uint8_t>(bytes->begin(), bytes->end());
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

std::filesystem::path journal_file_path(const std::string& key) {
  const std::filesystem::path path(key);
  if (path.is_absolute() || path.lexically_normal() != path ||
      key.find("..") != std::string::npos ||
      (key.rfind("cards/", 0) != 0 && key.rfind("trash/cards/", 0) != 0)) {
    throw std::runtime_error("invalid card mutation recovery path");
  }
  return path;
}

std::optional<git_oid> saved_head(const Json& saved) {
  if (saved.at("head").is_null()) return std::nullopt;
  git_oid oid{};
  check(git_oid_fromstr(&oid, saved.at("head").get<std::string>().c_str()));
  return oid;
}

// The saved branch's current commit, or nothing while it is unborn.
std::optional<git_oid> branch_tip(git_repository* repo, const Json& saved) {
  git_oid oid{};
  const int result =
      git_reference_name_to_id(&oid, repo, saved.at("reference").get<std::string>().c_str());
  if (result == GIT_ENOTFOUND) return std::nullopt;
  check(result);
  return oid;
}

bool same(const std::optional<git_oid>& a, const std::optional<git_oid>& b) {
  return a.has_value() == b.has_value() && (!a || git_oid_equal(&*a, &*b));
}

// The operation's own commit has the saved HEAD as its only parent (none for an
// unborn branch) and holds exactly the intended bytes at every changed path.
bool is_operation_commit(git_repository* repo, const git_oid& oid, const Json& saved) {
  git_commit* commit_raw = nullptr;
  check(git_commit_lookup(&commit_raw, repo, &oid));
  GitPtr<git_commit, git_commit_free> commit(commit_raw, git_commit_free);
  const auto before = saved_head(saved);
  if (git_commit_parentcount(commit.get()) != (before ? 1u : 0u)) return false;
  if (before && !git_oid_equal(git_commit_parent_id(commit.get(), 0), &*before)) return false;
  git_tree* tree_raw = nullptr;
  check(git_commit_tree(&tree_raw, commit.get()));
  GitPtr<git_tree, git_tree_free> tree(tree_raw, git_tree_free);
  for (const auto& [key, file] : saved.at("files").items()) {
    if (!file.contains("after")) continue;
    git_tree_entry* entry_raw = nullptr;
    const int result = git_tree_entry_bypath(&entry_raw, tree.get(), key.c_str());
    GitPtr<git_tree_entry, git_tree_entry_free> entry(entry_raw, git_tree_entry_free);
    if (file.at("after").is_null()) {
      if (result != GIT_ENOTFOUND) return false;
      continue;
    }
    if (result == GIT_ENOTFOUND) return false;
    check(result);
    const auto bytes = file.at("after").get<std::vector<std::uint8_t>>();
    git_oid expected{};
    check(git_odb_hash(&expected, bytes.data(), bytes.size(), GIT_OBJECT_BLOB));
    if (!git_oid_equal(&expected, git_tree_entry_id(entry.get()))) return false;
  }
  return true;
}

// Search the saved branch back to the saved HEAD, so commits made on top after an
// interruption do not hide the operation's commit.
std::optional<git_oid> landed_commit(git_repository* repo, const Json& saved) {
  const auto tip = branch_tip(repo, saved);
  if (!tip) return std::nullopt;
  git_revwalk* walk_raw = nullptr;
  check(git_revwalk_new(&walk_raw, repo));
  GitPtr<git_revwalk, git_revwalk_free> walk(walk_raw, git_revwalk_free);
  check(git_revwalk_push(walk.get(), &*tip));
  if (const auto before = saved_head(saved)) check(git_revwalk_hide(walk.get(), &*before));
  git_oid oid{};
  for (int remaining = 10000; remaining > 0 && git_revwalk_next(&oid, walk.get()) == 0;
       --remaining) {
    if (is_operation_commit(repo, oid, saved)) return oid;
  }
  return std::nullopt;
}

void restore_original_files(
    holder::core::Fs& fs,
    const std::filesystem::path& root,
    const Json& saved
) {
  for (const auto& [key, file] : saved.at("files").items())
    restore(fs, root / journal_file_path(key), file.at("before"));
}

// Restore the index as it was before the operation, then take the operation's
// own paths from `commit`, keeping unrelated staged work.
void restore_unrelated_index(
    holder::core::Fs& fs,
    Repository& repository,
    const Json& saved,
    const git_oid& commit_oid
) {
  git_commit* commit_raw = nullptr;
  check(git_commit_lookup(&commit_raw, repository.repo.get(), &commit_oid));
  GitPtr<git_commit, git_commit_free> commit(commit_raw, git_commit_free);
  git_tree* tree_raw = nullptr;
  check(git_commit_tree(&tree_raw, commit.get()));
  GitPtr<git_tree, git_tree_free> tree(tree_raw, git_tree_free);
  const auto path = repository.index_path();
  restore(fs, path, saved.at("index"));
  git_index* index_raw = nullptr;
  check(git_index_open(&index_raw, path.string().c_str()));
  GitPtr<git_index, git_index_free> index(index_raw, git_index_free);
  for (const auto& [key, file] : saved.at("files").items()) {
    git_tree_entry* entry_raw = nullptr;
    const int result = git_tree_entry_bypath(&entry_raw, tree.get(), key.c_str());
    GitPtr<git_tree_entry, git_tree_entry_free> entry(entry_raw, git_tree_entry_free);
    if (result == GIT_ENOTFOUND) {
      const int removed = git_index_remove_bypath(index.get(), key.c_str());
      if (removed != GIT_ENOTFOUND) check(removed);
      continue;
    }
    check(result);
    git_index_entry updated{};
    updated.mode = git_tree_entry_filemode(entry.get());
    updated.id = *git_tree_entry_id(entry.get());
    updated.path = key.c_str();
    check(git_index_add(index.get(), &updated));
  }
  check(git_index_write(index.get()));
}

// Undo a mutation in the process that started it, while it still holds the project
// lock. The branch is reset only when its tip is provably this operation's commit.
void roll_back(
    holder::core::Fs& fs,
    const std::filesystem::path& root,
    Repository& repository,
    const Json& saved
) {
  if (repository.repo) {
    const auto tip = branch_tip(repository.repo.get(), saved);
    const auto before = saved_head(saved);
    if (tip && !same(tip, before) && is_operation_commit(repository.repo.get(), *tip, saved)) {
      git_reference* raw = nullptr;
      check(git_reference_lookup(
          &raw,
          repository.repo.get(),
          saved.at("reference").get<std::string>().c_str()
      ));
      GitPtr<git_reference, git_reference_free> branch(raw, git_reference_free);
      if (before) {
        git_reference* reset = nullptr;
        check(git_reference_set_target(&reset, branch.get(), &*before, "Roll back card mutation"));
        git_reference_free(reset);
      } else {
        check(git_reference_delete(branch.get()));
      }
    }
  }
  restore_original_files(fs, root, saved);
  if (repository.repo) restore(fs, repository.index_path(), saved.at("index"));
}

// Settle a journal left by an interrupted process. If the operation's commit
// landed it is kept, since files and Git already agree; otherwise only files that
// still hold this operation's uncommitted bytes are put back. Refs are never moved.
void recover_journal(
    holder::core::Fs& fs,
    const std::filesystem::path& root,
    Repository& repository,
    const Json& saved
) {
  if (saved.at("version") != 2) throw std::runtime_error("unsupported card mutation journal");
  if (!repository.repo) {
    restore_original_files(fs, root, saved);
    return;
  }
  auto* repo = repository.repo.get();
  if (same(branch_tip(repo, saved), saved_head(saved))) {
    // Nothing was committed and nothing else has committed since: restore exactly.
    restore_original_files(fs, root, saved);
    restore(fs, repository.index_path(), saved.at("index"));
    return;
  }
  if (const auto landed = landed_commit(repo, saved)) {
    git_oid head{};
    if (git_reference_name_to_id(&head, repo, "HEAD") == 0 && git_oid_equal(&head, &*landed)) {
      restore_unrelated_index(fs, repository, saved, *landed);
    }
    return;
  }
  for (const auto& [key, file] : saved.at("files").items()) {
    if (!file.contains("after")) continue;
    const auto path = root / journal_file_path(key);
    if (snapshot(fs, path) == file.at("after")) {
      restore(fs, path, file.at("before"));
    } else if (snapshot(fs, path) != file.at("before")) {
      spdlog::warn("Leaving {} after an interrupted card change: it was changed since", key);
    }
  }
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
    const std::vector<std::string>& paths,
    const std::map<std::string, std::optional<std::string>>& changes
)
    : state_(std::make_unique<State>(fs, root)) {
  auto& s = *state_;
  if (fs.exists(s.repository.journal(root)))
    throw std::runtime_error("card mutation recovery required");
  s.saved = {{"version", 2}, {"files", Json::object()}, {"head", nullptr}, {"index", nullptr}};
  for (const auto& path : paths) {
    journal_file_path(path);
    s.saved["files"][path] = {{"before", snapshot(fs, root / path)}};
  }
  for (const auto& [path, bytes] : changes) {
    if (!s.saved["files"].contains(path)) throw std::invalid_argument("unjournaled card change");
    s.saved["files"][path]["after"] = bytes_json(bytes);
  }
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
  roll_back(s.fs, s.root, s.repository, s.saved);
  finish();
}

void CardMutation::finish() {
  auto& s = *state_;
  const auto path = s.repository.journal(s.root);
  if (s.fs.exists(path)) s.fs.remove(path);
}

bool CardMutation::pending(const std::filesystem::path& root) {
  // Checked before every card write, so avoid opening the repository in the usual layout.
  std::error_code error;
  if (std::filesystem::is_directory(root / ".git", error))
    return std::filesystem::exists(root / ".git" / "holder-card-mutation", error);
  Repository repository(root, false);
  return std::filesystem::exists(repository.journal(root));
}

void CardMutation::recover_files(holder::core::Fs& fs, const std::filesystem::path& root) {
  Repository repository(root, false);
  const auto path = repository.journal(root);
  if (!fs.exists(path)) return;
  const auto raw = fs.read_file(path);
  const auto saved = Json::from_msgpack(raw.begin(), raw.end());
  recover_journal(fs, root, repository, saved);
}

void CardMutation::finish_recovery(holder::core::Fs& fs, const std::filesystem::path& root) {
  Repository repository(root, false);
  const auto path = repository.journal(root);
  if (fs.exists(path)) fs.remove(path);
}
} // namespace holder::card
