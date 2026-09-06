#include "history/ProjectHistory.h"

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

} // namespace holder::history
