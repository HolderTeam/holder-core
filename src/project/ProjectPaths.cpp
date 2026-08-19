#include "project/ProjectPaths.h"

#include <cctype>
#include <stdexcept>
#include <unordered_set>

namespace holder::core {

std::string slugify(const std::string& input) {
  std::string out;
  bool prev_dash = false;
  for (const char c : input) {
    const unsigned char ch = static_cast<unsigned char>(c);
    if (ch > 127) {
      continue;
    }
    if (std::isspace(ch)) {
      if (!out.empty() && !prev_dash) {
        out.push_back('-');
        prev_dash = true;
      }
      continue;
    }
    if (std::isalnum(ch) || ch == '_' || ch == '-') {
      char lower = static_cast<char>(std::tolower(ch));
      if (lower == '-') {
        if (!out.empty() && !prev_dash) {
          out.push_back('-');
          prev_dash = true;
        }
      } else {
        out.push_back(lower);
        prev_dash = false;
      }
    }
  }
  while (!out.empty() && out.back() == '-') {
    out.pop_back();
  }
  if (out.empty()) {
    out = "project";
  }
  return out;
} // LCOV_EXCL_LINE

std::string unique_project_root(
    const std::filesystem::path& base_root,
    const std::string& slug,
    const std::vector<holder::model::Project>& existing
) {
  std::unordered_set<std::string> used;
  used.reserve(existing.size());
  for (const auto& project : existing) {
    used.insert(project.root_path);
  }

  for (int attempt = 1; attempt <= 1000; ++attempt) {
    std::string suffix;
    if (attempt > 1) {
      suffix = "-" + std::to_string(attempt);
    }
    const auto candidate = (base_root / (slug + suffix)).string();
    if (used.find(candidate) != used.end()) {
      continue;
    }
    if (std::filesystem::exists(candidate)) {
      continue;
    }
    return candidate;
  }
  throw std::runtime_error("Unable to generate unique project root path.");
}

} // namespace holder::core
