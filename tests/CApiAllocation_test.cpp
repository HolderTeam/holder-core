#include <holder/holder.h>

#include <cstddef>
#include <cstdio>
#include <new>

// GNU/LLVM linkers can fail the next allocation in the statically linked core
// without replacing the allocator in shared dependencies or the test framework.
// This executable is deliberately separate from the Catch2 test runner.
namespace {
bool fail_next_allocation = false;
}

extern "C" void* __real__Znwm(std::size_t size);
extern "C" void* __wrap__Znwm(std::size_t size) {
  if (fail_next_allocation) {
    fail_next_allocation = false;
    throw std::bad_alloc();
  }
  return __real__Znwm(size);
}

int main() {
  int failures = 0;
  for (int api = 0; api < 3; ++api) {
    int destroyed = 0;
    auto destroy = [](void* data) {
      ++*static_cast<int*>(data);
    };
    holder_error* error = nullptr;
    fail_next_allocation = true;
    int result = HOLDER_OK;
    switch (api) {
    case 0:
      result = holder_storage_provider_register(
          "allocation-test",
          nullptr,
          nullptr,
          nullptr,
          nullptr,
          &destroyed,
          destroy,
          &error
      );
      break;
    case 1:
      result = holder_keyring_set_provider(nullptr, nullptr, nullptr, &destroyed, destroy, &error);
      break;
    case 2:
      result = holder_git_set_ssh_signer(
          nullptr,
          nullptr,
          nullptr,
          0,
          nullptr,
          &destroyed,
          destroy,
          &error
      );
      break;
    }
    const bool injected = !fail_next_allocation;
    fail_next_allocation = false;
    holder_error_destroy(error);
    if (!injected || result != HOLDER_ERROR_ALLOCATION || destroyed != 1) {
      std::fprintf(
          stderr,
          "API %d: injected=%d, result=%d, cleanup calls=%d\n",
          api,
          injected,
          result,
          destroyed
      );
      ++failures;
    }
  }
  return failures == 0 ? 0 : 1;
}
