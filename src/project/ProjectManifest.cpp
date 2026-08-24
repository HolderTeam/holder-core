#include "project/ProjectManifest.h"

#include "privacy/PrivacyError.h"
#include "privacy/ProjectPrivacy.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace holder::project {
namespace {

std::string read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to open project metadata: " + path.string());
  }
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::string required_nonempty_string(
    const nlohmann::json& body,
    const char* field,
    const std::filesystem::path& path
) {
  if (!body.contains(field) || !body.at(field).is_string()) {
    throw std::runtime_error(
        "project metadata field '" + std::string(field) + "' is missing: " + path.string()
    );
  }
  auto value = body.at(field).get<std::string>();
  if (value.empty()) {
    throw std::runtime_error(
        "project metadata field '" + std::string(field) + "' is empty: " + path.string()
    );
  }
  return value;
}

void require_version_one(const nlohmann::json& body, const std::filesystem::path& path) {
  if (!body.contains("version") || !body.at("version").is_number_integer() ||
      body.at("version").get<int>() != 1) {
    throw std::runtime_error("unsupported project metadata version: " + path.string());
  }
}

} // namespace

std::string render_project_bootstrap(const holder::model::Project& project) {
  if (project.project_id.empty()) {
    throw std::invalid_argument("project_id must not be empty");
  }
  if (project.privacy_mode != "plain" && project.privacy_mode != "encrypted_git") {
    throw std::invalid_argument("unsupported project privacy mode: " + project.privacy_mode);
  }
  if (project.privacy_mode == "encrypted_git" &&
      (!project.project_key_id.has_value() || project.project_key_id->empty())) {
    throw std::invalid_argument("encrypted project must have project_key_id");
  }

  nlohmann::json body = {
      {"version", 1},
      {"project_id", project.project_id},
      {"mode", project.privacy_mode},
  };
  if (project.project_key_id.has_value() && !project.project_key_id->empty()) {
    body["key_id"] = *project.project_key_id;
  }
  return body.dump(2) + '\n';
}

std::string render_project_manifest(const holder::model::Project& project) {
  if (project.project_id.empty() || project.name.empty()) {
    throw std::invalid_argument("project manifest requires project_id and name");
  }

  nlohmann::json body = {
      {"version", 1},
      {"project_id", project.project_id},
      {"name", project.name},
      {"created_at", project.created_at},
      {"updated_at", project.updated_at},
  };
  if (project.git_provider.has_value()) {
    body["git_provider"] = *project.git_provider;
  }
  if (project.git_remote_url.has_value()) {
    body["git_remote_url"] = *project.git_remote_url;
  }

  const auto plain = body.dump(2) + '\n';
  if (project.privacy_mode != "encrypted_git") {
    return plain;
  }
  if (!project.project_key_id.has_value() || project.project_key_id->empty()) {
    throw std::invalid_argument("encrypted project must have project_key_id");
  }
  return holder::privacy::encrypt_project_blob(
      project.project_id,
      *project.project_key_id,
      plain
  );
}

void write_project_manifest(
    holder::git::GitOps& git,
    const holder::model::Project& project
) {
  git.open_or_init(project.root_path);
  git.write_file(kProjectBootstrapPath, render_project_bootstrap(project));
  git.write_file(kProjectManifestPath, render_project_manifest(project));
  git.stage_path(kProjectBootstrapPath);
  git.stage_path(kProjectManifestPath);
}

holder::model::Project read_project_manifest(const std::filesystem::path& root_path) {
  const auto bootstrap_path = root_path / kProjectBootstrapPath;
  const auto manifest_path = root_path / kProjectManifestPath;

  const auto bootstrap = nlohmann::json::parse(read_file(bootstrap_path));
  require_version_one(bootstrap, bootstrap_path);

  holder::model::Project project;
  project.root_path = root_path.string();
  project.project_id = required_nonempty_string(bootstrap, "project_id", bootstrap_path);
  project.privacy_mode = required_nonempty_string(bootstrap, "mode", bootstrap_path);
  if (project.privacy_mode != "plain" && project.privacy_mode != "encrypted_git") {
    throw std::runtime_error(
        "unsupported project privacy mode '" + project.privacy_mode + "': " +
        bootstrap_path.string()
    );
  }
  if (bootstrap.contains("key_id") && bootstrap.at("key_id").is_string() &&
      !bootstrap.at("key_id").get<std::string>().empty()) {
    project.project_key_id = bootstrap.at("key_id").get<std::string>();
  }
  if (project.privacy_mode == "encrypted_git" && !project.project_key_id.has_value()) {
    throw holder::privacy::PrivacyError(
        holder::privacy::PrivacyErrorCode::KeyMaterialMissing,
        "encrypted project bootstrap has no key_id: " + bootstrap_path.string()
    );
  }

  auto manifest_text = read_file(manifest_path);
  if (project.privacy_mode == "encrypted_git") {
    manifest_text = holder::privacy::decrypt_project_blob(
        project.project_id,
        *project.project_key_id,
        manifest_text
    );
  }
  const auto manifest = nlohmann::json::parse(manifest_text);
  require_version_one(manifest, manifest_path);
  const auto manifest_project_id = required_nonempty_string(manifest, "project_id", manifest_path);
  if (manifest_project_id != project.project_id) {
    throw std::runtime_error("project manifest id does not match bootstrap: " + manifest_path.string());
  }
  project.name = required_nonempty_string(manifest, "name", manifest_path);
  project.created_at = manifest.at("created_at").get<long long>();
  project.updated_at = manifest.at("updated_at").get<long long>();
  if (manifest.contains("git_provider") && manifest.at("git_provider").is_string()) {
    project.git_provider = manifest.at("git_provider").get<std::string>();
  }
  if (manifest.contains("git_remote_url") && manifest.at("git_remote_url").is_string()) {
    project.git_remote_url = manifest.at("git_remote_url").get<std::string>();
  }
  return project;
}

bool has_project_manifest(const std::filesystem::path& root_path) {
  return std::filesystem::is_regular_file(root_path / kProjectBootstrapPath) &&
         std::filesystem::is_regular_file(root_path / kProjectManifestPath);
}

} // namespace holder::project
