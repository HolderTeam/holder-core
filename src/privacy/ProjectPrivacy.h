#pragma once

#include "git/GitOps.h"
#include "privacy/PrivacyError.h"
#include "project/ProjectRepo.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace holder::privacy {

struct RecoveryTokenMetadata {
  std::string project_id;
  std::string project_key_id;
  std::optional<std::string> project_name;
  std::optional<std::string> git_remote_url;
};

struct EncryptionSafetyCheck {
  bool ok = true;
  std::size_t checked_files = 0;
  std::vector<std::string> unsafe_paths;
  std::string message;
};

void ensure_encrypted_git_setup(
    holder::git::GitOps& git,
    const std::string& root_path,
    const std::string& project_id,
    const std::string& project_key_id
);

std::string ensure_project_key_material(
    holder::project::ProjectRepo& repo,
    const std::string& project_id,
    const std::optional<std::string>& project_key_id,
    long long updated_at,
    const std::function<std::string()>& uuid_v4
);

void ensure_encrypted_project_ready(
    holder::git::GitOps& git,
    holder::project::ProjectRepo& repo,
    const std::string& project_id,
    const std::string& root_path,
    const std::optional<std::string>& project_key_id,
    long long updated_at,
    const std::function<std::string()>& uuid_v4
);

std::string export_recovery_token(
    const std::string& project_id,
    const std::string& project_key_id,
    const std::string& pin,
    const std::optional<std::string>& project_name = std::nullopt,
    const std::optional<std::string>& git_remote_url = std::nullopt
);

std::string encrypt_project_blob(
    const std::string& project_id,
    const std::string& project_key_id,
    const std::string& plaintext
);

std::string decrypt_project_blob(
    const std::string& project_id,
    const std::string& project_key_id,
    const std::string& envelope
);

void import_recovery_token(
    holder::project::ProjectRepo& repo,
    const std::string& project_id,
    const std::string& pin,
    const std::string& token_json,
    long long updated_at
);

RecoveryTokenMetadata inspect_recovery_token(const std::string& pin, const std::string& token_json);

EncryptionSafetyCheck run_encryption_safety_check(const std::string& root_path);

void assert_encryption_push_safe(const std::string& root_path);

void assert_encryption_index_paths_safe(
    const std::string& root_path,
    const std::vector<std::string>& relative_paths
);

} // namespace holder::privacy
