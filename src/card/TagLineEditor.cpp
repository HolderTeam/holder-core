#include "card/TagLineEditor.h"

#include "card/TagExtractor.h"

#include <algorithm>
#include <optional>
#include <vector>

namespace holder::core {
namespace {

bool is_line_whitespace(char c) {
  return c == ' ' || c == '\t' || c == '\r';
}

// Splits `body` into (content, trailing_ws) at the last character that isn't a newline or
// horizontal whitespace -- trailing_ws is everything after it, preserved verbatim by both
// upsert_trailing_tag_line and remove_from_trailing_tag_line. A body with no real content at
// all (empty, or only whitespace/blank lines) yields an empty content and the whole body as
// trailing_ws.
struct BodySplit {
  std::string content;
  std::string trailing_ws;
};

BodySplit split_trailing_whitespace(const std::string& body) {
  std::size_t end = body.size();
  while (end > 0 && (body[end - 1] == '\n' || is_line_whitespace(body[end - 1]))) {
    --end;
  }
  return {body.substr(0, end), body.substr(end)};
}

// Start offset (within `content`) of content's last line -- the character right after the
// last '\n', or 0 if content has none.
std::size_t last_line_start(const std::string& content) {
  const auto pos = content.find_last_of('\n');
  return pos == std::string::npos ? 0 : pos + 1;
}

// A pure tag line is one or more whitespace-separated #tag tokens and nothing else. Returns
// the tags in left-to-right order (not deduplicated -- callers dedupe as needed) if `line`
// qualifies, or an empty optional if it doesn't (including an empty line).
std::optional<std::vector<std::string>> parse_pure_tag_line(const std::string& line) {
  std::vector<std::string> tags;
  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && is_line_whitespace(line[i])) {
      ++i;
    }
    if (i >= line.size()) {
      break;
    }
    if (line[i] != '#') {
      return std::nullopt;
    }
    ++i;
    const std::size_t start = i;
    while (i < line.size() && !is_line_whitespace(line[i])) {
      ++i;
    }
    const std::string candidate = line.substr(start, i - start);
    if (!is_valid_tag(candidate)) {
      return std::nullopt;
    }
    tags.push_back(normalize_tag(candidate));
  }
  if (tags.empty()) {
    return std::nullopt;
  }
  return tags;
}

std::string join_tag_line(const std::vector<std::string>& tags) {
  std::string line;
  for (std::size_t i = 0; i < tags.size(); ++i) {
    if (i > 0) {
      line += ' ';
    }
    line += '#';
    line += tags[i];
  }
  return line;
}

} // namespace

std::string upsert_trailing_tag_line(const std::string& body, const std::string& tag) {
  const auto split = split_trailing_whitespace(body);
  const auto& content = split.content;

  if (content.empty()) {
    // No real content at all (blank/empty body) -- becomes just the tag line, discarding any
    // stray whitespace that was there since there's no content for it to trail.
    return "#" + tag;
  }

  const auto line_start = last_line_start(content);
  const std::string last_line = content.substr(line_start);
  auto existing = parse_pure_tag_line(last_line);

  if (existing.has_value()) {
    if (std::find(existing->begin(), existing->end(), tag) != existing->end()) {
      return body; // Already there -- no-op.
    }
    existing->push_back(tag);
    return content.substr(0, line_start) + join_tag_line(*existing) + split.trailing_ws;
  }

  // No tag line yet: create one, separated from existing content by exactly one blank line.
  return content + "\n\n" + "#" + tag + split.trailing_ws;
}

RemoveTagLineResult remove_from_trailing_tag_line(const std::string& body, const std::string& tag) {
  const auto split = split_trailing_whitespace(body);
  const auto& content = split.content;

  if (content.empty()) {
    return {body, RemoveTagLineOutcome::NotOnTrailingLine};
  }

  const auto line_start = last_line_start(content);
  const std::string last_line = content.substr(line_start);
  auto existing = parse_pure_tag_line(last_line);
  if (!existing.has_value()) {
    return {body, RemoveTagLineOutcome::NotOnTrailingLine};
  }

  const auto it = std::find(existing->begin(), existing->end(), tag);
  if (it == existing->end()) {
    return {body, RemoveTagLineOutcome::NotOnTrailingLine};
  }
  existing->erase(it);

  if (existing->empty()) {
    // Removing the line's only tag removes the line -- and the blank-line separator before
    // it, so we're left with exactly what preceded them, not a stray empty line.
    auto before = content.substr(0, line_start);
    while (!before.empty() && before.back() == '\n') {
      before.pop_back();
    }
    return {before + split.trailing_ws, RemoveTagLineOutcome::Removed};
  }

  return {
      content.substr(0, line_start) + join_tag_line(*existing) + split.trailing_ws,
      RemoveTagLineOutcome::Removed,
  };
}

} // namespace holder::core
