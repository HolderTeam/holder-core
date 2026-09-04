#include "history/CardHistory.h"

#include "card/CardFrontMatter.h"
#include "card/CardPaths.h"
#include "git/GitRepo.h"
#include "privacy/ProjectPrivacy.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace holder::history {
namespace {

constexpr long long kSessionGapSeconds = 10 * 60;
constexpr long long kSessionMaxSeconds = 30 * 60;

struct Snapshot {
  bool exists = false;
  holder::core::ParsedCardFile card;
};

std::string decode(const holder::model::Project& project, const std::string& raw) {
  if (project.privacy_mode != "encrypted_git") return raw;
  if (!project.project_key_id.has_value() || project.project_key_id->empty()) {
    throw std::runtime_error("Encrypted project is missing its project key id");
  }
  return holder::privacy::decrypt_project_blob(project.project_id, *project.project_key_id, raw);
}

Snapshot snapshot_at(
    holder::git::GitRepo& repo,
    const holder::model::Project& project,
    const std::string& card_id,
    const std::optional<std::string>& oid
) {
  if (!oid.has_value() || oid->empty()) return {};
  auto raw = repo.read_blob_at(*oid, holder::core::card_rel_path(card_id));
  if (!raw.has_value()) raw = repo.read_blob_at(*oid, holder::core::card_trash_rel_path(card_id));
  if (!raw.has_value()) return {};
  return {.exists = true, .card = holder::core::parse_card_file(decode(project, *raw))};
}

std::string kind_for(const holder::git::GitHistoryCommit& commit) {
  const auto& message = commit.message;
  if (commit.parent_oids.size() > 1 || message.rfind("Merge ", 0) == 0) return "merged";
  if (message.rfind("Add card ", 0) == 0) return "created";
  if (message.rfind("Move card ", 0) == 0) return "moved";
  if (message.rfind("Update links for ", 0) == 0 || message.rfind("Attach ", 0) == 0) {
    return "links";
  }
  if (message.rfind("Update milestones for ", 0) == 0) return "milestones";
  if (message.rfind("Delete card ", 0) == 0) return "deleted";
  if (message.rfind("Restore card ", 0) == 0) return "restored";
  if (message.rfind("Permanently delete card ", 0) == 0) return "permanently_deleted";
  return "updated";
}

bool may_group(const CardHistoryEntry& newer, const holder::git::GitHistoryCommit& older) {
  if (newer.kind != "updated" || kind_for(older) != "updated") return false;
  if (newer.is_merge || older.parent_oids.size() > 1) return false;
  if (newer.author_name != older.author_name || newer.author_email != older.author_email) return false;
  if (newer.parent_oids.size() != 1 || newer.parent_oids.front() != older.oid) return false;
  const auto gap = newer.started_at - older.committed_at;
  const auto span = newer.ended_at - older.committed_at;
  return gap >= 0 && gap <= kSessionGapSeconds && span <= kSessionMaxSeconds;
}

std::string first_meaningful_line(const std::string& text) {
  std::size_t start = 0;
  while (start <= text.size()) {
    const auto end = text.find('\n', start);
    auto line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    line.erase(line.begin(), std::find_if(line.begin(), line.end(), [](unsigned char ch) {
      return std::isspace(ch) == 0;
    }));
    line.erase(std::find_if(line.rbegin(), line.rend(), [](unsigned char ch) {
      return std::isspace(ch) == 0;
    }).base(), line.end());
    if (!line.empty()) {
      if (line.size() > 72) line = line.substr(0, 69) + "...";
      return line;
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return {};
}

std::string describe_change(const Snapshot& before, const Snapshot& after, const std::string& kind) {
  if (kind == "created" || (!before.exists && after.exists)) return "Card created";
  if (kind == "deleted") return "Moved card to Trash";
  if (kind == "permanently_deleted" || (before.exists && !after.exists)) {
    return "Permanently deleted card";
  }
  if (kind == "restored") return "Restored card from Trash";
  if (kind == "moved") return "Moved card";
  if (kind == "milestones") return "Changed milestones";
  if (kind == "links") return "Changed links or attachments";
  if (kind == "merged") return "Combined changes";
  if (!before.exists || !after.exists) return "Changed card";
  if (before.card.card.title != after.card.card.title) {
    return "Changed title to “" + after.card.card.title + "”";
  }
  if (before.card.links.size() != after.card.links.size()) return "Changed links";
  if (before.card.milestones.size() != after.card.milestones.size()) return "Changed milestones";
  const auto line = first_meaningful_line(after.card.body);
  return line.empty() ? "Edited card" : "Edited “" + line + "”";
}

std::vector<std::string> lines_of(const std::string& text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start < text.size()) {
    const auto end = text.find('\n', start);
    lines.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  if (!text.empty() && text.back() == '\n') lines.emplace_back();
  return lines;
}

std::vector<CardDiffLine> line_diff(const std::string& old_text, const std::string& new_text) {
  const auto old_lines = lines_of(old_text);
  const auto new_lines = lines_of(new_text);
  // Card bodies are normally small. Keep pathological imported documents bounded; the fallback
  // remains truthful, merely less compact.
  if (old_lines.size() * new_lines.size() > 1'000'000U) {
    std::vector<CardDiffLine> out;
    long long old_no = 1;
    for (const auto& line : old_lines) out.push_back({'-', line, old_no++, -1});
    long long new_no = 1;
    for (const auto& line : new_lines) out.push_back({'+', line, -1, new_no++});
    return out;
  }

  std::vector<std::vector<std::size_t>> lcs(
      old_lines.size() + 1, std::vector<std::size_t>(new_lines.size() + 1, 0)
  );
  for (std::size_t i = old_lines.size(); i-- > 0;) {
    for (std::size_t j = new_lines.size(); j-- > 0;) {
      lcs[i][j] = old_lines[i] == new_lines[j]
          ? lcs[i + 1][j + 1] + 1
          : std::max(lcs[i + 1][j], lcs[i][j + 1]);
    }
  }

  std::vector<CardDiffLine> out;
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < old_lines.size() || j < new_lines.size()) {
    if (i < old_lines.size() && j < new_lines.size() && old_lines[i] == new_lines[j]) {
      out.push_back({' ', old_lines[i], static_cast<long long>(i + 1), static_cast<long long>(j + 1)});
      ++i;
      ++j;
    } else if (j < new_lines.size() &&
               (i == old_lines.size() || lcs[i][j + 1] >= lcs[i + 1][j])) {
      out.push_back({'+', new_lines[j], -1, static_cast<long long>(j + 1)});
      ++j;
    } else {
      out.push_back({'-', old_lines[i], static_cast<long long>(i + 1), -1});
      ++i;
    }
  }
  return out;
}

CardVersion version_from(const Snapshot& snapshot, const std::optional<std::string>& oid) {
  CardVersion result;
  result.exists = snapshot.exists;
  result.oid = oid.value_or("");
  if (snapshot.exists) {
    result.title = snapshot.card.card.title;
    result.body = snapshot.card.body;
  }
  return result;
}

} // namespace

CardHistoryPage CardHistoryService::list(
    const holder::model::Project& project,
    const std::string& card_id,
    std::size_t limit,
    const std::optional<std::string>& cursor
) const {
  if (card_id.size() < 4) throw std::invalid_argument("card_id is invalid");
  if (limit == 0 || limit > 200) throw std::invalid_argument("history limit must be between 1 and 200");

  holder::git::GitRepo repo;
  repo.open_or_init(project.root_path);
  CardHistoryPage page;
  page.head_oid = repo.head_oid();
  bool has_more = false;
  const auto commits = repo.history_for_paths(
      {holder::core::card_rel_path(card_id), holder::core::card_trash_rel_path(card_id)},
      limit,
      cursor,
      has_more
  );

  for (const auto& commit : commits) {
    if (!page.entries.empty() && may_group(page.entries.back(), commit)) {
      auto& entry = page.entries.back();
      entry.first_oid = commit.oid;
      entry.started_at = commit.committed_at;
      entry.parent_oids = commit.parent_oids;
      ++entry.commit_count;
      continue;
    }
    CardHistoryEntry entry;
    entry.first_oid = commit.oid;
    entry.last_oid = commit.oid;
    entry.parent_oids = commit.parent_oids;
    entry.author_name = commit.author_name.empty() ? "Holder" : commit.author_name;
    entry.author_email = commit.author_email;
    entry.started_at = commit.committed_at;
    entry.ended_at = commit.committed_at;
    entry.kind = kind_for(commit);
    entry.commit_count = 1;
    entry.is_merge = commit.parent_oids.size() > 1;
    page.entries.push_back(std::move(entry));
  }

  for (auto& entry : page.entries) {
    const auto before_oid = entry.parent_oids.empty()
        ? std::optional<std::string>{}
        : std::optional<std::string>{entry.parent_oids.front()};
    entry.summary = describe_change(
        snapshot_at(repo, project, card_id, before_oid),
        snapshot_at(repo, project, card_id, entry.last_oid),
        entry.kind
    );
  }
  if (has_more && !commits.empty()) page.next_cursor = commits.back().oid;
  return page;
}

CardHistoryComparison CardHistoryService::compare(
    const holder::model::Project& project,
    const std::string& card_id,
    const std::optional<std::string>& from_oid,
    const std::optional<std::string>& to_oid
) const {
  if (card_id.size() < 4) throw std::invalid_argument("card_id is invalid");
  holder::git::GitRepo repo;
  repo.open_or_init(project.root_path);
  const auto resolved_to = to_oid.has_value() ? to_oid : repo.head_oid();
  const auto before = snapshot_at(repo, project, card_id, from_oid);
  const auto after = snapshot_at(repo, project, card_id, resolved_to);

  CardHistoryComparison result;
  result.from = version_from(before, from_oid);
  result.to = version_from(after, resolved_to);
  result.summary = describe_change(before, after, "updated");
  result.lines = line_diff(result.from.body, result.to.body);
  return result;
}

} // namespace holder::history
