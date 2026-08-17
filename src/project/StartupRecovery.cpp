#include "project/StartupRecovery.h"

#include "privacy/PrivacyError.h"
#include "project/ProjectRepo.h"
#include "project/Rebuilder.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace holder::project {
namespace {

bool should_retry_plain_recovery(
    const holder::privacy::PrivacyError& ex,
    const holder::model::Project& project
) {
  if (project.privacy_mode != "encrypted_git") {
    return false; // LCOV_EXCL_LINE
  }
  switch (ex.code()) {
  case holder::privacy::PrivacyErrorCode::EnvelopeInvalid:
  case holder::privacy::PrivacyErrorCode::KeyMaterialMissing:
    return true;
  default:
    return false;
  }
}

long long now_epoch_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch()
  )
      .count();
}

bool looks_like_project_root(const std::filesystem::path& root) {
  return std::filesystem::is_directory(root) &&
         (std::filesystem::exists(root / "cards") ||
          std::filesystem::exists(root / "trash" / "cards") ||
          std::filesystem::exists(root / "ai_messages") ||
          std::filesystem::exists(root / ".holder" / "privacy.json"));
}

std::string derive_project_name_from_root(const std::filesystem::path& root) {
  std::string name = root.filename().string();
  if (name.empty()) return "Project";

  for (char& ch : name) {
    if (ch == '-' || ch == '_') ch = ' ';
  }

  bool capitalize = true;
  for (char& ch : name) {
    if (std::isspace(static_cast<unsigned char>(ch))) {
      capitalize = true;
      continue;
    }
    ch = capitalize ? static_cast<char>(std::toupper(static_cast<unsigned char>(ch)))
                    : static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    capitalize = false;
  }
  return name;
}

void load_privacy_metadata(holder::model::Project& project) {
  const auto path = std::filesystem::path(project.root_path) / ".holder" / "privacy.json";
  if (!std::filesystem::exists(path)) {
    project.privacy_mode = "plain";
    project.project_key_id.reset();
    return;
  }

  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("failed to open privacy metadata: " + path.string()); // LCOV_EXCL_LINE
  }

  std::ostringstream buffer;
  buffer << in.rdbuf();
  const auto body = nlohmann::json::parse(buffer.str());
  if (body.contains("project_id") && body["project_id"].is_string() &&
      !body["project_id"].get<std::string>().empty()) {
    project.project_id = body["project_id"].get<std::string>();
  }
  project.privacy_mode = body.value("mode", std::string("plain"));
  if (body.contains("key_id") && body["key_id"].is_string()) {
    project.project_key_id = body["key_id"].get<std::string>();
  } else {
    project.project_key_id.reset();
  }
}

} // namespace

std::vector<holder::model::Project> recover_projects_from_disk(
    holder::platform::Db& db,
    holder::index::FtsIndexer* fts,
    const std::filesystem::path& projects_root,
    const std::function<std::string()>& uuid_v4
) {
  std::vector<holder::model::Project> recovered;
  if (!std::filesystem::exists(projects_root) || !std::filesystem::is_directory(projects_root)) {
    return recovered;
  }

  std::vector<std::filesystem::path> roots;
  for (const auto& entry : std::filesystem::directory_iterator(projects_root)) {
    if (looks_like_project_root(entry.path())) {
      roots.push_back(entry.path());
    }
  }
  std::sort(roots.begin(), roots.end());

  holder::project::ProjectRepo repo(db);
  holder::store::Rebuilder rebuilder(db, fts, nullptr, true);
  for (const auto& root : roots) {
    holder::model::Project project;
    project.project_id = uuid_v4();
    project.name = derive_project_name_from_root(root);
    project.root_path = root.string();
    project.created_at = now_epoch_seconds();
    project.updated_at = project.created_at;

    try {
      load_privacy_metadata(project);
      repo.create(project);
      rebuilder.rebuild_project(project);
      recovered.push_back(project);
      spdlog::info("Recovered project from disk: {} ({})", project.name, project.root_path);
    } catch (const holder::privacy::PrivacyError& ex) {
      if (should_retry_plain_recovery(ex, project)) {
        repo.remove(project.project_id);

        auto fallback = project;
        fallback.privacy_mode = "plain";
        fallback.project_key_id.reset();
        try {
          repo.create(fallback);
          rebuilder.rebuild_project(fallback);
          recovered.push_back(fallback);
          spdlog::warn(
              "Recovered project as plain after envelope mismatch: {} ({})",
              fallback.name,
              fallback.root_path
          );
        } catch (const std::exception& retry_ex) {
          repo.remove(fallback.project_id);
          spdlog::warn("Skipping project recovery at {}: {}", root.string(), retry_ex.what());
        }
      } else {
        repo.remove(project.project_id);
        spdlog::warn("Skipping project recovery at {}: {}", root.string(), ex.what());
      }
    } catch (const std::exception& ex) {
      repo.remove(project.project_id);
      spdlog::warn("Skipping project recovery at {}: {}", root.string(), ex.what());
    }
  }

  return recovered;
}

} // namespace holder::project
