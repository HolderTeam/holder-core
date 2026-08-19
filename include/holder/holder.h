#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HOLDER_OK 0
#define HOLDER_ERROR_INVALID_ARGUMENT 1
#define HOLDER_ERROR_RUNTIME 2
#define HOLDER_ERROR_ALLOCATION 3

typedef struct holder_context holder_context;
typedef struct holder_error holder_error;

const char* holder_version_string(void);
int holder_version_major(void);
int holder_version_minor(void);
int holder_version_patch(void);

int holder_context_open(
    const char* data_dir,
    const char* schema_sql,
    holder_context** out_context,
    holder_error** out_error
);

void holder_context_destroy(holder_context* context);

int holder_project_list(holder_context* context, char** out_json, holder_error** out_error);

int holder_card_list(
    holder_context* context,
    const char* project_id,
    char** out_json,
    holder_error** out_error
);

// Returns the card's markdown body (not front matter). Fails with
// HOLDER_ERROR_RUNTIME if the card is not found or its content file is missing.
int holder_card_get_content(
    holder_context* context,
    const char* card_id,
    char** out_content,
    holder_error** out_error
);

// privacy_mode may be NULL/empty, defaulting to "plain". root_path may be
// NULL/empty, defaulting to a directory derived from the project name under
// the context's data_dir.
int holder_project_create(
    holder_context* context,
    const char* name,
    const char* root_path,
    const char* privacy_mode,
    char** out_json,
    holder_error** out_error
);

// content and parent_card_id may be NULL.
int holder_card_create(
    holder_context* context,
    const char* project_id,
    const char* title,
    const char* content,
    const char* parent_card_id,
    char** out_json,
    holder_error** out_error
);

// If any project already exists, does nothing and sets *out_json to the JSON
// literal "null". Otherwise creates a plain project named `name` with a
// single welcome card, and sets *out_json to the created project. welcome_content
// may be NULL.
int holder_ensure_default_project(
    holder_context* context,
    const char* name,
    const char* welcome_title,
    const char* welcome_content,
    char** out_json,
    holder_error** out_error
);

// Renames a project, setting *out_json to the updated project.
int holder_project_rename(
    holder_context* context,
    const char* project_id,
    const char* name,
    char** out_json,
    holder_error** out_error
);

// Removes the project row (cascading to its cards per the schema) and any
// sync state. Does not touch the project's files on disk.
int holder_project_delete(holder_context* context, const char* project_id, holder_error** out_error);

// Replaces a card's content and, if title is non-NULL, its title. Sets
// *out_json to the updated card.
int holder_card_update_content(
    holder_context* context,
    const char* card_id,
    const char* content,
    const char* title,
    char** out_json,
    holder_error** out_error
);

// Soft-deletes (trashes) a card; it stops appearing in holder_card_list.
int holder_card_delete(holder_context* context, const char* card_id, holder_error** out_error);

// Full-text searches a project's (non-deleted) cards. Sets *out_json to a JSON
// array of {card_id, title, snippet, rank, created_at, updated_at}, ranked
// best-match first.
int holder_card_search(
    holder_context* context,
    const char* project_id,
    const char* query,
    int limit,
    int offset,
    char** out_json,
    holder_error** out_error
);

// Rebuilds the full-text search index from the database. Safe to call anytime;
// mainly useful to backfill data written before FTS indexing was wired up.
int holder_reindex(holder_context* context, holder_error** out_error);

void holder_string_free(char* value);

const char* holder_error_message(const holder_error* error);
void holder_error_destroy(holder_error* error);

// -- Git sync --

// Global, process-wide libgit2 "home" directory override. Needed on
// Android: libgit2's ssh transport looks for ~/.ssh/known_hosts via $HOME,
// which Android apps don't have. Call once, before any git_* operation,
// with a writable app-private directory (e.g. Context.filesDir). Not needed
// on platforms with a real $HOME (desktop).
int holder_git_set_homedir(const char* path, holder_error** out_error);

// Raw signing callback for holder_git_set_ssh_signer. Given the challenge
// bytes libssh2 wants signed, must return 0 and set *out_der_sig (malloc'd)
// / *out_der_sig_len to a DER-encoded ECDSA-Sig-Value{r,s} produced by an
// ecdsa-sha2-nistp256 (P-256/secp256r1) key, or return nonzero to indicate
// a signing failure for this attempt. holder-core takes ownership of
// *out_der_sig and frees it with free() after copying it.
//
// THREADING: may be invoked from any thread the host app performs a git
// operation on, and multiple times over the signer's lifetime -- once per
// git network operation that needs to authenticate, not once total. Do not
// assume it runs on the thread that called holder_git_set_ssh_signer, and
// do not cache anything thread-affine (e.g. a JNIEnv*) across calls.
typedef int (*holder_ssh_sign_fn)(
    void* user_data,
    const unsigned char* data,
    size_t data_len,
    unsigned char** out_der_sig,
    size_t* out_der_sig_len
);

// Called exactly once to release user_data: either when a later call to
// holder_git_set_ssh_signer replaces this signer, or when context is
// destroyed. May be NULL if user_data needs no cleanup.
typedef void (*holder_destroy_fn)(void* user_data);

// Installs a custom SSH identity (e.g. an Android Keystore-backed key) for
// all subsequent git network operations (test-remote/push/pull) on this
// context, replacing the default ssh-agent/~/.ssh lookup.
//
// public_key_blob is the raw SSH wire-format ecdsa-sha2-nistp256 public key
// blob (RFC 5656 3.1) -- NOT base64-encoded, NOT the
// "ecdsa-sha2-nistp256 AAAA... comment" line.
//
// OWNERSHIP: context takes ownership of user_data and will call
// destroy_user_data(user_data) exactly once -- either when a subsequent
// call to holder_git_set_ssh_signer replaces this signer, or when context
// is destroyed via holder_context_destroy. A caller handing in a JNI global
// reference (or similar) must release it only from destroy_user_data, never
// eagerly: sign_fn can be invoked at any point up until then.
int holder_git_set_ssh_signer(
    holder_context* context,
    const char* username,
    const unsigned char* public_key_blob,
    size_t public_key_blob_len,
    holder_ssh_sign_fn sign_fn,
    void* user_data,
    holder_destroy_fn destroy_user_data,
    holder_error** out_error
);

// Updates a project's configured git remote URL (remote_url NULL/empty
// clears it). Sets *out_json to the updated project.
int holder_project_update_git_remote(
    holder_context* context,
    const char* project_id,
    const char* remote_url,
    char** out_json,
    holder_error** out_error
);

// Sets remote "origin" to the project's configured git_remote_url (if any)
// and probes reachability. branch may be NULL/empty. Sets *out_json to
// {project_id, remote_url, branch, status, remote_has_head, error_message}.
int holder_git_test_remote(
    holder_context* context,
    const char* project_id,
    const char* branch,
    char** out_json,
    holder_error** out_error
);

// Pushes the project's local branch to its configured remote. branch may be
// NULL/empty to use the local HEAD's branch. Records the result into the
// project's sync state (see holder_git_sync_status). Sets *out_json to
// {project_id, remote_url, branch, status, ahead_count, behind_count,
//  local_head_commit, error_message}.
int holder_git_push(
    holder_context* context,
    const char* project_id,
    const char* branch,
    int set_upstream,
    char** out_json,
    holder_error** out_error
);

// Fast-forward-only pulls from the project's configured remote. Records the
// result into the project's sync state. Sets *out_json to
// {project_id, status: "succeeded"|"failed", error_message}.
int holder_git_pull(
    holder_context* context,
    const char* project_id,
    char** out_json,
    holder_error** out_error
);

// Sets *out_json to {project_id, sync: {...}} mirroring
// holder::model::ProjectSyncState (last_commit_at, last_push_at,
// last_pull_at, uncommitted_changes_count, unpushed_commits_count,
// last_push_status, last_pull_status, last_sync_error, last_sync_error_at,
// retry_count, next_retry_at, pull_retry_count, next_pull_retry_at,
// updated_at), or {project_id, sync: null} if no sync activity has been
// recorded yet.
int holder_git_sync_status(
    holder_context* context,
    const char* project_id,
    char** out_json,
    holder_error** out_error
);

#ifdef __cplusplus
}
#endif
