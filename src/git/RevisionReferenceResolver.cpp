#include "git/RevisionReferenceResolver.h"

#include "git/GitRepo.h"

#include <git2.h>

#include <stdexcept>
#include <string>

namespace holder::git {
namespace {

RevisionReferenceResult resolved(const git_oid& oid) {
  RevisionReferenceResult result;
  result.status = RevisionReferenceStatus::Resolved;
  result.oid = std::string(git_oid_tostr_s(&oid));
  return result;
}

std::runtime_error git_lookup_error(int rc) {
  const git_error* error = git_error_last();
  std::string message = "git_object_lookup_prefix failed (rc=" + std::to_string(rc) + ")";
  if (error != nullptr && error->message != nullptr) {
    message += ": ";
    message += error->message;
  }
  return std::runtime_error(message);
}

} // namespace

RevisionReferenceResult RevisionReferenceResolver::resolve(std::string_view reference) const {
  repo_.ensure_open();
  if (reference.size() < kMinimumPrefixLength || reference.size() > GIT_OID_HEXSZ) {
    return {};
  }

  git_oid prefix{};
  if (git_oid_fromstrn(&prefix, reference.data(), reference.size()) != 0) {
    return {};
  }

  auto* repo = reinterpret_cast<git_repository*>(repo_.repo_);
  git_object* object = nullptr;
  const int rc =
      git_object_lookup_prefix(&object, repo, &prefix, reference.size(), GIT_OBJECT_COMMIT);
  if (rc == GIT_ENOTFOUND) return {};
  if (rc == GIT_EAMBIGUOUS) {
    RevisionReferenceResult result;
    result.status = RevisionReferenceStatus::Ambiguous;
    return result;
  }
  if (rc != 0) throw git_lookup_error(rc);

  const git_oid oid = *git_object_id(object);
  git_object_free(object);
  return resolved(oid);
}

} // namespace holder::git
