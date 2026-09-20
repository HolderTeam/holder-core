#include <holder/holder.h>

int main() {
  char* output = nullptr;
  holder_error* error = nullptr;
  const int result = holder_project_list(nullptr, &output, &error);
  const bool valid = result == HOLDER_ERROR_INVALID_ARGUMENT && output == nullptr &&
                     error != nullptr;
  holder_string_free(output);
  holder_error_destroy(error);
  return valid ? 0 : 1;
}
