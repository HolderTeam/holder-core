#pragma once

#include "platform/Db.h"
#include "privacy/SecretStore.h"

namespace holder::ai {

void recover_ai_provider_credentials_from_secret_store(
    holder::platform::Db& db,
    holder::privacy::SecretStore& secret_store
);

} // namespace holder::ai
