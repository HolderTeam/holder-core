#include "history/ProjectHistory.h"

#include "ai/AiMessageFrontMatter.h"
#include "ai/AiThreadManifest.h"
#include "card/CardFrontMatter.h"
#include "git/GitRepo.h"
#include "privacy/ProjectPrivacy.h"
#include "project/ProjectManifest.h"
#include "resource/ResourceManifest.h"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace holder::history {
namespace {

bool starts_with(std::string_view value, std::string_view prefix) {
  return value.rfind(prefix, 0) == 0;
}

std::optional<std::string> decode_project_blob(
    const holder::model::Project& project,
    const std::string& raw
) {
  if (project.privacy_mode != "encrypted_git") return raw;
  if (!project.project_key_id.has_value() || project.project_key_id->empty()) {
    return std::nullopt;
  }
  return holder::privacy::decrypt_project_blob(project.project_id, *project.project_key_id, raw);
}

struct HistoricalDisplayMetadata {
  std::optional<std::string> title;
  std::optional<std::string> detail;
};

std::optional<std::string> milestone_summary(
    const std::vector<holder::model::Milestone>& milestones
) {
  if (milestones.empty()) return std::nullopt;
  constexpr std::size_t kShownMilestones = 3;
  std::string summary = milestones.size() == 1 ? "Milestone: " :
      "Milestones (" + std::to_string(milestones.size()) + "): ";
  for (std::size_t index = 0; index < milestones.size() && index < kShownMilestones; ++index) {
    if (index > 0) summary += ", ";
    const auto& milestone = milestones[index];
    summary += milestone.kind.value_or("Milestone");
    if (milestone.description.has_value() && !milestone.description->empty()) {
      summary += " — " + *milestone.description;
    }
  }
  if (milestones.size() > kShownMilestones) {
    summary += " +" + std::to_string(milestones.size() - kShownMilestones) + " more";
  }
  return summary;
}

std::optional<HistoricalDisplayMetadata> card_display_at(
    holder::git::GitRepo& repo,
    const holder::model::Project& project,
    const std::string& commit_oid,
    const std::string& path
) {
  try {
    const auto raw = repo.read_blob_at(commit_oid, path);
    if (!raw.has_value()) return std::nullopt;
    const auto decoded = decode_project_blob(project, *raw);
    if (!decoded.has_value() || decoded->find('\0') != std::string::npos) return std::nullopt;
    const auto parsed = holder::core::parse_card_file(*decoded);
    if (!parsed.has_front_matter || parsed.card.title.empty()) return std::nullopt;
    return HistoricalDisplayMetadata{parsed.card.title, milestone_summary(parsed.milestones)};
  } catch (const std::exception&) {
    // Project History remains useful when one historical card cannot be decoded.
    return std::nullopt;
  }
}

std::optional<std::string> resource_attachment_summary(const holder::model::ResourceBundle& bundle) {
  if (bundle.assets.empty()) return std::nullopt;
  constexpr std::size_t kShownAttachmentNames = 3;
  std::string summary = bundle.assets.size() == 1 ? "Attachment: " :
      "Attachments (" + std::to_string(bundle.assets.size()) + "): ";
  for (std::size_t index = 0; index < bundle.assets.size() && index < kShownAttachmentNames; ++index) {
    if (index > 0) summary += ", ";
    summary += bundle.assets[index].original_filename;
  }
  if (bundle.assets.size() > kShownAttachmentNames) {
    summary += " +" + std::to_string(bundle.assets.size() - kShownAttachmentNames) + " more";
  }
  return summary;
}

std::string first_meaningful_line(const std::string& text) {
  std::size_t start = 0;
  while (start < text.size()) {
    const auto end = text.find('\n', start);
    auto line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    line.erase(line.begin(), std::find_if(line.begin(), line.end(), [](unsigned char ch) {
      return std::isspace(ch) == 0;
    }));
    line.erase(std::find_if(line.rbegin(), line.rend(), [](unsigned char ch) {
      return std::isspace(ch) == 0;
    }).base(), line.end());
    if (!line.empty()) {
      if (line.size() > 80) line = line.substr(0, 77) + "...";
      return line;
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return {};
}

HistoricalDisplayMetadata ai_display_at(
    holder::git::GitRepo& repo,
    const holder::model::Project& project,
    const std::string& commit_oid,
    const std::string& path
) {
  HistoricalDisplayMetadata metadata;
  try {
    const auto raw = repo.read_blob_at(commit_oid, path);
    if (!raw.has_value()) return metadata;
    if (starts_with(path, "ai_threads/")) {
      metadata.title = holder::ai::parse_ai_thread_manifest(project, *raw).title;
      return metadata;
    }

    const auto decoded = decode_project_blob(project, *raw);
    if (!decoded.has_value() || decoded->find('\0') != std::string::npos) return metadata;
    const auto parsed = holder::core::parse_ai_message_file(*decoded);
    if (!parsed.has_front_matter || parsed.message.message_id.empty()) return metadata;
    const auto thread_path = holder::ai::ai_thread_manifest_rel_path(parsed.message.thread_id);
    const auto thread_raw = repo.read_blob_at(commit_oid, thread_path);
    if (thread_raw.has_value()) {
      metadata.title = holder::ai::parse_ai_thread_manifest(project, *thread_raw).title;
    }
    const auto excerpt = first_meaningful_line(parsed.body);
    if (!excerpt.empty()) {
      metadata.detail = (parsed.message.role.empty() ? "message" : parsed.message.role) + ": " + excerpt;
    }
  } catch (const std::exception&) {
    // AI data is optional enrichment; malformed or unavailable history stays path-only.
  }
  return metadata;
}

HistoricalDisplayMetadata project_settings_display_at(
    holder::git::GitRepo& repo,
    const holder::model::Project& project,
    const std::string& commit_oid,
    const std::string& path
) {
  HistoricalDisplayMetadata metadata;
  try {
    const auto raw = repo.read_blob_at(commit_oid, path);
    if (!raw.has_value()) return metadata;
    if (path == holder::project::kProjectBootstrapPath) {
      const auto bootstrap = nlohmann::json::parse(*raw);
      if (bootstrap.value("version", 0) != 1 ||
          bootstrap.value("project_id", std::string{}) != project.project_id) {
        return metadata;
      }
      const auto mode = bootstrap.value("mode", std::string{});
      if (mode != "plain" && mode != "encrypted_git") return metadata;
      metadata.title = "Privacy settings";
      metadata.detail = mode == "plain" ? "Mode: plain Git" : "Mode: encrypted Git";
      return metadata;
    }

    const auto decoded = decode_project_blob(project, *raw);
    if (!decoded.has_value() || decoded->find('\0') != std::string::npos) return metadata;
    const auto manifest = nlohmann::json::parse(*decoded);
    if (manifest.value("version", 0) != 1 ||
        manifest.value("project_id", std::string{}) != project.project_id) {
      return metadata;
    }
    const auto name = manifest.value("name", std::string{});
    if (name.empty()) return metadata;
    metadata.title = "Project settings";
    metadata.detail = "Project name: " + name;
    if (manifest.contains("git_provider") && manifest.at("git_provider").is_string()) {
      metadata.detail = *metadata.detail + " · Git provider: " +
          manifest.at("git_provider").get<std::string>();
    }
  } catch (const std::exception&) {
    // Settings metadata is optional enrichment and never blocks project History.
  }
  return metadata;
}

void resolve_display_metadata(
    holder::git::GitRepo& repo,
    const holder::model::Project& project,
    ProjectHistoryActivity& activity
) {
  for (auto& object : activity.affected_objects) {
    for (auto& item : object.items) {
      if (object.kind == ProjectHistoryObjectKind::Card) {
        const auto metadata = card_display_at(repo, project, activity.oid, item.path);
        if (metadata.has_value()) {
          item.title = metadata->title;
          item.detail = metadata->detail;
        }
        continue;
      }
      if (object.kind == ProjectHistoryObjectKind::AiData) {
        const auto metadata = ai_display_at(repo, project, activity.oid, item.path);
        item.title = metadata.title;
        item.detail = metadata.detail;
        continue;
      }
      if (object.kind == ProjectHistoryObjectKind::ProjectSettings) {
        const auto metadata = project_settings_display_at(repo, project, activity.oid, item.path);
        item.title = metadata.title;
        item.detail = metadata.detail;
        continue;
      }
      if (object.kind != ProjectHistoryObjectKind::Resource) continue;
      try {
        const auto raw = repo.read_blob_at(activity.oid, item.path);
        if (!raw.has_value()) continue;
        const auto decoded = decode_project_blob(project, *raw);
        if (!decoded.has_value() || decoded->find('\0') != std::string::npos) continue;
        const auto bundle = holder::resource::parse_resource_manifest(*decoded);
        item.title = bundle.resource.label;
        item.detail = resource_attachment_summary(bundle);
      } catch (const std::exception&) {
        // A malformed Resource manifest must not hide the rest of project History.
      }
    }
  }
}

} // namespace

ProjectHistoryObjectKind classify_project_history_path(std::string_view relative_path) {
  if (starts_with(relative_path, "cards/") || starts_with(relative_path, "trash/cards/")) {
    return ProjectHistoryObjectKind::Card;
  }
  if (starts_with(relative_path, "resources/")) return ProjectHistoryObjectKind::Resource;
  if (starts_with(relative_path, "locations/")) return ProjectHistoryObjectKind::Location;
  if (starts_with(relative_path, "ai_messages/") ||
      starts_with(relative_path, "trash/ai_messages/") ||
      starts_with(relative_path, "ai_threads/")) {
    return ProjectHistoryObjectKind::AiData;
  }
  if (relative_path == ".holder/project.json" || relative_path == ".holder/privacy.json") {
    return ProjectHistoryObjectKind::ProjectSettings;
  }
  return ProjectHistoryObjectKind::Unknown;
}

const char* project_history_object_kind_name(ProjectHistoryObjectKind kind) {
  switch (kind) {
    case ProjectHistoryObjectKind::Card: return "card";
    case ProjectHistoryObjectKind::Resource: return "resource";
    case ProjectHistoryObjectKind::Location: return "location";
    case ProjectHistoryObjectKind::AiData: return "ai_data";
    case ProjectHistoryObjectKind::ProjectSettings: return "project_settings";
    case ProjectHistoryObjectKind::Unknown: return "unknown";
  }
  return "unknown";
}

ProjectHistoryActivity group_project_history_activity(
    const std::string& oid,
    const std::vector<std::string>& parent_oids,
    const std::string& author_name,
    const std::string& author_email,
    long long authored_at,
    long long committed_at,
    const std::string& message,
    const std::vector<std::string>& changed_paths
) {
  ProjectHistoryActivity activity;
  activity.oid = oid;
  activity.parent_oids = parent_oids;
  activity.author_name = author_name;
  activity.author_email = author_email;
  activity.authored_at = authored_at;
  activity.committed_at = committed_at;
  activity.message = message;
  activity.is_merge = parent_oids.size() > 1;

  for (const auto& path : changed_paths) {
    const auto kind = classify_project_history_path(path);
    auto found = std::find_if(
        activity.affected_objects.begin(),
        activity.affected_objects.end(),
        [kind](const auto& object) { return object.kind == kind; }
    );
    if (found == activity.affected_objects.end()) {
      activity.affected_objects.push_back({kind, {{path, std::nullopt, std::nullopt}}});
    } else {
      found->items.push_back({path, std::nullopt, std::nullopt});
    }
  }
  return activity;
}

bool project_history_activity_matches(
    const ProjectHistoryActivity& activity,
    const std::optional<ProjectHistoryObjectKind>& kind_filter
) {
  if (!kind_filter.has_value()) return true;
  return std::any_of(
      activity.affected_objects.begin(),
      activity.affected_objects.end(),
      [&](const auto& object) { return object.kind == *kind_filter; }
  );
}

ProjectHistoryPage ProjectHistoryService::list(
    const holder::model::Project& project,
    std::size_t limit,
    const std::optional<std::string>& cursor,
    const std::optional<ProjectHistoryObjectKind>& kind_filter
) const {
  if (limit == 0 || limit > 200) {
    throw std::invalid_argument("history limit must be between 1 and 200");
  }

  holder::git::GitRepo repo;
  repo.open_existing(project.root_path);
  ProjectHistoryPage page;
  page.head_oid = repo.head_oid();
  auto raw_cursor = cursor;
  constexpr std::size_t kHistoryBatchSize = 64;

  while (true) {
    const auto batch = repo.history_all(
        std::max(kHistoryBatchSize, limit), raw_cursor, max_scanned_commits_
    );
    if (batch.commits.empty()) {
      if (batch.scan_limited) {
        page.scan_limited = true;
        page.next_cursor = batch.scan_cursor;
      }
      return page;
    }

    for (std::size_t index = 0; index < batch.commits.size(); ++index) {
      const auto& commit = batch.commits[index];
      const auto activity = group_project_history_activity(
          commit.oid,
          commit.parent_oids,
          commit.author_name.empty() ? "Holder" : commit.author_name,
          commit.author_email,
          commit.authored_at,
          commit.committed_at,
          commit.message,
          commit.changed_paths
      );
      auto titled_activity = activity;
      resolve_display_metadata(repo, project, titled_activity);
      if (!project_history_activity_matches(titled_activity, kind_filter)) continue;
      page.activities.push_back(std::move(titled_activity));
      if (page.activities.size() == limit) {
        if (index + 1 < batch.commits.size() || batch.has_more || batch.scan_limited) {
          page.next_cursor = commit.oid;
        }
        if (batch.scan_limited) page.scan_limited = true;
        return page;
      }
    }
    if (batch.scan_limited) {
      page.scan_limited = true;
      page.next_cursor = batch.scan_cursor;
      return page;
    }
    if (!batch.has_more) return page;
    raw_cursor = batch.commits.back().oid;
  }
}

} // namespace holder::history
