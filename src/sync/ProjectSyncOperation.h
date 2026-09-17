#pragma once

#include "git/GitOps.h"
#include "git/PushResult.h"
#include "index/FtsIndexer.h"
#include "platform/Db.h"

#include <optional>
#include <string>

namespace holder::sync {

enum class PullPhaseStatus {
  NotAttempted,
  Succeeded,
  RemoteUnset,
  Failed,
};

const char* pull_phase_status_name(PullPhaseStatus status);

struct PullPhaseResult {
  bool attempted = false;
  PullPhaseStatus status = PullPhaseStatus::NotAttempted;
  int conflicts_resolved = 0;
  std::optional<std::string> error_message;
};

struct PushPhaseResult {
  bool attempted = false;
  holder::git::PushStatus status = holder::git::PushStatus::UnknownError;
  int ahead_count = 0;
  int behind_count = 0;
  std::optional<std::string> local_head_commit;
  std::optional<std::string> error_message;
};

struct ProjectSyncRequest {
  bool pull = true;
  bool push = false;
  // Keeps legacy scheduled-sync behavior available to existing callers. Forced sync callers
  // should leave this false so a failed pull cannot be followed by a push.
  bool push_after_failed_pull = false;
  std::string branch;
  bool set_upstream = true;
  long long now = 0;
};

struct ProjectSyncResult {
  std::string project_id;
  std::optional<std::string> remote_url;
  PullPhaseResult pull;
  PushPhaseResult push;

  bool succeeded() const;
};

// Runs one serialized project operation. A pull always reconciles the SQLite index before it is
// considered successful. When both phases are requested, push is skipped unless pull succeeds.
// Expected Git failures are returned and recorded rather than thrown; invalid projects and
// database failures still throw.
ProjectSyncResult run_project_sync(
    holder::platform::Db& db,
    holder::index::FtsIndexer* fts,
    holder::git::GitOps& git,
    const std::string& project_id,
    const ProjectSyncRequest& request
);

} // namespace holder::sync
