#pragma once

#include "model/Project.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

struct ProjectHistoryAffectedPath {
  std::string path;
  std::optional<std::string> title;
  std::optional<std::string> detail;
};

struct ProjectHistoryAffectedObject {
  ProjectHistoryObjectKind kind = ProjectHistoryObjectKind::Unknown;
  std::vector<ProjectHistoryAffectedPath> items;
};

// One Git commit is one project activity. Its changed paths are grouped by
// Holder object kind, making a multi-file operation (such as an attachment)
// one truthful activity with several affected objects.
struct ProjectHistoryActivity {
  std::string oid;
  std::vector<std::string> parent_oids;
  std::string author_name;
  std::string author_email;
  long long authored_at = 0;
  long long committed_at = 0;
  std::string message;
  std::vector<ProjectHistoryAffectedObject> affected_objects;
  bool is_merge = false;
};

struct ProjectHistoryPage {
  std::optional<std::string> head_oid;
  std::vector<ProjectHistoryActivity> activities;
  std::optional<std::string> next_cursor;
  bool scan_limited = false;
};

ProjectHistoryActivity group_project_history_activity(
    const std::string& oid,
    const std::vector<std::string>& parent_oids,
    const std::string& author_name,
    const std::string& author_email,
    long long authored_at,
    long long committed_at,
    const std::string& message,
    const std::vector<std::string>& changed_paths
);

bool project_history_activity_matches(
    const ProjectHistoryActivity& activity,
    const std::optional<ProjectHistoryObjectKind>& kind_filter
);

class ProjectHistoryService {
 public:
  explicit ProjectHistoryService(std::size_t max_scanned_commits = 10'000)
      : max_scanned_commits_(max_scanned_commits) {}

  ProjectHistoryPage list(
      const holder::model::Project& project,
      std::size_t limit = 50,
      const std::optional<std::string>& cursor = std::nullopt, // LCOV_EXCL_LINE - gcov artefact: default argument is executed but never counted.
      const std::optional<ProjectHistoryObjectKind>& kind_filter = std::nullopt
  ) const;

 private:
  std::size_t max_scanned_commits_;
};

} // namespace holder::history
