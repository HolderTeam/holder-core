#include "model/IdScheme.h"

#include <stdexcept>

namespace holder::model {

std::string_view to_string(IdScheme scheme) {
  switch (scheme) {
  case IdScheme::Uuid4:
    return "uuid4";
  case IdScheme::Uuid7:
    return "uuid7";
  }
  throw std::invalid_argument("unsupported ID scheme"); // LCOV_EXCL_LINE
}

std::optional<IdScheme> id_scheme_from_string(std::string_view value) {
  if (value == "uuid4") return IdScheme::Uuid4;
  if (value == "uuid7") return IdScheme::Uuid7;
  return std::nullopt;
}

} // namespace holder::model
