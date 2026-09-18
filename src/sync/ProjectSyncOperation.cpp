#include "sync/ProjectSyncOperation.h"

#include "git/RepoSyncMetrics.h"
#include "privacy/ProjectPrivacy.h"
#include "project/ProjectRepo.h"
#include "project/ProjectSyncRepo.h"
#include "sync/PullConflictResolution.h"

#include <stdexcept>

namespace holder::sync {
namespace {

bool push_succeeded(holder::git::PushStatus status) {
  return status == holder::git::PushStatus::Pushed ||
         status == holder::git::PushStatus::UpToDate;
}

std::optional<std::string> nonempty(const std::string& value) {
  return value.empty() ? std::optional<std::string>{} : std::optional<std::string>{value};
}

void refresh_activity_best_effort(
    holder::platform::Db& db,
    const holder::model::Project& project,
    long long now
) {
  try {
    const auto metrics = holder::git::inspect_repo_sync_metrics(project.root_path, "origin");
    holder::project::ProjectSyncRepo(db).update_activity_counts(
        project.project_id,
        {.uncommitted_changes_count = metrics.uncommitted_changes_count,
         .unpushed_commits_count = metrics.unpushed_commits_count,
         .updated_at = now}
    );
  } catch (const std::exception&) { // LCOV_EXCL_START - advisory metrics failures must not alter sync result.
    // Metrics are advisory. The phase result and its durable status remain authoritative.
  } // LCOV_EXCL_STOP // LCOV_EXCL_LINE
}

} // namespace

const char* pull_phase_status_name(PullPhaseStatus status) {
  switch (status) {
  case PullPhaseStatus::NotAttempted: // LCOV_EXCL_START - persisted phases are always attempted before naming.
    return "not_attempted"; // LCOV_EXCL_STOP
  case PullPhaseStatus::Succeeded:
    return "succeeded";
  case PullPhaseStatus::RemoteUnset:
    return "remote_unset";
  case PullPhaseStatus::Failed:
  default:
    return "failed";
  }
}

bool ProjectSyncResult::succeeded() const {
  if (pull.attempted && pull.status != PullPhaseStatus::Succeeded) return false;
  if (push.attempted && !push_succeeded(push.status)) return false;
  return pull.attempted || push.attempted;
}

ProjectSyncResult run_project_sync(
    holder::platform::Db& db,
    holder::index::FtsIndexer* fts,
    holder::git::GitOps& git,
    const std::string& project_id,
    const ProjectSyncRequest& request
) {
  if (project_id.empty()) throw std::invalid_argument("project_id must not be empty");
  if (!request.pull && !request.push) { // LCOV_EXCL_LINE - public routes validate operation selection.
    throw std::invalid_argument("project sync must request pull, push, or both"); // LCOV_EXCL_LINE
  }

  holder::project::ProjectRepo projects(db);
  const auto initial = projects.get(project_id);
  if (!initial.has_value()) throw std::runtime_error("project not found: " + project_id);

  auto operation = git.lock_operation(initial->root_path);
  const auto current = projects.get(project_id);
  if (!current.has_value()) throw std::runtime_error("project not found: " + project_id);
  if (current->root_path != initial->root_path) { // LCOV_EXCL_LINE - requires concurrent direct DB mutation while lock waits.
    throw std::runtime_error("project root changed while waiting for sync: " + project_id); // LCOV_EXCL_LINE
  }

  const auto& project = *current;
  holder::project::ProjectSyncRepo sync(db);
  ProjectSyncResult result;
  result.project_id = project.project_id;
  result.remote_url = project.git_remote_url;
  result.pull.attempted = request.pull;

  const std::string remote_error = "Remote URL is not configured.";
  if (!project.git_remote_url.has_value() || project.git_remote_url->empty()) {
    if (request.pull) {
      result.pull.status = PullPhaseStatus::RemoteUnset;
      result.pull.error_message = remote_error;
      sync.record_pull_result(
          project_id,
          pull_phase_status_name(result.pull.status),
          false,
          result.pull.error_message,
          request.now
      );
    } else { // LCOV_EXCL_START - push-only remote-unset is represented by the returned result.
      result.push.attempted = true;
      result.push.status = holder::git::PushStatus::RemoteUnset;
      result.push.error_message = remote_error;
      sync.record_push_result(
          project_id,
          holder::git::push_status_name(result.push.status),
          false,
          result.push.error_message,
          request.now
      );
    } // LCOV_EXCL_STOP
    return result;
  }

  if (request.pull) {
    try {
      git.open_or_init(project.root_path);
      git.set_remote("origin", *project.git_remote_url);
      try {
        git.pull_remote_ff_only("origin");
      } catch (const holder::git::NonFastForwardPullError& diverged) {
        result.pull.conflicts_resolved =
            resolve_pull_conflicts(db, fts, project, git, diverged, request.now);
      }
      reconcile_index_after_pull(db, fts, project);
      result.pull.status = PullPhaseStatus::Succeeded;
      sync.record_pull_result(
          project_id,
          pull_phase_status_name(result.pull.status),
          true,
          std::nullopt,
          request.now
      );
    } catch (const std::exception& ex) {
      result.pull.status = PullPhaseStatus::Failed;
      result.pull.error_message = ex.what();
      sync.record_pull_result(
          project_id,
          pull_phase_status_name(result.pull.status),
          false,
          result.pull.error_message,
          request.now
      );
    }
    refresh_activity_best_effort(db, project, request.now);
  }

  if (request.push &&
      (!request.pull || result.pull.status == PullPhaseStatus::Succeeded ||
       request.push_after_failed_pull)) {
    result.push.attempted = true;
    try {
      git.open_or_init(project.root_path);
      git.set_remote("origin", *project.git_remote_url);
      if (project.privacy_mode == "encrypted_git") {
        holder::privacy::assert_encryption_push_safe(project.root_path);
      }
      const auto push = git.push_branch("origin", request.branch, request.set_upstream);
      result.push.status = push.status;
      result.push.ahead_count = push.ahead_count;
      result.push.behind_count = push.behind_count;
      result.push.local_head_commit = nonempty(push.local_head_commit);
      result.push.error_message = nonempty(push.error_message);
      sync.record_push_result(
          project_id,
          holder::git::push_status_name(push.status),
          push_succeeded(push.status),
          result.push.error_message,
          request.now
      );
    } catch (const std::exception& ex) {
      result.push.status = holder::git::PushStatus::UnknownError;
      result.push.error_message = ex.what();
      sync.record_push_result(
          project_id,
          holder::git::push_status_name(result.push.status),
          false,
          result.push.error_message,
          request.now
      );
    }
    refresh_activity_best_effort(db, project, request.now);
  }

  return result;
}

} // namespace holder::sync
