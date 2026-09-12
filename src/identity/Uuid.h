#pragma once

#include "model/IdScheme.h"

#include <string>
#include <string_view>

namespace holder::identity {

std::string uuid_v4();
std::string uuid_v7();
std::string generate_id(holder::model::IdScheme scheme);
bool is_valid_uuid(std::string_view value);

}  // namespace holder::identity
