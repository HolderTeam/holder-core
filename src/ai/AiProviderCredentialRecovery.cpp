#include "ai/AiProviderCredentialRecovery.h"

#include "ai/AiProviderCredentialRepo.h"

#include <spdlog/spdlog.h>

namespace holder::ai {
namespace {

constexpr const char* kAiProviderCredentialService = "holder.ai_provider_credentials";

}

void recover_ai_provider_credentials_from_secret_store(
    holder::platform::Db& db,
  holder::privacy::SecretStore& secret_store
) {
  AiProviderCredentialRepo repo(db);
  const auto entries = secret_store.list(kAiProviderCredentialService);
  for (const auto& entry : entries) {
    const auto existing = repo.get(entry.account);
    if (existing.has_value() && existing->api_key_preview == entry.preview &&
        existing->created_at == entry.created_at && existing->updated_at == entry.updated_at) {
      continue;
    }
    repo.upsert(entry.account, entry.preview, entry.created_at, entry.updated_at);
    spdlog::info("Recovered AI provider credential metadata from secret store: {}", entry.account);
  }
}

} // namespace holder::ai
