#include "core_test_helpers.h"
#include "resource/LocalDirectoryProvider.h"

#include <holder/holder.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

struct ProviderProbe {
  explicit ProviderProbe(const std::filesystem::path& root)
      : files(root) {}

  holder::resource::LocalDirectoryProvider files;
  std::function<void()> on_put;
  std::function<void()> on_get;
  std::function<void()> on_destroy;
  std::atomic<int> destroyed{0};
  int replacement_result = -1;
  bool destroyed_in_callback = false;
};

using ProbeOwner = std::shared_ptr<ProviderProbe>;

int register_probe(const char* name, const ProbeOwner& probe) {
  holder_error* error = nullptr;
  const int result = holder_storage_provider_register(
      name,
      [](void* data,
         const char* key,
         const char* file,
         long long size,
         const char* hash,
         int*,
         char**) {
        const auto state = *static_cast<ProbeOwner*>(data);
        state->files.put(key, file, size, hash);
        if (state->on_put) state->on_put();
        state->destroyed_in_callback = state->destroyed != 0;
        return 0;
      },
      [](void* data, const char* key, const char* file, int*, char**) {
        const auto state = *static_cast<ProbeOwner*>(data);
        state->files.get(key, file);
        if (state->on_get) state->on_get();
        state->destroyed_in_callback = state->destroyed != 0;
        return 0;
      },
      [](void* data, const char* key, int* exists, int*, char**) {
        const auto state = *static_cast<ProbeOwner*>(data);
        *exists = state->files.exists(key);
        return 0;
      },
      [](void* data, const char* key, int*, char**) {
        const auto state = *static_cast<ProbeOwner*>(data);
        state->files.remove(key);
        return 0;
      },
      new ProbeOwner(probe),
      [](void* data) {
        const std::unique_ptr<ProbeOwner> owner(static_cast<ProbeOwner*>(data));
        ++(*owner)->destroyed;
        if ((*owner)->on_destroy) (*owner)->on_destroy();
      },
      &error
  );
  holder_error_destroy(error);
  return result;
}

template <typename Fn> nlohmann::json call_json(Fn&& fn) {
  char* raw = nullptr;
  holder_error* error = nullptr;
  const int result = fn(&raw, &error);
  const std::unique_ptr<char, decltype(&holder_string_free)> output(raw, holder_string_free);
  const std::unique_ptr<holder_error, decltype(&holder_error_destroy)> failure(
      error,
      holder_error_destroy
  );
  INFO((error ? holder_error_message(error) : "no C API error"));
  REQUIRE(result == HOLDER_OK);
  return nlohmann::json::parse(raw);
}

struct AssetFixture {
  std::filesystem::path root = holder::test::make_temp_dir();
  std::unique_ptr<holder_context, decltype(&holder_context_destroy)> context{
      nullptr,
      holder_context_destroy
  };
  std::string project_id;
  std::string card_id;

  explicit AssetFixture(const char* provider) {
    std::ifstream schema_file(SCHEMA_SQL_PATH);
    const std::string schema((std::istreambuf_iterator<char>(schema_file)), {});
    holder_context* raw_context = nullptr;
    holder_error* error = nullptr;
    const int result =
        holder_context_open(root.string().c_str(), schema.c_str(), &raw_context, &error);
    context.reset(raw_context);
    holder_error_destroy(error);
    REQUIRE(result == HOLDER_OK);
    project_id =
        call_json([&](char** out, holder_error** err) {
          return holder_project_create(context.get(), "Lifetime", nullptr, nullptr, out, err);
        })
            .at("project_id")
            .get<std::string>();
    card_id = call_json([&](char** out, holder_error** err) {
                return holder_card_create(
                    context.get(),
                    project_id.c_str(),
                    "Asset",
                    nullptr,
                    nullptr,
                    out,
                    err
                );
              })
                  .at("card_id")
                  .get<std::string>();
    const auto location = nlohmann::json{
        {"location_id", "lifetime-location"},
        {"project_id", project_id},
        {"name", "Lifetime"},
        {"provider", provider},
        {"configuration", nlohmann::json::object()},
        {"created_at", 1},
        {"updated_at", 1}
    }.dump();
    call_json([&](char** out, holder_error** err) {
      return holder_location_put_json(context.get(), location.c_str(), out, err);
    });
  }

  nlohmann::json import() {
    const auto source = (root / "input.txt").string();
    std::ofstream(source) << "provider lifetime bytes";
    return call_json([&](char** out, holder_error** err) {
      return holder_asset_import_file(
          context.get(),
          project_id.c_str(),
          card_id.c_str(),
          "lifetime-location",
          source.c_str(),
          out,
          err
      );
    });
  }
};

} // namespace

TEST_CASE("C API asset operations retain providers replaced inside callbacks", "[capi][resource]") {
  const char* name = "lifetime-operation";
  AssetFixture fixture(name);
  auto original = std::make_shared<ProviderProbe>(fixture.root / "objects");
  auto replacement = std::make_shared<ProviderProbe>(fixture.root / "objects");
  REQUIRE(register_probe(name, original) == HOLDER_OK);
  const auto replace = [weak = std::weak_ptr<ProviderProbe>(original), replacement, name] {
    if (auto state = weak.lock()) state->replacement_result = register_probe(name, replacement);
  };

  SECTION("import retains the original through put and existence verification") {
    original->on_put = replace;
    fixture.import();
  }
  SECTION("retrieve retains the original until the get callback returns") {
    const auto imported = fixture.import();
    const auto resource_id = imported.at("resource_id").get<std::string>();
    const auto asset_id = imported.at("asset_id").get<std::string>();
    const auto resource = call_json([&](char** out, holder_error** err) {
      return holder_resource_get(fixture.context.get(), resource_id.c_str(), out, err);
    });
    const auto placement_id =
        resource.at("assets").at(0).at("placements").at(0).at("placement_id").get<std::string>();
    original->on_get = replace;
    holder_error* error = nullptr;
    const auto destination = (fixture.root / "download.txt").string();
    const int result = holder_asset_retrieve(
        fixture.context.get(),
        resource_id.c_str(),
        asset_id.c_str(),
        placement_id.c_str(),
        destination.c_str(),
        &error
    );
    holder_error_destroy(error);
    REQUIRE(result == HOLDER_OK);
    std::ifstream downloaded(destination);
    REQUIRE(
        std::string((std::istreambuf_iterator<char>(downloaded)), {}) == "provider lifetime bytes"
    );
  }
  REQUIRE(original->replacement_result == HOLDER_OK);
  REQUIRE_FALSE(original->destroyed_in_callback);
  REQUIRE(original->destroyed.load() == 1);
  REQUIRE(replacement->destroyed.load() == 0);
}

TEST_CASE("C API storage provider cleanup can register another provider", "[capi][resource]") {
  const auto root = holder::test::make_temp_dir();
  auto original = std::make_shared<ProviderProbe>(root);
  auto replacement = std::make_shared<ProviderProbe>(root);
  auto nested = std::make_shared<ProviderProbe>(root);
  original->on_destroy = [weak = std::weak_ptr<ProviderProbe>(original), nested] {
    if (auto state = weak.lock())
      state->replacement_result = register_probe("lifetime-nested", nested);
  };
  REQUIRE(register_probe("lifetime-cleanup", original) == HOLDER_OK);
  REQUIRE(register_probe("lifetime-cleanup", replacement) == HOLDER_OK);
  REQUIRE(original->destroyed.load() == 1);
  REQUIRE(original->replacement_result == HOLDER_OK);
}

TEST_CASE(
    "C API retains an active provider during concurrent replacements",
    "[capi][resource][concurrency][stress]"
) {
  using namespace std::chrono_literals;
  const char* name = "lifetime-concurrent";
  AssetFixture fixture(name);
  auto original = std::make_shared<ProviderProbe>(fixture.root / "objects");
  REQUIRE(register_probe(name, original) == HOLDER_OK);
  std::vector<ProbeOwner> replacements;
  for (int i = 0; i < 128; ++i)
    replacements.push_back(std::make_shared<ProviderProbe>(fixture.root / "objects"));

  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> replaced;
  auto replaced_future = replaced.get_future();
  bool callback_synchronized = false;
  bool retained_during_replacement = false;
  const auto during_callback = [&] {
    entered.set_value();
    callback_synchronized = replaced_future.wait_for(10s) == std::future_status::ready;
    retained_during_replacement = original->destroyed.load() == 0;
  };
  std::string resource_id, asset_id, placement_id;
  SECTION("import") { original->on_put = during_callback; }
  SECTION("retrieve") {
    const auto imported = fixture.import();
    resource_id = imported.at("resource_id").get<std::string>();
    asset_id = imported.at("asset_id").get<std::string>();
    const auto resource = call_json([&](char** out, holder_error** err) {
      return holder_resource_get(fixture.context.get(), resource_id.c_str(), out, err);
    });
    placement_id =
        resource.at("assets").at(0).at("placements").at(0).at("placement_id").get<std::string>();
    original->on_get = during_callback;
  }
  bool registrations_ok = true;
  std::jthread worker([&] {
    if (entered_future.wait_for(10s) == std::future_status::ready) {
      for (const auto& replacement : replacements)
        if (register_probe(name, replacement) != HOLDER_OK) registrations_ok = false;
    } else {
      registrations_ok = false;
    }
    replaced.set_value();
  });
  if (resource_id.empty()) {
    fixture.import();
  } else {
    holder_error* error = nullptr;
    const auto destination = (fixture.root / "concurrent-download.txt").string();
    const auto result = holder_asset_retrieve(
        fixture.context.get(),
        resource_id.c_str(),
        asset_id.c_str(),
        placement_id.c_str(),
        destination.c_str(),
        &error
    );
    holder_error_destroy(error);
    REQUIRE(result == HOLDER_OK);
    std::ifstream downloaded(destination);
    REQUIRE(
        std::string((std::istreambuf_iterator<char>(downloaded)), {}) == "provider lifetime bytes"
    );
  }
  worker.join();
  REQUIRE(registrations_ok);
  REQUIRE(callback_synchronized);
  REQUIRE(retained_during_replacement);
  REQUIRE_FALSE(original->destroyed_in_callback);
  REQUIRE(original->destroyed.load() == 1);
  for (std::size_t i = 0; i + 1 < replacements.size(); ++i)
    REQUIRE(replacements[i]->destroyed.load() == 1);
  REQUIRE(replacements.back()->destroyed.load() == 0);
}
