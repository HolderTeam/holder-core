#include "card/TagExtractor.h"

#include <md4c.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <regex>
#include <set>

namespace holder::core {
namespace {

const std::regex& tag_pattern() {
  static const std::regex pattern(R"((^|[\s([{>,.;:!?])#([A-Za-z][A-Za-z0-9_/\-]*))");
  return pattern;
}

bool is_hex_digit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Matches CSS shorthand/RGB/RGBA/RRGGBB/RRGGBBAA hex color lengths, so "colour #ff8800" doesn't
// become tag "ff8800". The accepted cost: a deliberately-chosen tag that happens to look like a
// hex string (e.g. "#deadbeef") also gets excluded.
bool looks_like_hex_color(const std::string& candidate) {
  const auto len = candidate.size();
  if (len != 3 && len != 4 && len != 6 && len != 8) {
    return false;
  }
  return std::all_of(candidate.begin(), candidate.end(), is_hex_digit);
}

std::string to_lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

struct ExtractorState {
  bool in_code = false;
  std::vector<std::string> tags;
  std::set<std::string> seen;
  std::vector<TagOccurrence> occurrences;
  std::uintptr_t source_begin = 0;
  std::uintptr_t source_end = 0;
};

void collect_tags(
    const std::string& text,
    ExtractorState& state,
    std::optional<std::size_t> source_offset
) {
  for (auto it = std::sregex_iterator(text.begin(), text.end(), tag_pattern());
       it != std::sregex_iterator();
       ++it) {
    std::string candidate = (*it)[2].str();
    if (looks_like_hex_color(candidate)) {
      continue;
    }
    std::string tag = to_lower(std::move(candidate));
    if (source_offset.has_value()) {
      const auto candidate_pos = static_cast<std::size_t>((*it).position(2));
      state.occurrences.push_back({
          tag,
          source_offset.value() + candidate_pos - 1,
          source_offset.value() + candidate_pos + static_cast<std::size_t>((*it).length(2)),
      });
    }
    if (state.seen.insert(tag).second) {
      state.tags.push_back(std::move(tag));
    }
  }
}

// MD_BLOCK_CODE covers both fenced and indented code blocks -- md4c doesn't distinguish them at
// the block-type level, only in the (unused here) detail struct.
int on_enter_block(MD_BLOCKTYPE type, void*, void* userdata) {
  if (type == MD_BLOCK_CODE) {
    static_cast<ExtractorState*>(userdata)->in_code = true;
  }
  return 0;
}

int on_leave_block(MD_BLOCKTYPE type, void*, void* userdata) {
  if (type == MD_BLOCK_CODE) {
    static_cast<ExtractorState*>(userdata)->in_code = false;
  }
  return 0;
}

int on_enter_span(MD_SPANTYPE type, void*, void* userdata) {
  if (type == MD_SPAN_CODE) {
    static_cast<ExtractorState*>(userdata)->in_code = true;
  }
  return 0;
}

int on_leave_span(MD_SPANTYPE type, void*, void* userdata) {
  if (type == MD_SPAN_CODE) {
    static_cast<ExtractorState*>(userdata)->in_code = false;
  }
  return 0;
}

int on_text(MD_TEXTTYPE, const MD_CHAR* text, MD_SIZE size, void* userdata) {
  auto* state = static_cast<ExtractorState*>(userdata);
  if (!state->in_code) {
    const auto address = reinterpret_cast<std::uintptr_t>(text);
    std::optional<std::size_t> source_offset;
    if (address >= state->source_begin && address + size <= state->source_end) {
      source_offset = static_cast<std::size_t>(address - state->source_begin);
    }
    collect_tags(std::string(text, size), *state, source_offset);
  }
  return 0;
}

} // namespace

static ExtractorState parse_tags(const std::string& markdown_body) {
  ExtractorState state;
  state.source_begin = reinterpret_cast<std::uintptr_t>(markdown_body.data());
  state.source_end = state.source_begin + markdown_body.size();

  MD_PARSER parser = {};
  parser.abi_version = 0;
  parser.flags = 0;
  parser.enter_block = on_enter_block;
  parser.leave_block = on_leave_block;
  parser.enter_span = on_enter_span;
  parser.leave_span = on_leave_span;
  parser.text = on_text;

  md_parse(markdown_body.c_str(), static_cast<MD_SIZE>(markdown_body.size()), &parser, &state);

  return state;
}

std::vector<std::string> extract_tags(const std::string& markdown_body) {
  return parse_tags(markdown_body).tags;
}

std::vector<TagOccurrence> extract_tag_occurrences(const std::string& markdown_body) {
  return parse_tags(markdown_body).occurrences;
}

} // namespace holder::core
