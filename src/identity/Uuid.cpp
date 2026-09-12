#include "identity/Uuid.h"

#include <sodium.h>

#include <chrono>
#include <cstdint>
#include <stdexcept>

namespace holder::identity {
namespace {

void ensure_sodium() {
  if (sodium_init() < 0) {
    throw std::runtime_error("failed to initialize libsodium"); // LCOV_EXCL_LINE
  }
}

std::string format_uuid(const unsigned char bytes[16]) {
  static constexpr char kHex[] = "0123456789abcdef";

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
}

}  // namespace

std::string uuid_v4() {
  ensure_sodium();

  unsigned char bytes[16];
  randombytes_buf(bytes, sizeof(bytes));

  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40); // version 4
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80); // variant 10xx

  return format_uuid(bytes);
}

std::string uuid_v7() {
  ensure_sodium();

  unsigned char bytes[16];
  randombytes_buf(bytes, sizeof(bytes));

  const auto now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();

  if (now_ms < 0 ||
      static_cast<std::uint64_t>(now_ms) > 0xFFFFFFFFFFFFULL) {
    throw std::runtime_error("system clock outside UUIDv7 range"); // LCOV_EXCL_LINE
  }

  const auto timestamp = static_cast<std::uint64_t>(now_ms);

  // UUIDv7 stores the Unix timestamp in milliseconds in the first 48 bits,
  // most significant byte first.
  bytes[0] = static_cast<unsigned char>((timestamp >> 40) & 0xFF);
  bytes[1] = static_cast<unsigned char>((timestamp >> 32) & 0xFF);
  bytes[2] = static_cast<unsigned char>((timestamp >> 24) & 0xFF);
  bytes[3] = static_cast<unsigned char>((timestamp >> 16) & 0xFF);
  bytes[4] = static_cast<unsigned char>((timestamp >> 8) & 0xFF);
  bytes[5] = static_cast<unsigned char>(timestamp & 0xFF);

  // The remaining random bytes already provide rand_a and rand_b.
  // Preserve those random bits while setting the RFC version and variant.
  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x70); // version 7
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80); // variant 10xx

  return format_uuid(bytes);
}

}  // namespace holder::identity
