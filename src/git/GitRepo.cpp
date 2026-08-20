#include "git/GitRepo.h"

#include "git/SshAgentAndFileCredentialProvider.h"

#include <git2.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <set>
#include <stdexcept>
#include <system_error>

namespace fs = std::filesystem;

namespace holder::git {

static std::runtime_error git_err(const std::string& what, int rc) {
  const git_error* e = git_error_last();
  std::string msg = what + " (rc=" + std::to_string(rc) + ")";
  if (e && e->message) {
    msg += ": ";
    msg += e->message;
  }
  return std::runtime_error(msg);
}

static std::string oid_to_hex(const git_oid& oid) {
  return std::string(git_oid_tostr_s(&oid));
}

static std::string git_error_message_or_default(const std::string& fallback) {
  const git_error* e = git_error_last();
  if (e && e->message) return std::string(e->message);
  return fallback; // LCOV_EXCL_LINE
}

static bool contains_icase(const std::string& haystack, const std::string& needle) {
  std::string h = haystack;
  std::string n = needle;
  std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) {
    return std::tolower(c);
  });
  std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) {
    return std::tolower(c);
  });
  return h.find(n) != std::string::npos;
}

static RemoteProbeStatus classify_remote_probe_error(const std::string& message) {
  if (contains_icase(message, "authentication") || contains_icase(message, "permission denied") ||
      contains_icase(message, "publickey") || contains_icase(message, "access denied")) {
    return RemoteProbeStatus::AuthFailed;
  }
  if (contains_icase(message, "repository not found") || contains_icase(message, "not found")) {
    return RemoteProbeStatus::NotFound;
  }
  if (contains_icase(message, "unsupported url protocol") ||
      contains_icase(message, "invalid url") || contains_icase(message, "malformed")) {
    return RemoteProbeStatus::InvalidRemoteUrl;
  }
  if (contains_icase(message, "could not resolve host") ||
      contains_icase(message, "failed to resolve address") ||
      contains_icase(message, "connection refused") ||
      contains_icase(message, "failed to connect") || contains_icase(message, "timed out") ||
      contains_icase(message, "timeout")) {
    return RemoteProbeStatus::NetworkError;
  }
  return RemoteProbeStatus::UnknownError;
}

static PushStatus classify_push_error(const std::string& message) {
  if (contains_icase(message, "authentication") || contains_icase(message, "permission denied") ||
      contains_icase(message, "publickey") || contains_icase(message, "access denied")) {
    return PushStatus::AuthFailed;
  }
  if (contains_icase(message, "repository not found") || contains_icase(message, "not found")) {
    return PushStatus::NotFound;
  }
  if (contains_icase(message, "non-fast-forward") || contains_icase(message, "non fast forward") ||
      contains_icase(message, "fetch first")) {
    return PushStatus::NonFastForward;
  }
  if (contains_icase(message, "could not resolve host") ||
      contains_icase(message, "failed to resolve address") ||
      contains_icase(message, "connection refused") ||
      contains_icase(message, "failed to connect") || contains_icase(message, "timed out") ||
      contains_icase(message, "timeout")) {
    return PushStatus::NetworkError;
  }
  return PushStatus::UnknownError;
}

// LCOV_EXCL_START
static PushResult push_head_error_result(git_remote* remote, const std::string& local_head_commit) {
  const std::string error = git_error_message_or_default("git_repository_head failed");
  git_remote_free(remote);
  return {
      .status = classify_push_error(error),
      .ahead_count = 0,
      .behind_count = 0,
      .local_head_commit = local_head_commit,
      .error_message = error,
  };
}
// LCOV_EXCL_STOP

static std::string configured_default_branch_name() {
  const char* env_branch = std::getenv("GIT_DEFAULT_BRANCH");
  if (env_branch && env_branch[0] != '\0') {
    return std::string(env_branch);
  }

  git_config* cfg = nullptr;
  if (git_config_open_default(&cfg) != 0 || cfg == nullptr) {
    return {};
  }

  git_buf value = GIT_BUF_INIT;
  const int rc = git_config_get_string_buf(&value, cfg, "init.defaultBranch");
  std::string out;
  if (rc == 0 && value.ptr && value.size > 0) {
    out.assign(value.ptr, value.size); // LCOV_EXCL_LINE
  }
  git_buf_dispose(&value);
  git_config_free(cfg);
  return out;
}

static std::string local_head_symbolic_branch_name(git_repository* repo) {
  git_reference* head_ref = nullptr;
  const int rc = git_reference_lookup(&head_ref, repo, "HEAD");
  if (rc != 0 || head_ref == nullptr) {
    return {}; // LCOV_EXCL_LINE
  }

  const char* sym_target = git_reference_symbolic_target(head_ref);
  std::string out;
  if (sym_target != nullptr) {
    static constexpr const char* kHeadsPrefix = "refs/heads/";
    const std::string target(sym_target);
    if (target.rfind(kHeadsPrefix, 0) == 0) {
      out = target.substr(std::char_traits<char>::length(kHeadsPrefix));
    } else {
      out = target; // LCOV_EXCL_LINE
    }
  }
  git_reference_free(head_ref);
  return out;
}

static std::string resolve_branch_name(git_repository* repo, const std::string& requested_branch) {
  if (!requested_branch.empty()) return requested_branch;

  auto head_branch = local_head_symbolic_branch_name(repo);
  if (!head_branch.empty()) return head_branch;

  auto configured = configured_default_branch_name();
  if (!configured.empty()) return configured;

  // Holder fallback branch when no local/default Git branch is discoverable.
  return "cards";
}

static std::string local_head_commit_or_empty(git_repository* repo) {
  git_reference* head = nullptr;
  const int rc = git_repository_head(&head, repo);
  if (rc != 0 || head == nullptr) {
    return {};
  }

  const git_oid* oid = git_reference_target(head);
  std::string out;
  if (oid != nullptr) {
    const char* text = git_oid_tostr_s(oid);
    if (text != nullptr) {
      out = text;
    }
  }
  git_reference_free(head);
  return out;
}

static int git_credential_acquire_cb(
    git_credential** out,
    const char* url,
    const char* username_from_url,
    unsigned int allowed_types,
    void* payload
) {
  auto* provider = static_cast<GitCredentialProvider*>(payload);
  if (provider == nullptr) return GIT_PASSTHROUGH; // LCOV_EXCL_LINE

  return provider->acquire(out, url, username_from_url, allowed_types) ? 0 : GIT_PASSTHROUGH;
}

static git_remote_callbacks make_remote_callbacks(GitCredentialProvider* provider) {
  git_remote_callbacks callbacks{};
  const int rc = git_remote_init_callbacks(&callbacks, GIT_REMOTE_CALLBACKS_VERSION);
  if (rc != 0) throw git_err("git_remote_init_callbacks failed", rc); // LCOV_EXCL_LINE
  callbacks.credentials = git_credential_acquire_cb;
  callbacks.payload = provider;
  return callbacks;
}

static std::string trim_ref_prefix(const std::string& refname, const std::string& prefix) {
  if (refname.rfind(prefix, 0) == 0) {
    return refname.substr(prefix.size());
  }
  return refname; // LCOV_EXCL_LINE
}

static bool reference_exists(git_repository* repo, const std::string& refname) {
  git_reference* ref = nullptr;
  const int rc = git_reference_lookup(&ref, repo, refname.c_str());
  if (rc == 0) {
    git_reference_free(ref);
    return true;
  }
  return false; // LCOV_EXCL_LINE
}

static git_oid remote_branch_target_oid_or_throw(
    git_repository* repo,
    const std::string& remote_name,
    const std::string& branch
) {
  const std::string refname = "refs/remotes/" + remote_name + "/" + branch;
  git_reference* ref = nullptr;
  const int rc = git_reference_lookup(&ref, repo, refname.c_str());
  if (rc != 0) throw git_err("git_reference_lookup failed for " + refname, rc);
  const git_oid* oid = git_reference_target(ref);
  if (!oid) {
    git_reference_free(ref); // LCOV_EXCL_LINE
    throw std::runtime_error("Remote reference has no target: " + refname); // LCOV_EXCL_LINE
  }
  git_oid out{};
  git_oid_cpy(&out, oid);
  git_reference_free(ref);
  return out;
}

static void checkout_commit_oid(git_repository* repo, const git_oid& oid) {
  git_object* commit_obj = nullptr;
  int rc = git_object_lookup(&commit_obj, repo, &oid, GIT_OBJECT_COMMIT);
  if (rc != 0) throw git_err("git_object_lookup failed", rc);

  git_checkout_options checkout_opts{};
  rc = git_checkout_options_init(&checkout_opts, GIT_CHECKOUT_OPTIONS_VERSION);
  if (rc != 0) throw git_err("git_checkout_options_init failed", rc); // LCOV_EXCL_LINE
  checkout_opts.checkout_strategy = GIT_CHECKOUT_SAFE | GIT_CHECKOUT_RECREATE_MISSING;
  rc = git_checkout_tree(repo, commit_obj, &checkout_opts);
  git_object_free(commit_obj);
  if (rc != 0) throw git_err("git_checkout_tree failed", rc);
}

enum class ChangeType { AddedOrModified, Deleted };
struct ChangedPath {
  std::string path;
  ChangeType type;
};

// Relative paths that differ between two commits' trees -- used to classify a diverged pull's
// changes into "local-only", "remote-only" and "both" (the conflict set), never to attempt a
// line-level content merge.
static std::vector<ChangedPath> diff_changed_paths(
    git_repository* repo,
    const git_oid& from_oid,
    const git_oid& to_oid
) {
  git_commit* from_commit = nullptr;
  int rc = git_commit_lookup(&from_commit, repo, &from_oid);
  if (rc != 0) throw git_err("git_commit_lookup (diff from) failed", rc);
  git_commit* to_commit = nullptr;
  rc = git_commit_lookup(&to_commit, repo, &to_oid);
  if (rc != 0) {
    // LCOV_EXCL_START -- to_oid was already resolved once by the caller; a lookup failure here
    // means the object database changed or was corrupted between the two calls.
    git_commit_free(from_commit);
    throw git_err("git_commit_lookup (diff to) failed", rc);
    // LCOV_EXCL_STOP
  }

  git_tree* from_tree = nullptr;
  rc = git_commit_tree(&from_tree, from_commit);
  if (rc != 0) {
    // LCOV_EXCL_START
    git_commit_free(from_commit);
    git_commit_free(to_commit);
    throw git_err("git_commit_tree (diff from) failed", rc);
    // LCOV_EXCL_STOP
  }
  git_tree* to_tree = nullptr;
  rc = git_commit_tree(&to_tree, to_commit);
  if (rc != 0) {
    // LCOV_EXCL_START
    git_tree_free(from_tree);
    git_commit_free(from_commit);
    git_commit_free(to_commit);
    throw git_err("git_commit_tree (diff to) failed", rc);
    // LCOV_EXCL_STOP
  }

  git_diff_options diff_opts{};
  rc = git_diff_options_init(&diff_opts, GIT_DIFF_OPTIONS_VERSION);
  git_diff* diff = nullptr;
  if (rc == 0) rc = git_diff_tree_to_tree(&diff, repo, from_tree, to_tree, &diff_opts);

  git_tree_free(from_tree);
  git_tree_free(to_tree);
  git_commit_free(from_commit);
  git_commit_free(to_commit);
  if (rc != 0) throw git_err("git_diff_tree_to_tree failed", rc); // LCOV_EXCL_LINE

  std::vector<ChangedPath> result;
  const size_t n = git_diff_num_deltas(diff);
  for (size_t i = 0; i < n; ++i) {
    const git_diff_delta* delta = git_diff_get_delta(diff, i);
    const char* path = delta->new_file.path ? delta->new_file.path : delta->old_file.path;
    const auto type = delta->status == GIT_DELTA_DELETED ? ChangeType::Deleted : ChangeType::AddedOrModified;
    result.push_back({std::string(path), type});
  }
  git_diff_free(diff);
  return result;
}

GitRepo::GitRepo() : credential_provider_(std::make_shared<SshAgentAndFileCredentialProvider>()) {
  git_libgit2_init();
}

GitRepo::~GitRepo() {
  if (repo_) {
    git_repository_free(reinterpret_cast<git_repository*>(repo_));
    repo_ = nullptr;
  }
  git_libgit2_shutdown();
}

void GitRepo::set_credential_provider(std::shared_ptr<GitCredentialProvider> provider) {
  credential_provider_ = std::move(provider);
}

void GitRepo::ensure_open() const {
  if (!repo_) throw std::runtime_error("GitRepo not opened");
}

void GitRepo::open_or_init(const fs::path& repo_dir) {
  if (repo_) {
    git_repository_free(reinterpret_cast<git_repository*>(repo_));
    repo_ = nullptr;
  }

  repo_dir_ = repo_dir;
  std::error_code ec;
  fs::create_directories(repo_dir_, ec);
  if (ec)
    throw std::runtime_error(
        "Failed to create repo dir: " + repo_dir_.string() + " (" + ec.message() + ")"
    );

  git_repository* r = nullptr;

  // Try open first
  int rc = git_repository_open(&r, repo_dir_.string().c_str());
  if (rc == 0) {
    repo_ = r;
    spdlog::info("Opened existing git repo: {}", repo_dir_.string());
    return;
  }

  // Otherwise init
  git_repository_init_options opts{};
  rc = git_repository_init_options_init(&opts, GIT_REPOSITORY_INIT_OPTIONS_VERSION);
  if (rc != 0) throw git_err("git_repository_init_options_init failed", rc); // LCOV_EXCL_LINE
  opts.flags = GIT_REPOSITORY_INIT_MKPATH; // make dirs
  opts.mode = GIT_REPOSITORY_INIT_SHARED_UMASK;

  rc = git_repository_init_ext(&r, repo_dir_.string().c_str(), &opts);
  if (rc != 0) throw git_err("git_repository_init_ext failed", rc);

  repo_ = r;
  spdlog::info("Initialized new git repo: {}", repo_dir_.string());
}

void GitRepo::write_file(const fs::path& relative_path, const std::string& content) {
  ensure_open();

  // repo workdir:
  const char* wd = git_repository_workdir(reinterpret_cast<git_repository*>(repo_));
  if (!wd) throw std::runtime_error("Repository has no working directory (bare?)");

  fs::path full = fs::path(wd) / relative_path;

  std::error_code ec;
  fs::create_directories(full.parent_path(), ec);
  if (ec)
    // LCOV_EXCL_START
    throw std::runtime_error(
        "Failed to create dirs: " + full.parent_path().string() + " (" + ec.message() + ")"
    );
  // LCOV_EXCL_STOP

  std::ofstream out(full, std::ios::binary);
  if (!out.is_open()) throw std::runtime_error("Failed to open for write: " + full.string());
  out << content;
  out.close();

  spdlog::info("Wrote file: {}", full.string());
}

void GitRepo::stage_path(const fs::path& relative_path) {
  ensure_open();

  git_index* index = nullptr;
  int rc = git_repository_index(&index, reinterpret_cast<git_repository*>(repo_));
  if (rc != 0) throw git_err("git_repository_index failed", rc);

  // libgit2 expects POSIX paths inside repo
  std::string p = relative_path.generic_string();

  rc = git_index_add_bypath(index, p.c_str());
  if (rc != 0) {
    git_index_free(index);
    throw git_err("git_index_add_bypath failed for " + p, rc);
  }

  rc = git_index_write(index);
  git_index_free(index);
  if (rc != 0) throw git_err("git_index_write failed", rc);

  spdlog::info("Staged path: {}", p);
}

void GitRepo::remove_path(const fs::path& relative_path) {
  ensure_open();

  git_index* index = nullptr;
  int rc = git_repository_index(&index, reinterpret_cast<git_repository*>(repo_));
  if (rc != 0) throw git_err("git_repository_index failed", rc);

  std::string p = relative_path.generic_string();
  rc = git_index_remove_bypath(index, p.c_str());
  if (rc != 0) {
    git_index_free(index); // LCOV_EXCL_LINE
    throw git_err("git_index_remove_bypath failed for " + p, rc); // LCOV_EXCL_LINE
  }

  rc = git_index_write(index);
  git_index_free(index);
  if (rc != 0) throw git_err("git_index_write failed", rc);

  spdlog::info("Removed path: {}", p);
}

void GitRepo::make_signature(void** out_sig) const {
  ensure_open();
  git_signature* sig = nullptr;

  // Try user's git config (user.name/user.email)
  int rc = git_signature_default(&sig, reinterpret_cast<git_repository*>(repo_));
  if (rc == 0 && sig) {
    *out_sig = sig;
    return;
  }

  // Fallback: placeholders (v0.1). Cards can later help set real identity.
  rc = git_signature_now(&sig, "Holder", "holder@localhost");
  if (rc != 0) throw git_err("git_signature_now failed", rc);

  *out_sig = sig;
}

void GitRepo::commit(const std::string& message) {
  ensure_open();

  git_index* index = nullptr;
  int rc = git_repository_index(&index, reinterpret_cast<git_repository*>(repo_));
  if (rc != 0) throw git_err("git_repository_index failed", rc);

  git_oid tree_oid{};
  rc = git_index_write_tree(&tree_oid, index);
  if (rc != 0) {
    git_index_free(index); // LCOV_EXCL_LINE
    throw git_err("git_index_write_tree failed", rc); // LCOV_EXCL_LINE
  }

  rc = git_index_write(index);
  git_index_free(index);
  if (rc != 0) throw git_err("git_index_write failed", rc);

  git_tree* tree = nullptr;
  rc = git_tree_lookup(&tree, reinterpret_cast<git_repository*>(repo_), &tree_oid);
  if (rc != 0) throw git_err("git_tree_lookup failed", rc);

  git_signature* sig = nullptr;
  make_signature(reinterpret_cast<void**>(&sig));

  // Determine if HEAD exists (initial commit vs subsequent)
  git_reference* head_ref = nullptr;
  rc = git_repository_head(&head_ref, reinterpret_cast<git_repository*>(repo_));

  git_oid commit_oid{};

  // No commits yet (either HEAD missing or branch is unborn)
  if (rc == GIT_ENOTFOUND || rc == GIT_EUNBORNBRANCH) {
    rc = git_commit_create_v(
        &commit_oid,
        reinterpret_cast<git_repository*>(repo_),
        "HEAD",
        sig,
        sig,
        nullptr,
        message.c_str(),
        tree,
        0
    );

    git_tree_free(tree);
    git_signature_free(sig);

    if (rc != 0) throw git_err("git_commit_create_v (initial) failed", rc);

    spdlog::info("Created initial commit.");
    return;
  }

  if (rc != 0) {
    git_tree_free(tree);
    git_signature_free(sig);
    throw git_err("git_repository_head failed", rc);
  }

  // Lookup parent commit
  const git_oid* parent_oid = git_reference_target(head_ref);
  if (!parent_oid) {
    git_reference_free(head_ref); // LCOV_EXCL_LINE
    git_tree_free(tree); // LCOV_EXCL_LINE
    git_signature_free(sig); // LCOV_EXCL_LINE
    throw std::runtime_error("HEAD has no target oid"); // LCOV_EXCL_LINE
  }

  git_commit* parent = nullptr;
  rc = git_commit_lookup(&parent, reinterpret_cast<git_repository*>(repo_), parent_oid);
  git_reference_free(head_ref);
  if (rc != 0) {
    git_tree_free(tree);
    git_signature_free(sig);
    throw git_err("git_commit_lookup failed", rc);
  }

  rc = git_commit_create_v(
      &commit_oid,
      reinterpret_cast<git_repository*>(repo_),
      "HEAD",
      sig,
      sig,
      nullptr,
      message.c_str(),
      tree,
      1,
      parent
  );

  git_commit_free(parent);
  git_tree_free(tree);
  git_signature_free(sig);

  if (rc != 0) throw git_err("git_commit_create_v failed", rc);

  spdlog::info("Created commit: {}", message);
}

void GitRepo::set_remote(const std::string& name, const std::string& url) {
  ensure_open();

  git_remote* remote = nullptr;
  const int lookup =
      git_remote_lookup(&remote, reinterpret_cast<git_repository*>(repo_), name.c_str());
  if (lookup == 0) {
    git_remote_free(remote);
    const int rc =
        git_remote_set_url(reinterpret_cast<git_repository*>(repo_), name.c_str(), url.c_str());
    if (rc != 0) throw git_err("git_remote_set_url failed", rc);
    spdlog::info("Updated git remote {} -> {}", name, url);
    return;
  }
  if (lookup != GIT_ENOTFOUND) {
    throw git_err("git_remote_lookup failed", lookup); // LCOV_EXCL_LINE
  }

  const int rc = git_remote_create(
      &remote,
      reinterpret_cast<git_repository*>(repo_),
      name.c_str(),
      url.c_str()
  );
  if (rc != 0) throw git_err("git_remote_create failed", rc);
  git_remote_free(remote);
  spdlog::info("Created git remote {} -> {}", name, url);
}

void GitRepo::remove_remote(const std::string& name) {
  ensure_open();

  const int rc = git_remote_delete(reinterpret_cast<git_repository*>(repo_), name.c_str());
  if (rc == GIT_ENOTFOUND) {
    return;
  }
  if (rc != 0) throw git_err("git_remote_delete failed", rc);
  spdlog::info("Removed git remote {}", name);
}

RemoteProbeResult GitRepo::probe_remote(const std::string& name) {
  ensure_open();
  auto* repo = reinterpret_cast<git_repository*>(repo_);

  git_remote* remote = nullptr;
  const int lookup = git_remote_lookup(&remote, repo, name.c_str());
  if (lookup == GIT_ENOTFOUND) {
    return {
        .status = RemoteProbeStatus::RemoteUnset,
        .remote_has_head = false,
        .error_message = "Remote not configured: " + name,
    };
  }
  if (lookup != 0) {
    return {
        .status = RemoteProbeStatus::UnknownError,
        .remote_has_head = false,
        .error_message = git_error_message_or_default("git_remote_lookup failed"), // LCOV_EXCL_LINE
    }; // LCOV_EXCL_LINE
  }

  auto callbacks = make_remote_callbacks(credential_provider_.get());
  const int connect_rc =
      git_remote_connect(remote, GIT_DIRECTION_FETCH, &callbacks, nullptr, nullptr);
  if (connect_rc != 0) {
    const std::string error = git_error_message_or_default("git_remote_connect failed");
    const auto status = classify_remote_probe_error(error);
    git_remote_free(remote);
    return {
        .status = status,
        .remote_has_head = false,
        .error_message = error,
    };
  }

  const git_remote_head** heads = nullptr;
  size_t heads_len = 0;
  const int ls_rc = git_remote_ls(&heads, &heads_len, remote);
  if (ls_rc != 0) {
    const std::string error = git_error_message_or_default("git_remote_ls failed"
    ); // LCOV_EXCL_LINE
    const auto status = classify_remote_probe_error(error); // LCOV_EXCL_LINE
    git_remote_disconnect(remote); // LCOV_EXCL_LINE
    git_remote_free(remote); // LCOV_EXCL_LINE
    return {
        .status = status,
        .remote_has_head = false,
        .error_message = error, // LCOV_EXCL_LINE
    }; // LCOV_EXCL_LINE
  } // LCOV_EXCL_LINE

  git_remote_disconnect(remote);
  git_remote_free(remote);
  return {
      .status = RemoteProbeStatus::Reachable,
      .remote_has_head = heads_len > 0,
      .error_message = {},
  };
}

PushResult GitRepo::push_branch(
    const std::string& name,
    const std::string& branch,
    bool set_upstream
) {
  ensure_open();
  auto* repo = reinterpret_cast<git_repository*>(repo_);
  const auto local_head_commit = local_head_commit_or_empty(repo);
  const auto resolved_branch = resolve_branch_name(repo, branch);
  if (resolved_branch.empty()) {
    return {
        .status = PushStatus::UnknownError,
        .ahead_count = 0,
        .behind_count = 0,
        .local_head_commit = local_head_commit,
        .error_message =
            "No branch configured. Set GIT_DEFAULT_BRANCH or git config init.defaultBranch.", // LCOV_EXCL_LINE
    }; // LCOV_EXCL_LINE
  }

  git_remote* remote = nullptr;
  const int lookup = git_remote_lookup(&remote, repo, name.c_str());
  if (lookup == GIT_ENOTFOUND) {
    return {
        .status = PushStatus::RemoteUnset,
        .ahead_count = 0,
        .behind_count = 0,
        .local_head_commit = local_head_commit,
        .error_message = "Remote not configured: " + name,
    };
  }
  if (lookup != 0) {
    return {
        .status = PushStatus::UnknownError,
        .ahead_count = 0,
        .behind_count = 0,
        .local_head_commit = local_head_commit,
        .error_message = git_error_message_or_default("git_remote_lookup failed"), // LCOV_EXCL_LINE
    }; // LCOV_EXCL_LINE
  }

  git_reference* head = nullptr;
  int rc = git_repository_head(&head, repo);
  if (rc == GIT_EUNBORNBRANCH || rc == GIT_ENOTFOUND) {
    git_remote_free(remote);
    return {
        .status = PushStatus::UpToDate,
        .ahead_count = 0,
        .behind_count = 0,
        .local_head_commit = local_head_commit,
        .error_message = {},
    };
  }
  if (rc != 0) {
    return push_head_error_result(remote, local_head_commit); // LCOV_EXCL_LINE
  }
  git_reference_free(head);

  // Push current local HEAD to the resolved remote branch. This avoids failures
  // when local and remote branch names differ.
  const std::string refspec_text = "HEAD:refs/heads/" + resolved_branch;
  char* strings[] = {const_cast<char*>(refspec_text.c_str())};
  git_strarray refspecs{strings, 1};
  git_push_options push_opts{};
  rc = git_push_options_init(&push_opts, GIT_PUSH_OPTIONS_VERSION);
  if (rc != 0) throw git_err("git_push_options_init failed", rc); // LCOV_EXCL_LINE
  push_opts.callbacks = make_remote_callbacks(credential_provider_.get());
  rc = git_remote_push(remote, &refspecs, &push_opts);
  if (rc != 0) {
    const std::string error = git_error_message_or_default("git_remote_push failed");
    git_remote_free(remote);
    return {
        .status = classify_push_error(error),
        .ahead_count = 0,
        .behind_count = 0,
        .local_head_commit = local_head_commit,
        .error_message = error,
    };
  }

  if (set_upstream) {
    git_reference* local_branch = nullptr;
    const std::string local_ref_name = "refs/heads/" + resolved_branch;
    if (git_reference_lookup(&local_branch, repo, local_ref_name.c_str()) == 0) {
      const std::string upstream = name + "/" + resolved_branch;
      // Best-effort: ignore failure here; push already succeeded.
      (void)git_branch_set_upstream(local_branch, upstream.c_str());
      git_reference_free(local_branch);
    }
  }

  git_remote_free(remote);
  return {
      .status = PushStatus::Pushed,
      .ahead_count = 0,
      .behind_count = 0,
      .local_head_commit = local_head_commit,
      .error_message = {},
  };
}

void GitRepo::pull_remote_ff_only(const std::string& name) {
  ensure_open();
  auto* repo = reinterpret_cast<git_repository*>(repo_);

  git_remote* remote = nullptr;
  int rc = git_remote_lookup(&remote, repo, name.c_str());
  if (rc != 0) throw git_err("git_remote_lookup failed", rc);

  git_fetch_options fetch_opts{};
  rc = git_fetch_options_init(&fetch_opts, GIT_FETCH_OPTIONS_VERSION);
  if (rc != 0) throw git_err("git_fetch_options_init failed", rc); // LCOV_EXCL_LINE
  fetch_opts.callbacks = make_remote_callbacks(credential_provider_.get());
  rc = git_remote_fetch(remote, nullptr, &fetch_opts, nullptr);
  if (rc != 0) {
    git_remote_free(remote);
    throw git_err("git_remote_fetch failed", rc);
  }

  std::string remote_branch;
  git_buf default_branch_buf = GIT_BUF_INIT;
  rc = git_remote_default_branch(&default_branch_buf, remote);
  if (rc == 0 && default_branch_buf.ptr && default_branch_buf.size > 0) {
    remote_branch = trim_ref_prefix(default_branch_buf.ptr, "refs/heads/");
  }
  git_buf_dispose(&default_branch_buf);
  git_remote_free(remote);

  if (remote_branch.empty()) {
    const auto configured = configured_default_branch_name();
    if (!configured.empty() && reference_exists(repo, "refs/remotes/" + name + "/" + configured)) {
      remote_branch = configured; // LCOV_EXCL_LINE
    } else if (reference_exists(repo, "refs/remotes/" + name + "/cards")) {
      remote_branch = "cards";
    } else if (reference_exists(repo, "refs/remotes/" + name + "/main")) { // LCOV_EXCL_LINE
      remote_branch = "main"; // LCOV_EXCL_LINE
    } else if (reference_exists(repo, "refs/remotes/" + name + "/master")) { // LCOV_EXCL_LINE
      remote_branch = "master"; // LCOV_EXCL_LINE
    } else {
      // LCOV_EXCL_START
      throw std::runtime_error("Unable to determine remote default branch for " + name);
      // LCOV_EXCL_STOP
    }
  }

  const git_oid remote_oid = remote_branch_target_oid_or_throw(repo, name, remote_branch);

  git_reference* head = nullptr;
  rc = git_repository_head(&head, repo);
  if (rc == GIT_EUNBORNBRANCH || rc == GIT_ENOTFOUND) {
    const std::string local_ref = "refs/heads/" + remote_branch;
    git_reference* created = nullptr;
    rc = git_reference_create(&created, repo, local_ref.c_str(), &remote_oid, 1, "pull bootstrap");
    if (rc != 0) throw git_err("git_reference_create failed", rc);
    git_reference_free(created);

    rc = git_repository_set_head(repo, local_ref.c_str());
    if (rc != 0) throw git_err("git_repository_set_head failed", rc);

    checkout_commit_oid(repo, remote_oid);
    spdlog::info("Pulled {} (initialized branch {})", name, remote_branch);
    return;
  }
  if (rc != 0) throw git_err("git_repository_head failed", rc);

  git_reference* head_resolved = nullptr;
  rc = git_reference_resolve(&head_resolved, head);
  git_reference_free(head);
  if (rc != 0) throw git_err("git_reference_resolve failed", rc);

  const git_oid* local_oid_ptr = git_reference_target(head_resolved);
  if (!local_oid_ptr) {
    git_reference_free(head_resolved); // LCOV_EXCL_LINE
    throw std::runtime_error("HEAD has no target OID"); // LCOV_EXCL_LINE
  }
  git_oid local_oid{};
  git_oid_cpy(&local_oid, local_oid_ptr);

  if (git_oid_equal(&local_oid, &remote_oid) != 0) {
    git_reference_free(head_resolved);
    spdlog::info("Pull {} skipped (already up to date)", name);
    return;
  }

  const int ff_ok = git_graph_descendant_of(repo, &remote_oid, &local_oid);
  if (ff_ok != 1) {
    git_reference_free(head_resolved);
    // Local being strictly ahead (remote hasn't moved since local's last pull) isn't a
    // divergence -- there's nothing to pull, and it's a routine state (e.g. right after a local
    // commit that hasn't been pushed yet), not an error.
    if (git_graph_descendant_of(repo, &local_oid, &remote_oid) == 1) {
      spdlog::info("Pull {} skipped (local already ahead)", name);
      return;
    }
    throw NonFastForwardPullError(oid_to_hex(local_oid), oid_to_hex(remote_oid), name);
  }

  git_reference* updated = nullptr;
  rc = git_reference_set_target(&updated, head_resolved, &remote_oid, "pull: fast-forward");
  git_reference_free(head_resolved);
  if (rc != 0) throw git_err("git_reference_set_target failed", rc);

  const char* updated_name = git_reference_name(updated);
  rc = git_repository_set_head(repo, updated_name);
  git_reference_free(updated);
  if (rc != 0) throw git_err("git_repository_set_head failed", rc);

  checkout_commit_oid(repo, remote_oid);
  spdlog::info("Pulled {} (fast-forward)", name);
}

GitRepo::DivergedMergeResult GitRepo::merge_remote_taking_theirs_for_conflicts(
    const std::string& name,
    const std::string& local_oid_hex,
    const std::string& remote_oid_hex
) {
  ensure_open();
  auto* repo = reinterpret_cast<git_repository*>(repo_);

  git_oid local_oid{};
  if (git_oid_fromstr(&local_oid, local_oid_hex.c_str()) != 0) {
    throw std::runtime_error("invalid local oid: " + local_oid_hex); // LCOV_EXCL_LINE
  }
  git_oid remote_oid{};
  if (git_oid_fromstr(&remote_oid, remote_oid_hex.c_str()) != 0) {
    throw std::runtime_error("invalid remote oid: " + remote_oid_hex); // LCOV_EXCL_LINE
  }

  git_oid merge_base_oid{};
  int rc = git_merge_base(&merge_base_oid, repo, &local_oid, &remote_oid);
  if (rc != 0) throw git_err("git_merge_base failed", rc);

  const auto local_changes = diff_changed_paths(repo, merge_base_oid, local_oid);
  const auto remote_changes = diff_changed_paths(repo, merge_base_oid, remote_oid);

  std::set<std::string> remote_changed_set;
  for (const auto& c : remote_changes) remote_changed_set.insert(c.path);

  std::vector<std::string> conflicted;
  std::vector<ChangedPath> local_only;
  for (const auto& c : local_changes) {
    if (remote_changed_set.count(c.path) != 0) {
      conflicted.push_back(c.path);
    } else {
      local_only.push_back(c);
    }
  }

  // Remote is the baseline: correct as-is for remote-only changes, and (deliberately, per this
  // strategy) for conflicted paths too -- the pre-merge local content at those paths is not
  // lost, just no longer live here; the caller reads it via read_blob_at(local_oid_hex, path)
  // to preserve it separately (e.g. as a duplicate card) if it wants to.
  checkout_commit_oid(repo, remote_oid);

  const char* wd = git_repository_workdir(repo);
  if (!wd) throw std::runtime_error("Repository has no working directory (bare?)"); // LCOV_EXCL_LINE

  for (const auto& c : local_only) {
    if (c.type == ChangeType::Deleted) {
      std::error_code ec;
      fs::remove(fs::path(wd) / c.path, ec);
      remove_path(c.path);
    } else {
      const auto content = read_blob_at(local_oid_hex, c.path);
      if (content.has_value()) {
        write_file(c.path, *content);
        stage_path(c.path);
      }
    }
  }

  git_commit* local_parent = nullptr;
  rc = git_commit_lookup(&local_parent, repo, &local_oid);
  if (rc != 0) throw git_err("git_commit_lookup (local parent) failed", rc); // LCOV_EXCL_LINE
  git_commit* remote_parent = nullptr;
  rc = git_commit_lookup(&remote_parent, repo, &remote_oid);
  if (rc != 0) {
    git_commit_free(local_parent); // LCOV_EXCL_LINE
    throw git_err("git_commit_lookup (remote parent) failed", rc); // LCOV_EXCL_LINE
  }

  git_index* index = nullptr;
  rc = git_repository_index(&index, repo);
  if (rc != 0) {
    git_commit_free(local_parent); // LCOV_EXCL_LINE
    git_commit_free(remote_parent); // LCOV_EXCL_LINE
    throw git_err("git_repository_index failed", rc); // LCOV_EXCL_LINE
  }

  git_oid tree_oid{};
  rc = git_index_write_tree(&tree_oid, index);
  if (rc != 0) {
    git_index_free(index); // LCOV_EXCL_LINE
    git_commit_free(local_parent); // LCOV_EXCL_LINE
    git_commit_free(remote_parent); // LCOV_EXCL_LINE
    throw git_err("git_index_write_tree failed", rc); // LCOV_EXCL_LINE
  }
  rc = git_index_write(index);
  git_index_free(index);
  if (rc != 0) {
    git_commit_free(local_parent); // LCOV_EXCL_LINE
    git_commit_free(remote_parent); // LCOV_EXCL_LINE
    throw git_err("git_index_write failed", rc); // LCOV_EXCL_LINE
  }

  git_tree* merged_tree = nullptr;
  rc = git_tree_lookup(&merged_tree, repo, &tree_oid);
  if (rc != 0) {
    git_commit_free(local_parent); // LCOV_EXCL_LINE
    git_commit_free(remote_parent); // LCOV_EXCL_LINE
    throw git_err("git_tree_lookup failed", rc); // LCOV_EXCL_LINE
  }

  git_signature* sig = nullptr;
  make_signature(reinterpret_cast<void**>(&sig));

  const std::string message = "Merge " + name + ": keep remote for " +
      std::to_string(conflicted.size()) + " conflicting card(s)";

  git_oid merge_commit_oid{};
  rc = git_commit_create_v(
      &merge_commit_oid,
      repo,
      "HEAD",
      sig,
      sig,
      nullptr,
      message.c_str(),
      merged_tree,
      2,
      local_parent,
      remote_parent
  );

  git_tree_free(merged_tree);
  git_signature_free(sig);
  git_commit_free(local_parent);
  git_commit_free(remote_parent);

  if (rc != 0) throw git_err("git_commit_create_v (merge) failed", rc); // LCOV_EXCL_LINE

  spdlog::info(
      "Merged {} (kept remote for {} conflicting path(s), preserved {} local-only change(s))",
      name,
      conflicted.size(),
      local_only.size()
  );

  return {conflicted};
}

std::optional<std::string> GitRepo::read_blob_at(
    const std::string& commit_oid_hex,
    const fs::path& relative_path
) {
  ensure_open();
  auto* repo = reinterpret_cast<git_repository*>(repo_);

  git_oid commit_oid{};
  if (git_oid_fromstr(&commit_oid, commit_oid_hex.c_str()) != 0) {
    throw std::runtime_error("invalid commit oid: " + commit_oid_hex); // LCOV_EXCL_LINE
  }

  git_commit* commit = nullptr;
  int rc = git_commit_lookup(&commit, repo, &commit_oid);
  if (rc != 0) throw git_err("git_commit_lookup failed", rc); // LCOV_EXCL_LINE

  git_tree* tree = nullptr;
  rc = git_commit_tree(&tree, commit);
  git_commit_free(commit);
  if (rc != 0) throw git_err("git_commit_tree failed", rc); // LCOV_EXCL_LINE

  const std::string p = relative_path.generic_string();
  git_tree_entry* entry = nullptr;
  rc = git_tree_entry_bypath(&entry, tree, p.c_str());
  git_tree_free(tree);
  if (rc == GIT_ENOTFOUND) return std::nullopt;
  if (rc != 0) throw git_err("git_tree_entry_bypath failed", rc); // LCOV_EXCL_LINE

  git_blob* blob = nullptr;
  rc = git_blob_lookup(&blob, repo, git_tree_entry_id(entry));
  git_tree_entry_free(entry);
  if (rc != 0) throw git_err("git_blob_lookup failed", rc); // LCOV_EXCL_LINE

  const char* data = static_cast<const char*>(git_blob_rawcontent(blob));
  const auto size = static_cast<size_t>(git_blob_rawsize(blob));
  std::string content(data, size);
  git_blob_free(blob);
  return content;
}

int GitRepo::credential_callback_for_tests(
    unsigned int allowed_types,
    const char* username_from_url,
    bool* out_credential_created
) {
  SshAgentAndFileCredentialProvider provider;
  git_credential* cred = nullptr;
  const int rc = git_credential_acquire_cb(
      &cred,
      "ssh://example.invalid/repo.git",
      username_from_url,
      allowed_types,
      &provider
  );
  if (out_credential_created) {
    *out_credential_created = (cred != nullptr);
  }
  if (cred != nullptr) {
    git_credential_free(cred); // LCOV_EXCL_LINE
  }
  return rc;
}

RemoteProbeStatus GitRepo::classify_remote_probe_error_for_tests(const std::string& message) {
  return classify_remote_probe_error(message);
}

PushStatus GitRepo::classify_push_error_for_tests(const std::string& message) {
  return classify_push_error(message);
}

std::string GitRepo::configured_default_branch_name_for_tests() {
  return configured_default_branch_name();
}

} // namespace holder::git
