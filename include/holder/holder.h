#pragma once

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

#ifdef __cplusplus
}
#endif
