#pragma once

#include "model/IdScheme.h"

#include <string>

namespace holder::identity {

std::string uuid_v4();
std::string uuid_v7();
std::string generate_id(holder::model::IdScheme scheme);

}  // namespace holder::identity
