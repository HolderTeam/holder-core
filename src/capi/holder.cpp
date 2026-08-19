#include "holder/holder.h"

#include "card/CardRepo.h"
#include "card/CardStore.h"
#include "platform/Db.h"
#include "project/DefaultProject.h"
#include "project/ProjectRepo.h"
#include "project/ProjectStore.h"

#include <nlohmann/json.hpp>
#include <sodium.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <utility>

struct holder_context {
  std::filesystem::path data_dir;
  std::filesystem::path db_path;
  holder::platform::Db db;
};

struct holder_error {
  std::string message;
};

namespace {

void clear_error(holder_error** out_error) {
  if (out_error != nullptr) {
    *out_error = nullptr;
  }
}

int set_error(holder_error** out_error, int code, std::string message) {
  if (out_error != nullptr) {
    try {
      *out_error = new holder_error{std::move(message)};
    } catch (...) {
      *out_error = nullptr;
      return HOLDER_ERROR_ALLOCATION;
    }
  }
  return code;
}

int set_exception(holder_error** out_error, const std::exception& e) {
  return set_error(out_error, HOLDER_ERROR_RUNTIME, e.what());
}

int set_unknown_exception(holder_error** out_error) {
  return set_error(out_error, HOLDER_ERROR_RUNTIME, "unknown holder error");
}

char* duplicate_string(const std::string& value) {
  auto* out = static_cast<char*>(std::malloc(value.size() + 1));
  if (out == nullptr) {
    return nullptr;
  }
  std::memcpy(out, value.c_str(), value.size() + 1);
  return out;
}

nlohmann::json project_to_json(const holder::model::Project& project) {
  nlohmann::json body = {
      {"project_id", project.project_id},
      {"name", project.name},
      {"root_path", project.root_path},
      {"privacy_mode", project.privacy_mode},
      {"created_at", project.created_at},
      {"updated_at", project.updated_at},
  };

  body["git_remote_url"] = project.git_remote_url.has_value()
                               ? nlohmann::json(*project.git_remote_url)
                               : nlohmann::json(nullptr);
  body["git_provider"] = project.git_provider.has_value()
                             ? nlohmann::json(*project.git_provider)
                             : nlohmann::json(nullptr);
  body["project_key_id"] = project.project_key_id.has_value()
                               ? nlohmann::json(*project.project_key_id)
                               : nlohmann::json(nullptr);
  return body;
}

nlohmann::json card_to_json(const holder::model::Card& card) {
  nlohmann::json body = {
      {"card_id", card.card_id},
      {"project_id", card.project_id},
      {"title", card.title},
      {"rel_path", card.rel_path},
      {"sort_key", card.sort_key},
      {"created_at", card.created_at},
      {"updated_at", card.updated_at},
  };

  body["parent_card_id"] = card.parent_card_id.has_value()
                               ? nlohmann::json(*card.parent_card_id)
                               : nlohmann::json(nullptr);
  body["deleted_at"] = card.deleted_at.has_value()
                           ? nlohmann::json(*card.deleted_at)
                           : nlohmann::json(nullptr);
  return body;
}

long long now_epoch_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch()
  )
      .count();
}

std::string uuid_v4() {
  if (sodium_init() < 0) {
    throw std::runtime_error("failed to initialize libsodium"); // LCOV_EXCL_LINE
  }
  unsigned char bytes[16];
  randombytes_buf(bytes, sizeof(bytes));
  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40); // version 4
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80); // variant 10xx

  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(36);
  for (int i = 0; i < 16; ++i) {
    out.push_back(kHex[bytes[i] >> 4]);
    out.push_back(kHex[bytes[i] & 0x0F]);
    if (i == 3 || i == 5 || i == 7 || i == 9) {
      out.push_back('-');
    }
  }
  return out;
}

} // namespace

int holder_context_open(
    const char* data_dir,
    const char* schema_sql,
    holder_context** out_context,
    holder_error** out_error
) {
  clear_error(out_error);
  if (out_context == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "out_context must not be null");
  }
  *out_context = nullptr;

  if (data_dir == nullptr || data_dir[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "data_dir must not be empty");
  }

  try {
    auto context = std::make_unique<holder_context>();
    context->data_dir = std::filesystem::path(data_dir);
    context->db_path = context->data_dir / "server" / "holder.db";
    std::filesystem::create_directories(context->db_path.parent_path());
    context->db.open(context->db_path);
    if (schema_sql != nullptr && schema_sql[0] != '\0') {
      context->db.exec(schema_sql);
    }

    *out_context = context.release();
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);
  }
}

void holder_context_destroy(holder_context* context) {
  delete context;
}

int holder_project_list(holder_context* context, char** out_json, holder_error** out_error) {
  clear_error(out_error);
  if (out_json == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "out_json must not be null");
  }
  *out_json = nullptr;

  if (context == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "context must not be null");
  }

  try {
    holder::project::ProjectRepo repo(context->db);
    nlohmann::json body = nlohmann::json::array();
    for (const auto& project : repo.list()) {
      body.push_back(project_to_json(project));
    }

    auto* out = duplicate_string(body.dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);
  }
}

int holder_card_list(
    holder_context* context,
    const char* project_id,
    char** out_json,
    holder_error** out_error
) {
  clear_error(out_error);
  if (out_json == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "out_json must not be null");
  }
  *out_json = nullptr;

  if (context == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "context must not be null");
  }
  if (project_id == nullptr || project_id[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "project_id must not be empty");
  }

  try {
    holder::card::CardRepo repo(context->db);
    nlohmann::json body = nlohmann::json::array();
    for (const auto& card : repo.list_all(project_id)) {
      body.push_back(card_to_json(card));
    }

    auto* out = duplicate_string(body.dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);
  }
}

int holder_card_get_content(
    holder_context* context,
    const char* card_id,
    char** out_content,
    holder_error** out_error
) {
  clear_error(out_error);
  if (out_content == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "out_content must not be null");
  }
  *out_content = nullptr;

  if (context == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "context must not be null");
  }
  if (card_id == nullptr || card_id[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "card_id must not be empty");
  }

  try {
    holder::card::CardRepo repo(context->db);
    const auto card = repo.get(card_id);
    if (!card.has_value()) {
      return set_error(out_error, HOLDER_ERROR_RUNTIME, "card not found: " + std::string(card_id));
    }

    holder::card::CardStore store(context->db, nullptr);
    const auto content = store.get_content(*card);
    if (!content.has_value()) {
      return set_error(out_error, HOLDER_ERROR_RUNTIME, "card content missing");
    }

    auto* out = duplicate_string(*content);
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
    }

    *out_content = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);
  }
}

int holder_project_create(
    holder_context* context,
    const char* name,
    const char* root_path,
    const char* privacy_mode,
    char** out_json,
    holder_error** out_error
) {
  clear_error(out_error);
  if (out_json == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "out_json must not be null");
  }
  *out_json = nullptr;

  if (context == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "context must not be null");
  }
  if (name == nullptr || name[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "name must not be empty");
  }

  try {
    holder::model::Project project;
    project.name = name;
    if (root_path != nullptr && root_path[0] != '\0') {
      project.root_path = root_path;
    }
    project.privacy_mode =
        (privacy_mode != nullptr && privacy_mode[0] != '\0') ? std::string(privacy_mode) : "plain";

    holder::project::ProjectStore store(context->db);
    const auto created =
        store.create(std::move(project), uuid_v4, context->data_dir / "projects");

    auto* out = duplicate_string(project_to_json(created).dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);
  }
}

int holder_card_create(
    holder_context* context,
    const char* project_id,
    const char* title,
    const char* content,
    const char* parent_card_id,
    char** out_json,
    holder_error** out_error
) {
  clear_error(out_error);
  if (out_json == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "out_json must not be null");
  }
  *out_json = nullptr;

  if (context == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "context must not be null");
  }
  if (project_id == nullptr || project_id[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "project_id must not be empty");
  }
  if (title == nullptr || title[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "title must not be empty");
  }

  try {
    holder::model::Card card;
    card.card_id = uuid_v4();
    card.project_id = project_id;
    card.title = title;
    card.created_at = now_epoch_seconds();
    card.updated_at = card.created_at;
    if (parent_card_id != nullptr && parent_card_id[0] != '\0') {
      card.parent_card_id = std::string(parent_card_id);
    }

    holder::card::CardStore store(context->db, nullptr);
    store.create(card, content != nullptr ? std::string(content) : std::string());

    holder::card::CardRepo repo(context->db);
    const auto created = repo.get(card.card_id);
    auto* out = duplicate_string(card_to_json(created.value()).dump());
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);
  }
}

int holder_ensure_default_project(
    holder_context* context,
    const char* name,
    const char* welcome_title,
    const char* welcome_content,
    char** out_json,
    holder_error** out_error
) {
  clear_error(out_error);
  if (out_json == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "out_json must not be null");
  }
  *out_json = nullptr;

  if (context == nullptr) {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "context must not be null");
  }
  if (name == nullptr || name[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "name must not be empty");
  }
  if (welcome_title == nullptr || welcome_title[0] == '\0') {
    return set_error(out_error, HOLDER_ERROR_INVALID_ARGUMENT, "welcome_title must not be empty");
  }

  try {
    const auto created = holder::project::ensure_default_project(
        context->db,
        name,
        "plain",
        welcome_title,
        welcome_content != nullptr ? std::string(welcome_content) : std::string(),
        uuid_v4,
        context->data_dir / "projects"
    );

    auto* out = duplicate_string(
        created.has_value() ? project_to_json(created.value()).dump() : std::string("null")
    );
    if (out == nullptr) {
      return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
    }

    *out_json = out;
    return HOLDER_OK;
  } catch (const std::bad_alloc&) {
    return set_error(out_error, HOLDER_ERROR_ALLOCATION, "allocation failed");
  } catch (const std::exception& e) {
    return set_exception(out_error, e);
  } catch (...) {
    return set_unknown_exception(out_error);
  }
}

void holder_string_free(char* value) {
  std::free(value);
}

const char* holder_error_message(const holder_error* error) {
  if (error == nullptr) {
    return "";
  }
  return error->message.c_str();
}

void holder_error_destroy(holder_error* error) {
  delete error;
}
