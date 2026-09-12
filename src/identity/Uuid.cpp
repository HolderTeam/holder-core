#include "identity/Uuid.h"

#include <sodium.h>

#include <stdexcept>

namespace holder::identity {

std::string uuid_v4() {
  if (sodium_init() < 0) {
    throw std::runtime_error("failed to initialize libsodium"); // LCOV_EXCL_LINE
  }

  unsigned char bytes[16];
  randombytes_buf(bytes, sizeof(bytes));

  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40); // version 4
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80); // variant 10xx

  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(36);

  for (int i = 0; i < 16; ++i) {
    out.push_back(kHex[bytes[i] >> 4]);
    out.push_back(kHex[bytes[i] & 0x0F]);
    if (i == 3 || i == 5 || i == 7 || i == 9) {
      out.push_back('-');
    }
  }

  return out;
}  // LCOV_EXCL_LINE

}  // namespace holder::identity
