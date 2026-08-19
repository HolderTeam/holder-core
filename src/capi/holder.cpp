#include "holder/holder.h"

#include "card/CardRepo.h"
#include "platform/Db.h"
#include "project/ProjectRepo.h"

#include <nlohmann/json.hpp>

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
