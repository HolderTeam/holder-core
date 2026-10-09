#include "project/ProjectImport.h"

#include "card/CardMutation.h"
#include "platform/Fs.h"
#include "platform/Tx.h"
#include "project/ProjectManifest.h"
#include "project/ProjectRepo.h"
#include "project/Rebuilder.h"

#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace holder::project {

holder::model::Project import_project_into_empty_database(
    holder::platform::Db& db,
    holder::index::FtsIndexer& fts,
    const std::filesystem::path& data_dir,
    const std::filesystem::path& project_root
) {
  if (std::filesystem::is_symlink(project_root) ||
      std::filesystem::is_symlink(data_dir / "projects")) {
    throw std::invalid_argument("project import does not support symlink roots");
  }
  const auto managed = std::filesystem::canonical(data_dir / "projects");
  const auto root = std::filesystem::canonical(project_root);
  if (root.parent_path() != managed || !std::filesystem::is_directory(root)) {
    throw std::invalid_argument("project import requires an immediate managed project directory");
  }
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    const auto status = entry.symlink_status();
    if (std::filesystem::is_symlink(status) ||
        (!std::filesystem::is_directory(status) && !std::filesystem::is_regular_file(status))) {
      throw std::invalid_argument("project import requires ordinary files without symlinks");
    }
    if (std::filesystem::is_regular_file(status) &&
        std::filesystem::hard_link_count(entry.path()) != 1) {
      throw std::invalid_argument("project import does not support shared hard-linked files");
    }
  }
  std::ifstream bootstrap_file(root / ".holder/privacy.json");
  const auto bootstrap = nlohmann::json::parse(bootstrap_file);
  if (bootstrap.value("mode", std::string()) != "plain") {
    throw std::invalid_argument("project import currently supports plain projects only");
  }
  auto project = read_project_manifest(root);
  // A fresh import is separate from loss/corruption recovery. Existing recovery
  // readiness and object-count checks must not be relaxed to onboard a new copy.
  holder::core::RealFs fs;
  holder::card::CardMutation::recover_files(fs, root);
  holder::platform::Tx tx(db);
  ProjectRepo projects(db);
  if (!projects.list().empty()) {
    throw std::invalid_argument("project import requires an empty project database");
  }
  projects.create(project);
  holder::store::Rebuilder(db, &fts, nullptr, false, true).rebuild_project_in_transaction(project);
  tx.commit();
  holder::card::CardMutation::finish_recovery(fs, root);
  return project;
}

} // namespace holder::project
