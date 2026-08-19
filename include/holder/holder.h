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

void holder_string_free(char* value);

const char* holder_error_message(const holder_error* error);
void holder_error_destroy(holder_error* error);

#ifdef __cplusplus
}
#endif
