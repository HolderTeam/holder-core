#include "project/StartupRecovery.h"

#include "project/ProjectManifest.h"
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
          std::filesystem::exists(root / "resources") ||
          std::filesystem::exists(root / "locations") ||
          std::filesystem::exists(root / ".holder" / "privacy.json") ||
          std::filesystem::exists(root / ".holder" / "project.json"));
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
    const std::function<std::string()>& uuid_v4,
    bool require_durable_manifest
) {
  if (!std::filesystem::exists(projects_root) || !std::filesystem::is_directory(projects_root)) {
    return {};
  }

  std::vector<std::filesystem::path> roots;
  for (const auto& entry : std::filesystem::directory_iterator(projects_root)) {
    if (looks_like_project_root(entry.path())) {
      roots.push_back(entry.path());
    }
  }
  return recover_project_roots(db, fts, std::move(roots), uuid_v4, require_durable_manifest);
}

std::vector<holder::model::Project> recover_project_roots(
    holder::platform::Db& db,
    holder::index::FtsIndexer* fts,
    std::vector<std::filesystem::path> roots,
    const std::function<std::string()>& uuid_v4,
    bool require_durable_manifest
) {
  std::vector<holder::model::Project> recovered;
  std::sort(roots.begin(), roots.end());
  roots.erase(std::unique(roots.begin(), roots.end()), roots.end());

  holder::project::ProjectRepo repo(db);
  holder::store::Rebuilder rebuilder(db, fts, nullptr, true, require_durable_manifest);
  for (const auto& root : roots) {
    holder::model::Project project;
    bool project_created = false;

    try {
      if (has_project_manifest(root)) {
        project = read_project_manifest(root);
      } else {
        if (require_durable_manifest) {
          throw std::runtime_error("project has no durable manifest: " + root.string());
        }
        project.project_id = uuid_v4();
        project.name = derive_project_name_from_root(root);
        project.root_path = root.string();
        project.created_at = now_epoch_seconds();
        project.updated_at = project.created_at;
        load_privacy_metadata(project);
        spdlog::warn(
            "Recovering legacy project without durable metadata: {}. "
            "Its identity may not be recoverable after database loss.",
            root.string()
        );
      }
      if (const auto existing = repo.get(project.project_id); existing.has_value()) {
        if (std::filesystem::path(existing->root_path).lexically_normal() !=
            root.lexically_normal()) {
          throw std::runtime_error(
              "duplicate project_id discovered at different roots: " + project.project_id
          );
        }
        continue;
      }
      repo.create(project);
      project_created = true;
      rebuilder.rebuild_project(project);
      recovered.push_back(project);
      spdlog::info("Recovered project from disk: {} ({})", project.name, project.root_path);
    } catch (const std::exception& ex) {
      if (project_created) {
        repo.remove(project.project_id);
      }
      if (require_durable_manifest) {
        throw;
      }
      spdlog::warn("Skipping project recovery at {}: {}", root.string(), ex.what());
    }
  }

  return recovered;
}

} // namespace holder::project
