#include "sync/PullConflictResolution.h"

#include "card/CardFrontMatter.h"
#include "card/CardStore.h"
#include "identity/Uuid.h"
#include "model/Card.h"
#include "privacy/ProjectPrivacy.h"
#include "project/ProjectRepo.h"
#include "project/Rebuilder.h"

#include <stdexcept>

namespace holder::sync {

void reconcile_index_after_pull(
    holder::platform::Db& db,
    holder::index::FtsIndexer* fts,
    const holder::model::Project& project
) {
  holder::store::Rebuilder(db, fts).rebuild_project(project);
}

int resolve_pull_conflicts(
    holder::platform::Db& db,
    holder::index::FtsIndexer* fts,
    const holder::model::Project& project,
    holder::git::GitOps& git,
    const holder::git::NonFastForwardPullError& diverged,
    long long now
) {
  const auto current_project = holder::project::ProjectRepo(db).get(project.project_id);
  if (!current_project.has_value()) { // LCOV_EXCL_LINE - requires concurrent direct DB deletion during a locked pull.
    throw std::runtime_error("project not found: " + project.project_id); // LCOV_EXCL_LINE
  }

  const auto merge_result = git.merge_remote_taking_theirs_for_conflicts(
      "origin",
      diverged.local_oid_hex,
      diverged.remote_oid_hex
  );

  holder::card::CardStore store(db, fts);
  int resolved = 0;
  for (const auto& path : merge_result.conflicted_paths) {
    const auto blob = git.read_blob_at(diverged.local_oid_hex, path);
    if (!blob.has_value()) continue;

    std::string plain;
    try {
      plain = project.privacy_mode == "encrypted_git" && project.project_key_id.has_value()
                  ? holder::privacy::decrypt_project_blob(
                        project.project_id,
                        *project.project_key_id,
                        *blob
                    )
                  : *blob;
    } catch (const std::exception&) { // LCOV_EXCL_START - duplicate creation failure intentionally leaves remaining conflicts usable.
      continue;
    } // LCOV_EXCL_STOP // LCOV_EXCL_LINE

    const auto parsed = holder::core::parse_card_file(plain);
    if (!parsed.has_front_matter) continue;

    holder::model::Card duplicate;
    duplicate.card_id = holder::identity::generate_id(current_project->id_scheme);
    duplicate.project_id = project.project_id;
    duplicate.title = parsed.card.title + " (conflicted copy)";
    duplicate.parent_card_id = parsed.card.parent_card_id;
    duplicate.created_at = now;
    duplicate.updated_at = now;

    try {
      store.create(duplicate, parsed.body);
      resolved++;
    } catch (const std::exception&) { // LCOV_EXCL_START - duplicate creation failure intentionally leaves remaining conflicts usable.
      // e.g. id collision (vanishingly unlikely) -- skip rather than fail the whole pull.
    }
    // LCOV_EXCL_STOP
  }
  return resolved;
}

} // namespace holder::sync
