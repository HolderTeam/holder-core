#include "project/DefaultProject.h"

#include "card/CardStore.h"
#include "model/Card.h"
#include "project/ProjectRepo.h"
#include "project/ProjectStore.h"

#include <chrono>

namespace holder::project {
namespace {

long long now_epoch_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch()
  )
      .count();
}

} // namespace

std::optional<holder::model::Project> ensure_default_project(
    holder::platform::Db& db,
    const std::string& name,
    const std::string& privacy_mode,
    const std::string& welcome_title,
    const std::string& welcome_content,
    const std::function<std::string()>& uuid_v4,
    const std::filesystem::path& projects_root,
    holder::index::FtsIndexer* fts,
    holder::git::GitOps* git
) {
  ProjectRepo repo(db);
  if (!repo.list().empty()) {
    return std::nullopt;
  }

  holder::model::Project project;
  project.name = name;
  project.privacy_mode = privacy_mode;

  ProjectStore store(db, git);
  auto created = store.create(std::move(project), uuid_v4, projects_root);

  holder::card::CardStore card_store(db, fts, /*fs=*/nullptr, git);
  holder::model::Card welcome;
  welcome.card_id = uuid_v4();
  welcome.project_id = created.project_id;
  welcome.title = welcome_title;
  welcome.created_at = now_epoch_seconds();
  welcome.updated_at = welcome.created_at;
  card_store.create(welcome, welcome_content);

  return created;
}

} // namespace holder::project
