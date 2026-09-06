#pragma once

#include <string_view>

namespace holder::history {

// The durable, project-owned paths that can be shown prominently in project
// history. Everything else stays Unknown so a future renderer can retain it
// without inventing Holder semantics for arbitrary Git files.
enum class ProjectHistoryObjectKind {
  Card,
  Resource,
  Location,
  AiData,
  ProjectSettings,
  Unknown,
};

ProjectHistoryObjectKind classify_project_history_path(std::string_view relative_path);
const char* project_history_object_kind_name(ProjectHistoryObjectKind kind);

} // namespace holder::history
