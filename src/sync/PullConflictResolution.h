#pragma once

#include "git/GitOps.h"
#include "index/FtsIndexer.h"
#include "model/Project.h"
#include "platform/Db.h"

#include <functional>
#include <string>

namespace holder::sync {

// A pull only fetches and checks out git's working tree; nothing else keeps the SQLite card
// index (what card listing/search actually read) in sync with content that arrived from a
// remote rather than through CardStore's own create/update calls. Reconciling it is exactly
// what Rebuilder already does for StartupRecovery, so every successful pull -- from any caller,
// C ABI or the daemon's own sync worker -- needs to run this too. Throws on failure by design:
// if pull's git state moved but the index didn't, the pull hasn't actually delivered on its
// purpose (new content becoming visible), so it should be reported failed, not silently wrong.
void reconcile_index_after_pull(
    holder::platform::Db& db,
    holder::index::FtsIndexer* fts,
    const holder::model::Project& project
);

// Called after GitRepo::merge_remote_taking_theirs_for_conflicts has already resolved a
// diverged pull at the git level (remote wins for any card touched on both sides since the
// merge-base). This preserves the pre-merge LOCAL version of each such card by re-creating it
// as a brand new card titled "<original title> (conflicted copy)" -- the Dropbox-style
// resolution: never a line-level merge, always both full versions kept, nothing silently lost.
// A card whose pre-merge content can't be read/decrypted/parsed is skipped rather than failing
// the whole pull -- a partial resolution beats losing an otherwise-successful pull over one
// unreadable conflict. Returns how many conflicts were resolved this way.
int resolve_pull_conflicts(
    holder::platform::Db& db,
    holder::index::FtsIndexer* fts,
    const holder::model::Project& project,
    holder::git::RealGitOps& git,
    const holder::git::NonFastForwardPullError& diverged,
    long long now,
    const std::function<std::string()>& uuid_v4
);

} // namespace holder::sync
