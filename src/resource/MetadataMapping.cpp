#include "resource/MetadataMapping.h"

#include <map>

namespace holder::resource {
namespace {

const std::map<std::string, std::string> mappings = {
    {"title", "http://purl.org/dc/terms/title"},
    {"creator", "http://purl.org/dc/terms/creator"},
    {"subject", "http://purl.org/dc/terms/subject"},
    {"description", "http://purl.org/dc/terms/description"},
    {"publisher", "http://purl.org/dc/terms/publisher"},
    {"contributor", "http://purl.org/dc/terms/contributor"},
    {"date", "http://purl.org/dc/terms/date"},
    {"format", "http://purl.org/dc/terms/format"},
    {"identifier", "http://purl.org/dc/terms/identifier"},
    {"source", "http://purl.org/dc/terms/source"},
    {"language", "http://purl.org/dc/terms/language"},
    {"relation", "http://purl.org/dc/terms/relation"},
    {"coverage", "http://purl.org/dc/terms/coverage"},
    {"rights", "http://purl.org/dc/terms/rights"},
};

} // namespace

std::optional<std::string> dublin_core_term_for(const std::string& holder_property) {
  const auto found = mappings.find(holder_property);
  if (found == mappings.end()) return std::nullopt;
  return found->second;
}

std::string holder_property_for_dublin_core(const std::string& external_term) {
  for (const auto& [friendly, uri] : mappings) {
    if (external_term == uri || external_term == "dcterms:" + friendly ||
        external_term == "dc:" + friendly) {
      return friendly;
    }
  }
  return external_term;
}

} // namespace holder::resource
