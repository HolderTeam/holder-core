#include "history/ProjectHistory.h"

#include "git/GitRepo.h"

#include <algorithm>
#include <stdexcept>

namespace holder::history {
namespace {

bool starts_with(std::string_view value, std::string_view prefix) {
  return value.rfind(prefix, 0) == 0;
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
      activity.affected_objects.push_back({kind, {path}});
    } else {
      found->paths.push_back(path);
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
      if (!project_history_activity_matches(activity, kind_filter)) continue;
      page.activities.push_back(activity);
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
