#include "resource/AssetEnvelope.h"

#include "privacy/ProjectPrivacy.h"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <sodium.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace holder::resource {
namespace {

constexpr std::string_view kMagic = "HolderAsset1\n";
constexpr std::size_t kChunkSize = 64 * 1024;

class Sha256 {
 public:
  Sha256() : context_(EVP_MD_CTX_new(), EVP_MD_CTX_free) {
    if (!context_ || EVP_DigestInit_ex(context_.get(), EVP_sha256(), nullptr) != 1) {
      throw std::runtime_error("failed to initialise SHA-256");
    }
  }

  void update(const void* data, std::size_t size) {
    if (size > 0 && EVP_DigestUpdate(context_.get(), data, size) != 1) {
      throw std::runtime_error("failed to update SHA-256");
    }
  }

  std::string finish() {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(context_.get(), digest.data(), &size) != 1) {
      throw std::runtime_error("failed to finish SHA-256");
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (unsigned int index = 0; index < size; ++index) out << std::setw(2) << +digest[index];
    return out.str();
  }

 private:
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context_;
};

void ensure_sodium() {
  static const int initialized = sodium_init();
  if (initialized < 0) throw std::runtime_error("libsodium initialization failed");
}

void make_private(const std::filesystem::path& path) {
#ifndef _WIN32
  if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
    throw std::runtime_error("failed to set private asset file permissions");
  }
#else
  (void)path;
#endif
}

void write_bytes(std::ofstream& out, Sha256& hash, long long& size, const void* data, std::size_t n) {
  out.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
  if (!out) throw std::runtime_error("failed to write staged asset");
  hash.update(data, n);
  size += static_cast<long long>(n);
}

void write_u32(std::ofstream& out, Sha256& hash, long long& size, std::uint32_t value) {
  const std::array<unsigned char, 4> bytes = {
      static_cast<unsigned char>((value >> 24) & 0xff),
      static_cast<unsigned char>((value >> 16) & 0xff),
      static_cast<unsigned char>((value >> 8) & 0xff),
      static_cast<unsigned char>(value & 0xff),
  };
  write_bytes(out, hash, size, bytes.data(), bytes.size());
}

std::uint32_t read_u32(std::ifstream& in, Sha256* stored_hash = nullptr) {
  std::array<unsigned char, 4> bytes{};
  in.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
  if (in.gcount() != static_cast<std::streamsize>(bytes.size())) {
    throw std::runtime_error("truncated HolderAsset1 length");
  }
  if (stored_hash) stored_hash->update(bytes.data(), bytes.size());
  return (static_cast<std::uint32_t>(bytes[0]) << 24) |
         (static_cast<std::uint32_t>(bytes[1]) << 16) |
         (static_cast<std::uint32_t>(bytes[2]) << 8) | static_cast<std::uint32_t>(bytes[3]);
}

std::string require_key_id(const holder::model::Project& project) {
  if (!project.project_key_id.has_value() || project.project_key_id->empty()) {
    throw std::runtime_error("encrypted project missing project_key_id");
  }
  return *project.project_key_id;
}

nlohmann::json authenticated_header(
    const holder::model::Project& project,
    const std::string& resource_id,
    const std::string& asset_id
) {
  return {
      {"asset_id", asset_id},
      {"key_id", require_key_id(project)},
      {"project_id", project.project_id},
      {"resource_id", resource_id},
      {"version", 1},
  };
}

void verify_digest(const FileDigest& actual, const FileDigest& expected, const char* label) {
  if (actual.byte_size != expected.byte_size || actual.sha256 != expected.sha256) {
    throw std::runtime_error(std::string(label) + " integrity check failed");
  }
}

} // namespace

FileDigest digest_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("failed to open asset file: " + path.string());
  Sha256 hash;
  long long size = 0;
  std::array<char, kChunkSize> buffer{};
  while (input) {
    input.read(buffer.data(), buffer.size());
    const auto count = input.gcount();
    if (count > 0) {
      hash.update(buffer.data(), static_cast<std::size_t>(count));
      size += count;
    }
  }
  if (!input.eof()) throw std::runtime_error("failed while reading asset file");
  return {size, hash.finish()};
}

StagedAsset stage_asset_file(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const holder::model::Project& project,
    const std::string& resource_id,
    const std::string& asset_id
) {
  std::ifstream input(source, std::ios::binary);
  if (!input) throw std::runtime_error("failed to open asset source: " + source.string());
  std::filesystem::create_directories(destination.parent_path());
  std::ofstream output(destination, std::ios::binary | std::ios::trunc);
  if (!output) throw std::runtime_error("failed to open asset staging file: " + destination.string());
  make_private(destination);

  Sha256 plain_hash;
  Sha256 stored_hash;
  long long plain_size = 0;
  long long stored_size = 0;

  if (project.privacy_mode != "encrypted_git") {
    std::array<char, kChunkSize> buffer{};
    while (input) {
      input.read(buffer.data(), buffer.size());
      const auto count = input.gcount();
      if (count > 0) {
        plain_hash.update(buffer.data(), static_cast<std::size_t>(count));
        write_bytes(output, stored_hash, stored_size, buffer.data(), static_cast<std::size_t>(count));
        plain_size += count;
      }
    }
    if (!input.eof()) throw std::runtime_error("failed while reading asset source");
    output.close();
    return {"plain", {plain_size, plain_hash.finish()}, {stored_size, stored_hash.finish()}};
  }

  ensure_sodium();
  const auto header_json = authenticated_header(project, resource_id, asset_id).dump();
  const auto key = holder::privacy::load_project_key_bytes(project.project_id, require_key_id(project));
  crypto_secretstream_xchacha20poly1305_state state{};
  std::array<unsigned char, crypto_secretstream_xchacha20poly1305_HEADERBYTES> stream_header{};
  if (crypto_secretstream_xchacha20poly1305_init_push(&state, stream_header.data(), key.data()) != 0) {
    throw std::runtime_error("failed to initialise HolderAsset1 encryption");
  }

  write_bytes(output, stored_hash, stored_size, kMagic.data(), kMagic.size());
  write_u32(output, stored_hash, stored_size, static_cast<std::uint32_t>(header_json.size()));
  write_bytes(output, stored_hash, stored_size, header_json.data(), header_json.size());
  write_bytes(output, stored_hash, stored_size, stream_header.data(), stream_header.size());

  std::vector<unsigned char> current(kChunkSize);
  std::vector<unsigned char> next(kChunkSize);
  input.read(
      reinterpret_cast<char*>(current.data()), static_cast<std::streamsize>(current.size())
  );
  std::size_t current_size = static_cast<std::size_t>(input.gcount());
  if (current_size == 0) {
    std::array<unsigned char, crypto_secretstream_xchacha20poly1305_ABYTES> cipher{};
    unsigned long long cipher_size = 0;
    crypto_secretstream_xchacha20poly1305_push(
        &state,
        cipher.data(),
        &cipher_size,
        nullptr,
        0,
        reinterpret_cast<const unsigned char*>(header_json.data()),
        header_json.size(),
        crypto_secretstream_xchacha20poly1305_TAG_FINAL
    );
    write_u32(output, stored_hash, stored_size, static_cast<std::uint32_t>(cipher_size));
    write_bytes(output, stored_hash, stored_size, cipher.data(), cipher_size);
  } else {
    while (true) {
      input.read(
          reinterpret_cast<char*>(next.data()), static_cast<std::streamsize>(next.size())
      );
      const std::size_t next_size = static_cast<std::size_t>(input.gcount());
      const bool final = next_size == 0;
      plain_hash.update(current.data(), current_size);
      plain_size += static_cast<long long>(current_size);
      std::vector<unsigned char> cipher(
          current_size + crypto_secretstream_xchacha20poly1305_ABYTES
      );
      unsigned long long cipher_size = 0;
      if (crypto_secretstream_xchacha20poly1305_push(
              &state,
              cipher.data(),
              &cipher_size,
              current.data(),
              current_size,
              reinterpret_cast<const unsigned char*>(header_json.data()),
              header_json.size(),
              final ? crypto_secretstream_xchacha20poly1305_TAG_FINAL : 0
          ) != 0) {
        throw std::runtime_error("failed to encrypt HolderAsset1 chunk");
      }
      write_u32(output, stored_hash, stored_size, static_cast<std::uint32_t>(cipher_size));
      write_bytes(output, stored_hash, stored_size, cipher.data(), cipher_size);
      if (final) break;
      current.swap(next);
      current_size = next_size;
    }
  }
  if (!input.eof()) throw std::runtime_error("failed while reading asset source");
  output.close();
  return {
      "holder_asset_v1",
      {plain_size, plain_hash.finish()},
      {stored_size, stored_hash.finish()},
  };
}

void recover_asset_file(
    const std::filesystem::path& stored,
    const std::filesystem::path& destination,
    const holder::model::Project& project,
    const std::string& resource_id,
    const std::string& asset_id,
    const std::string& encoding,
    const FileDigest& expected_stored,
    const FileDigest& expected_plaintext
) {
  verify_digest(digest_file(stored), expected_stored, "stored asset");
  std::ifstream input(stored, std::ios::binary);
  if (!input) throw std::runtime_error("failed to open stored asset");
  std::filesystem::create_directories(destination.parent_path());
  std::ofstream output(destination, std::ios::binary | std::ios::trunc);
  if (!output) throw std::runtime_error("failed to open recovered asset");
  make_private(destination);

  Sha256 plain_hash;
  long long plain_size = 0;
  if (encoding == "plain") {
    std::array<char, kChunkSize> buffer{};
    while (input) {
      input.read(buffer.data(), buffer.size());
      const auto count = input.gcount();
      if (count > 0) {
        output.write(buffer.data(), count);
        plain_hash.update(buffer.data(), static_cast<std::size_t>(count));
        plain_size += count;
      }
    }
  } else if (encoding == "holder_asset_v1") {
    ensure_sodium();
    std::string magic(kMagic.size(), '\0');
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (magic != kMagic) throw std::runtime_error("invalid HolderAsset1 header");
    const auto header_size = read_u32(input);
    if (header_size == 0 || header_size > 1024 * 1024) {
      throw std::runtime_error("invalid HolderAsset1 metadata size");
    }
    std::string header_json(header_size, '\0');
    input.read(header_json.data(), static_cast<std::streamsize>(header_json.size()));
    if (input.gcount() != static_cast<std::streamsize>(header_json.size())) {
      throw std::runtime_error("truncated HolderAsset1 metadata");
    }
    if (nlohmann::json::parse(header_json) != authenticated_header(project, resource_id, asset_id)) {
      throw std::runtime_error("HolderAsset1 identity mismatch");
    }
    std::array<unsigned char, crypto_secretstream_xchacha20poly1305_HEADERBYTES> stream_header{};
    input.read(reinterpret_cast<char*>(stream_header.data()), stream_header.size());
    if (input.gcount() != static_cast<std::streamsize>(stream_header.size())) {
      throw std::runtime_error("truncated HolderAsset1 stream header");
    }
    const auto key = holder::privacy::load_project_key_bytes(project.project_id, require_key_id(project));
    crypto_secretstream_xchacha20poly1305_state state{};
    if (crypto_secretstream_xchacha20poly1305_init_pull(&state, stream_header.data(), key.data()) != 0) {
      throw std::runtime_error("failed to initialise HolderAsset1 decryption");
    }
    bool final_seen = false;
    while (!final_seen) {
      const auto cipher_size = read_u32(input);
      if (cipher_size < crypto_secretstream_xchacha20poly1305_ABYTES ||
          cipher_size > kChunkSize + crypto_secretstream_xchacha20poly1305_ABYTES) {
        throw std::runtime_error("invalid HolderAsset1 chunk size");
      }
      std::vector<unsigned char> cipher(cipher_size);
      input.read(
          reinterpret_cast<char*>(cipher.data()), static_cast<std::streamsize>(cipher.size())
      );
      if (input.gcount() != static_cast<std::streamsize>(cipher.size())) {
        throw std::runtime_error("truncated HolderAsset1 chunk");
      }
      std::vector<unsigned char> plain(cipher_size);
      unsigned long long plain_count = 0;
      unsigned char tag = 0;
      if (crypto_secretstream_xchacha20poly1305_pull(
              &state,
              plain.data(),
              &plain_count,
              &tag,
              cipher.data(),
              cipher.size(),
              reinterpret_cast<const unsigned char*>(header_json.data()),
              header_json.size()
          ) != 0) {
        throw std::runtime_error("HolderAsset1 authentication failed");
      }
      if (tag != 0 && tag != crypto_secretstream_xchacha20poly1305_TAG_FINAL) {
        throw std::runtime_error("unsupported HolderAsset1 stream tag");
      }
      output.write(
          reinterpret_cast<const char*>(plain.data()), static_cast<std::streamsize>(plain_count)
      );
      if (!output) throw std::runtime_error("failed to write recovered asset");
      plain_hash.update(plain.data(), static_cast<std::size_t>(plain_count));
      plain_size += static_cast<long long>(plain_count);
      final_seen = tag == crypto_secretstream_xchacha20poly1305_TAG_FINAL;
    }
    if (input.peek() != std::char_traits<char>::eof()) {
      throw std::runtime_error("trailing data after HolderAsset1 final chunk");
    }
  } else {
    throw std::runtime_error("unsupported asset encoding: " + encoding);
  }
  output.close();
  verify_digest({plain_size, plain_hash.finish()}, expected_plaintext, "plaintext asset");
}

} // namespace holder::resource
