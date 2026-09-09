#pragma once

#include <string>

namespace holder::core {

// Manages a card's *trailing tag line* -- the last non-blank line of the body, when (and only
// when) that line consists solely of whitespace-separated #tag tokens. This is the one place
// in a card's Markdown that Holder itself is willing to add or remove text on behalf of the
// user (e.g. from a "Tags" UI), because editing it can never corrupt prose: a tag written
// anywhere else in the body is left alone.
//
// Both functions preserve the body's existing trailing whitespace (blank lines, or the lack of
// a final newline) byte-for-byte -- the tag line is inserted or updated immediately before that
// trailing run, never after it, and the run itself is never added to, removed from, or
// reordered. See CardStore::add_tag/remove_tag for the full semantic operation these compose
// into (including checking whether a tag already exists anywhere in the document, not just on
// this line).

// Ensures `tag` (already normalize_tag'd) appears on body's trailing tag line, creating that
// line -- separated from the preceding content by exactly one blank line -- if none exists yet.
// A tag already present on the trailing line is not duplicated. New tags are appended after
// whatever's already on the line, left to right; existing tags are never reordered. Assumes the
// caller has already confirmed `tag` doesn't occur anywhere else in the document (see
// extract_tags) -- this function only manages the trailing line itself.
std::string upsert_trailing_tag_line(const std::string& body, const std::string& tag);

enum class RemoveTagLineOutcome {
  Removed,          // tag was on the trailing tag line and has been removed from it.
  NotOnTrailingLine, // tag wasn't found there (it may or may not exist elsewhere in the body).
};

struct RemoveTagLineResult {
  std::string new_body;
  RemoveTagLineOutcome outcome = RemoveTagLineOutcome::NotOnTrailingLine;
};

// Removes `tag` (already normalize_tag'd) from body's trailing tag line, if it's there. If
// removing it empties the line, the line and the blank-line separator before it are removed
// too, rather than leaving a stray empty line. Never touches anything outside the trailing tag
// line -- prose containing the same tag elsewhere in the body is left untouched either way.
RemoveTagLineResult remove_from_trailing_tag_line(const std::string& body, const std::string& tag);

} // namespace holder::core
