#include "ai/AiThreadManifest.h"

#include "privacy/ProjectPrivacy.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace holder::ai {

std::string ai_thread_manifest_rel_path(const std::string& thread_id) {
  if (thread_id.size() < 4) {
    throw std::invalid_argument("thread_id too short for path sharding");
  }
  return "ai_threads/" + thread_id.substr(0, 2) + "/" + thread_id.substr(2, 2) + "/" +
         thread_id + ".json";
}

std::string render_ai_thread_manifest(
    const holder::model::Project& project,
    const holder::model::AiThread& thread
) {
  if (thread.thread_id.empty() || thread.project_id != project.project_id || thread.title.empty()) {
    throw std::invalid_argument("invalid AI thread manifest fields");
  }
  nlohmann::json body = {
      {"version", 1},
      {"thread_id", thread.thread_id},
      {"project_id", thread.project_id},
      {"title", thread.title},
      {"created_at", thread.created_at},
      {"updated_at", thread.updated_at},
  };
  body["card_id"] = thread.card_id.has_value() ? nlohmann::json(*thread.card_id)
                                                : nlohmann::json(nullptr);
  const auto plain = body.dump(2) + '\n';
  if (project.privacy_mode != "encrypted_git") return plain;
  if (!project.project_key_id.has_value() || project.project_key_id->empty()) {
    throw std::invalid_argument("encrypted project has no key id");
  }
  return holder::privacy::encrypt_project_blob(project.project_id, *project.project_key_id, plain);
}

holder::model::AiThread parse_ai_thread_manifest(
    const holder::model::Project& project,
    const std::string& raw
) {
  auto plain = raw;
  if (project.privacy_mode == "encrypted_git") {
    if (!project.project_key_id.has_value() || project.project_key_id->empty()) {
      throw std::invalid_argument("encrypted project has no key id");
    }
    plain = holder::privacy::decrypt_project_blob(project.project_id, *project.project_key_id, raw);
  }
  const auto body = nlohmann::json::parse(plain);
  if (body.value("version", 0) != 1) {
    throw std::runtime_error("unsupported AI thread manifest version");
  }
  holder::model::AiThread thread;
  thread.thread_id = body.at("thread_id").get<std::string>();
  thread.project_id = body.at("project_id").get<std::string>();
  thread.title = body.at("title").get<std::string>();
  thread.created_at = body.at("created_at").get<long long>();
  thread.updated_at = body.at("updated_at").get<long long>();
  if (body.contains("card_id") && !body.at("card_id").is_null()) {
    thread.card_id = body.at("card_id").get<std::string>();
  }
  if (thread.thread_id.empty() || thread.project_id != project.project_id || thread.title.empty()) {
    throw std::runtime_error("AI thread manifest does not match project");
  }
  return thread;
}

void write_ai_thread_manifest(
    holder::git::GitOps& git,
    const holder::model::Project& project,
    const holder::model::AiThread& thread
) {
  const auto rel_path = ai_thread_manifest_rel_path(thread.thread_id);
  git.open_or_init(project.root_path);
  git.write_file(rel_path, render_ai_thread_manifest(project, thread));
  git.stage_path(rel_path);
}

holder::model::AiThread read_ai_thread_manifest(
    const holder::model::Project& project,
    const std::filesystem::path& path
) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("failed to open AI thread manifest: " + path.string());
  std::ostringstream out;
  out << in.rdbuf();
  auto thread = parse_ai_thread_manifest(project, out.str());
  if (path.filename() != thread.thread_id + ".json") {
    throw std::runtime_error("AI thread manifest path does not match thread_id: " + path.string());
  }
  return thread;
}

} // namespace holder::ai
