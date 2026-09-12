#pragma once

#include <optional>
#include <string_view>

namespace holder::model {

enum class IdScheme {
  Uuid4,
  Uuid7,
};

std::string_view to_string(IdScheme scheme);
std::optional<IdScheme> id_scheme_from_string(std::string_view value);

}  // namespace holder::model
