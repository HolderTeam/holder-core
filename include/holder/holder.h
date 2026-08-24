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

// Rebuilds the disposable SQLite projection from durable project files beneath
// data_dir/projects. The platform keyring provider must already be registered
// when encrypted projects may be present. This function must be called while
// no holder_context for data_dir is open. Sets *out_json to a rebuild report.
int holder_database_rebuild(
    const char* data_dir,
    const char* schema_sql,
    int dry_run,
    char** out_json,
    holder_error** out_error
);

int holder_project_list(holder_context* context, char** out_json, holder_error** out_error);

// Resource, Asset and Storage Location JSON APIs. Resources are returned as
// {resource: {...}, assets: [{..., placements: [...]}]}; Locations are returned
// as their safe portable declarations. Private Location bindings are deliberately
// outside this C API and remain a host/SecretStore concern.
int holder_resource_list(
    holder_context* context,
    const char* project_id,
    char** out_json,
    holder_error** out_error
);
int holder_resource_get(
    holder_context* context,
    const char* resource_id,
    char** out_json,
    holder_error** out_error
);
int holder_resource_put_json(
    holder_context* context,
    const char* resource_bundle_json,
    char** out_json,
    holder_error** out_error
);
int holder_resource_delete(
    holder_context* context,
    const char* resource_id,
    holder_error** out_error
);
int holder_asset_get(
    holder_context* context,
    const char* asset_id,
    char** out_json,
    holder_error** out_error
);
int holder_asset_put_json(
    holder_context* context,
    const char* resource_id,
    const char* asset_json,
    char** out_json,
    holder_error** out_error
);
int holder_asset_delete(
    holder_context* context,
    const char* asset_id,
    char** out_json,
    holder_error** out_error
);
int holder_location_list(
    holder_context* context,
    const char* project_id,
    char** out_json,
    holder_error** out_error
);
int holder_location_get(
    holder_context* context,
    const char* location_id,
    char** out_json,
    holder_error** out_error
);
int holder_location_put_json(
    holder_context* context,
    const char* location_json,
    char** out_json,
    holder_error** out_error
);
int holder_location_delete(
    holder_context* context,
    const char* location_id,
    holder_error** out_error
);

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

// Lists project_id's trashed (soft-deleted) cards, same JSON card-array shape as
// holder_card_list.
int holder_card_list_trashed(
    holder_context* context,
    const char* project_id,
    char** out_json,
    holder_error** out_error
);

// Restores a trashed card; it reappears in holder_card_list. Sets *out_json to
// the restored card. Fails if card_id isn't currently trashed.
int holder_card_restore(
    holder_context* context,
    const char* card_id,
    char** out_json,
    holder_error** out_error
);

// Permanently deletes a trashed card: removes its database row and its file
// from the git working tree. Irreversible. Fails if card_id isn't currently
// trashed -- restore it first, or use holder_card_delete to trash it.
int holder_card_purge(holder_context* context, const char* card_id, holder_error** out_error);

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

// Lists card_id's connections: *out_json becomes
// {"outgoing": [{to_card_id, to_type, kind, label, created_at, to_title}],
//  "backlinks": [{from_card_id, kind, label, created_at, from_title}],
//  "parent": {card_id, title} | null,
//  "children": [{card_id, title}, ...]}.
// to_title/from_title are null when the id doesn't resolve to a known card.
// parent/children reflect the card's hierarchy (parent_card_id) automatically,
// same as desktop Holder's Connections tool -- they aren't editable through
// this function; move a card via its parent_card_id instead. outgoing/
// backlinks are the explicit link list stored in front matter. Neither
// includes inline [[wikilinks]].
int holder_card_list_links(
    holder_context* context,
    const char* card_id,
    char** out_json,
    holder_error** out_error
);

// Adds (or updates, if from_card_id/to_card_id/kind already matches) an explicit
// outgoing connection. label may be NULL. Sets *out_json to from_card_id's
// updated outgoing connection list, same shape as holder_card_list_links's
// "outgoing" array.
int holder_card_link_add(
    holder_context* context,
    const char* from_card_id,
    const char* to_card_id,
    const char* kind,
    const char* label,
    char** out_json,
    holder_error** out_error
);

// Removes the from_card_id -> to_card_id connection of the given kind, if any.
int holder_card_link_remove(
    holder_context* context,
    const char* from_card_id,
    const char* to_card_id,
    const char* kind,
    holder_error** out_error
);

// Lists card_id's #tags (as extracted from its body by holder_card_create/
// holder_card_update_content), lowercased: *out_json becomes ["todo", "urgent"].
// Tags aren't editable through a dedicated function -- edit the #tag text in
// the card body instead; the index follows automatically.
int holder_card_list_tags(
    holder_context* context,
    const char* card_id,
    char** out_json,
    holder_error** out_error
);

// Lists project_id's (non-trashed) cards carrying `tag` (case-insensitive):
// *out_json becomes [{card_id, title}, ...]. Empty, not an error, for an
// unknown project_id or a tag nothing carries.
int holder_cards_with_tag(
    holder_context* context,
    const char* project_id,
    const char* tag,
    char** out_json,
    holder_error** out_error
);

// Lists every distinct tag in project_id with how many cards carry it, most-
// used first: *out_json becomes [{"tag": "todo", "count": 3}, ...].
int holder_project_list_tags(
    holder_context* context,
    const char* project_id,
    char** out_json,
    holder_error** out_error
);

// Lists card_id's milestones, ordered by start_at: *out_json becomes
// [{milestone_id, card_id, start_at, end_at, all_day, kind, description,
// created_at, updated_at}, ...]. end_at/kind/description are null when unset.
int holder_card_list_milestones(
    holder_context* context,
    const char* card_id,
    char** out_json,
    holder_error** out_error
);

// Adds a new milestone to card_id. has_end_at is 0 to omit end_at (a point
// in time rather than a span); all_day/has_end_at are 0 or 1; kind and
// description may be NULL. Sets *out_json to card_id's updated milestone
// list, same shape as holder_card_list_milestones.
int holder_card_milestone_add(
    holder_context* context,
    const char* card_id,
    long long start_at,
    int has_end_at,
    long long end_at,
    int all_day,
    const char* kind,
    const char* description,
    char** out_json,
    holder_error** out_error
);

// Removes milestone_id from card_id, if present. A no-op, not an error, if
// milestone_id doesn't exist or belongs to a different card.
int holder_card_milestone_remove(
    holder_context* context,
    const char* card_id,
    const char* milestone_id,
    holder_error** out_error
);

// Lists every milestone in project_id whose start_at falls within [from, to]
// (inclusive), ordered by start_at -- the Calendar's primary query. *out_json
// becomes [{..same fields as holder_card_list_milestones, plus card_title},
// ...]. Never includes a trashed card's milestones. card_title is null when
// the card can't be resolved.
int holder_project_list_milestones_in_range(
    holder_context* context,
    const char* project_id,
    long long from,
    long long to,
    char** out_json,
    holder_error** out_error
);

// Lists Holder's full built-in vocabulary of card_links.kind values with
// curated English labels: *out_json becomes [{"id": "depends_on",
// "forward": "Depends on", "reverse": "Required by"}, ...]. forward is how
// the relationship reads from the linking (outgoing) card's side, reverse
// from the linked-to (backlink) card's side. kind on a link isn't
// constrained to this list -- a kind outside it just has no known label
// here; callers should fall back to a humanized display of the raw string.
// Not scoped to a context, since this is static reference data.
int holder_link_kind_list(char** out_json, holder_error** out_error);

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
// OWNERSHIP: this call takes ownership of user_data unconditionally, from
// the moment it's called -- regardless of whether it succeeds. destroy_user_data
// is guaranteed to run exactly once: immediately, before this call returns,
// if it fails for any reason (including invalid arguments); or later, when a
// subsequent call to holder_git_set_ssh_signer replaces this signer or
// context is destroyed via holder_context_destroy, if it succeeds. Callers
// never need to release user_data themselves on a failure return, and must
// not release it eagerly on success (e.g. a JNI global reference must only
// be freed from destroy_user_data, since sign_fn can be invoked at any point
// up until then).
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

// Pulls and/or pushes the project only if enough time has passed since the
// last attempt of each, per the same cadence policy holder-daemon's
// background worker uses (holder::sync::should_attempt_pull/push) --
// intended for a periodic background job (e.g. Android WorkManager) to call
// on a tighter schedule than the desired sync interval, without over-syncing.
// A no-op (HOLDER_OK, pull_attempted/push_attempted both false) if the
// project has no remote configured, or if neither is due yet.
//
// push_interval_seconds/pull_interval_seconds override the default cadence
// (1200s/300s, matching holder-daemon's worker) when positive; pass 0 for
// the default.
//
// Sets *out_json to {project_id, pull_attempted, pull_status, pull_error,
// push_attempted, push_status, push_error}, where each *_status/*_error is
// null when its *_attempted is false.
int holder_git_sync_if_due(
    holder_context* context,
    const char* project_id,
    int push_interval_seconds,
    int pull_interval_seconds,
    char** out_json,
    holder_error** out_error
);

// -- Platform keyring --
//
// Encrypted projects, and other secrets (e.g. AI provider credentials), are
// stored via the host OS's own keyring on desktop (libsecret/Keychain/Windows
// Credential Manager). Android has none of those; holder_keyring_set_provider
// installs a substitute for the whole process, backed by whatever the caller
// wants (Android: an AndroidKeyStore-encrypted store, via JNI).
//
// kind is 0 for a generic secret (identified by service+account) or 1 for a
// project encryption key (identified by project_id+account; service is empty).

// Must return 0 and set *out_found: 1 with *out_secret (malloc'd) set to the
// secret's value if it exists, or 0 (leaving *out_secret untouched) if it does
// not -- that is not a failure. Return nonzero, optionally setting *out_error
// (malloc'd), to indicate a real failure.
typedef int (*holder_keyring_lookup_fn)(
    void* user_data,
    int kind,
    const char* service,
    const char* account,
    const char* project_id,
    int* out_found,
    char** out_secret,
    char** out_error
);

typedef int (*holder_keyring_store_fn)(
    void* user_data,
    int kind,
    const char* service,
    const char* account,
    const char* project_id,
    const char* label,
    const char* secret,
    char** out_error
);

typedef int (*holder_keyring_remove_fn)(
    void* user_data,
    int kind,
    const char* service,
    const char* account,
    const char* project_id,
    char** out_error
);

// Installs a process-wide platform keyring provider -- not scoped to a
// holder_context, since the platform keyring itself isn't either.
//
// OWNERSHIP: unconditional from the moment this is called, exactly like
// holder_git_set_ssh_signer -- destroy_user_data runs exactly once, either
// immediately if this call fails for any reason, or later when a subsequent
// call to holder_keyring_set_provider replaces this provider.
int holder_keyring_set_provider(
    holder_keyring_lookup_fn lookup_fn,
    holder_keyring_store_fn store_fn,
    holder_keyring_remove_fn remove_fn,
    void* user_data,
    holder_destroy_fn destroy_user_data,
    holder_error** out_error
);

// -- Encryption / recovery --

// Runs the encrypted_git safety check (verifies every file under the
// project's cards/ directory has an encryption envelope header) and reports
// the project's privacy mode. For a "plain" project, *out_json's "check" is
// always {ok: true, ...} without touching disk -- there's nothing to check.
// Sets *out_json to {project_id, privacy_mode,
// check: {ok, checked_files, unsafe_files, unsafe_paths, message}}.
int holder_encryption_check(
    holder_context* context,
    const char* project_id,
    char** out_json,
    holder_error** out_error
);

// Exports a PIN-protected recovery token for project_id's encryption key,
// name, and configured git remote -- meant to be moved to another device
// (physically or otherwise) and consumed by holder_recovery_token_import or
// holder_recovery_token_import_global there. Fails if the project has no
// key material configured (i.e. isn't encrypted_git).
// Sets *out_json to {project_id, key_id, recovery_token}.
int holder_recovery_token_export(
    holder_context* context,
    const char* project_id,
    const char* pin,
    char** out_json,
    holder_error** out_error
);

// Imports a recovery token into an *existing* project (project_id must
// already match the token's own project_id, or this fails) -- e.g. to
// recover after this device's own copy of the key was lost without losing
// the project itself. See holder_recovery_token_import_global for the
// device-setup case where the project doesn't exist yet.
// Sets *out_json to {project_id}.
int holder_recovery_token_import(
    holder_context* context,
    const char* project_id,
    const char* pin,
    const char* recovery_token,
    char** out_json,
    holder_error** out_error
);

// Decrypts and returns a recovery token's metadata without importing
// anything -- lets a caller show the user what they're about to recover
// (project name, whether a remote is included) before committing. Not
// scoped to a context, since it has no side effects at all.
// Sets *out_json to {project_id, project_key_id, project_name,
// git_remote_url}, where project_name/git_remote_url are null if the token
// didn't include them.
int holder_recovery_token_inspect(
    const char* pin,
    const char* recovery_token,
    char** out_json,
    holder_error** out_error
);

// The device-setup path: given just a PIN and a recovery token (no known
// project_id), recovers the project -- creating it first if this device has
// never seen it before -- and, if the token includes a remote hint,
// configures that remote and attempts an initial pull.
// Sets *out_json to {project_id, project_created, remote_hint_present,
// remote_configured, remote_error, pull_status, pull_error}, mirroring
// holder-daemon's POST /recovery-token/import response shape.
int holder_recovery_token_import_global(
    holder_context* context,
    const char* pin,
    const char* recovery_token,
    char** out_json,
    holder_error** out_error
);

#ifdef __cplusplus
}
#endif
