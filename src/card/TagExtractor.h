#pragma once

#include <string>
#include <vector>

namespace holder::core {

// Extracts #tag occurrences from a card's Markdown body, lowercased and de-duplicated (first
// occurrence order). Uses md4c to stay code-block/inline-code aware -- a tag inside a fenced
// code block, indented code block, or inline `code span` is never extracted, matching how a
// real Markdown viewer would never render "#foo" inside code as anything but literal text.
//
// A run of text starting at a boundary (string start, whitespace, or one of ([{>,.;:!?) is a
// tag when it matches #[A-Za-z][A-Za-z0-9_/-]*. Requiring a letter first already rules out
// "issue #123". A candidate that's exactly 3, 4, 6, or 8 hex digits (matching CSS shorthand/
// RGB/RGBA/RRGGBB/RRGGBBAA color lengths, case-insensitive) is rejected too, so "colour #ff8800"
// doesn't become tag "ff8800" -- the one accepted cost is a deliberately-chosen tag that happens
// to look like a hex string (e.g. "#deadbeef") also getting excluded.
std::vector<std::string> extract_tags(const std::string& markdown_body);

} // namespace holder::core
