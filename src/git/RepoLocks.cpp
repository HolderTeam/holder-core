#include "git/RepoLocks.h"

#include <map>
#include <system_error>

namespace holder::git {
namespace {

std::mutex& registry_mutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::filesystem::path, std::weak_ptr<std::recursive_mutex>>& registry() {
  static std::map<std::filesystem::path, std::weak_ptr<std::recursive_mutex>> map;
  return map;
}

} // namespace

std::filesystem::path canonical_repo_key(const std::filesystem::path& repo_root) {
  std::error_code ec;
  auto canonical = std::filesystem::weakly_canonical(repo_root, ec);
  if (ec || canonical.empty()) {
    return repo_root.lexically_normal();
  }
  return canonical;
}

std::shared_ptr<std::recursive_mutex> repo_mutex_for(const std::filesystem::path& repo_root) {
  const auto key = canonical_repo_key(repo_root);

  std::lock_guard<std::mutex> guard(registry_mutex());
  auto& slot = registry()[key];
  if (auto existing = slot.lock()) {
    return existing;
  }
  auto created = std::make_shared<std::recursive_mutex>();
  slot = created;
  return created;
}

} // namespace holder::git
