#include "ps3_pfd.h"

#include <cstring>

#include "aes128.h"

namespace eternalsonata {
namespace {

using Bytes = std::vector<uint8_t>;

// Bruteforce Save Data's games.conf: one id for every release of the game.
constexpr uint8_t kSecureFileId[16] = {0x35, 0xFC, 0xBD, 0x0E, 0xF4, 0x8B, 0x9A, 0xE7,
                                       0x23, 0xDC, 0xCD, 0xCE, 0x15, 0xF2, 0xC4, 0x54};
constexpr uint8_t kSysconManagerKey[16] = {0xD4, 0x13, 0xB8, 0x96, 0x63, 0xE1, 0xFE, 0x9F,
                                           0x75, 0x14, 0x3D, 0x3B, 0xB4, 0x56, 0x52, 0x74};

constexpr size_t kHashTableOffset = 0x60;
constexpr size_t kEntrySize = 272;
constexpr size_t kEntryNameOffset = 8;
constexpr size_t kEntryNameSize = 65;
constexpr size_t kEntryKeyOffset = 80;
constexpr size_t kEntryKeySize = 64;
constexpr size_t kEntryFileSizeOffset = 264;

uint64_t U64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i)
    v = (v << 8) | p[i];
  return v;
}

}  // namespace

bool DecryptPs3ProtectedFile(const Bytes& pfd, const std::string& name, Bytes& data) {
  if (pfd.size() < kHashTableOffset + 24 || U64(pfd.data()) != 0x50464442ull)
    return false;
  const uint64_t capacity = U64(&pfd[kHashTableOffset]);
  const uint64_t used = U64(&pfd[kHashTableOffset + 16]);
  const uint64_t entries = kHashTableOffset + 24 + capacity * 8;
  if (capacity > pfd.size() || used > pfd.size() || entries + used * kEntrySize > pfd.size())
    return false;

  for (uint64_t n = 0; n < used; ++n) {
    const uint8_t* entry = &pfd[entries + n * kEntrySize];
    const char* entry_name = reinterpret_cast<const char*>(entry + kEntryNameOffset);
    if (std::string(entry_name, strnlen(entry_name, kEntryNameSize)) != name)
      continue;
    const uint64_t size = U64(entry + kEntryFileSizeOffset);
    if (size > data.size() || data.size() % 16)
      return false;

    // The entry key is CBC encrypted under the syscon manager key, with an IV
    // made from the secure file id; its first 16 bytes are the file key.
    uint8_t iv[16] = {kSecureFileId[0], 11, 15, kSecureFileId[1], kSecureFileId[2], 14,
                      kSecureFileId[3], kSecureFileId[4], 10};
    std::memcpy(iv + 9, kSecureFileId + 5, 7);
    uint8_t key[kEntryKeySize];
    std::memcpy(key, entry + kEntryKeyOffset, kEntryKeySize);
    const Aes128 syscon(kSysconManagerKey);
    for (size_t off = 0; off < kEntryKeySize; off += 16) {
      uint8_t cipher[16];
      std::memcpy(cipher, key + off, 16);
      syscon.Decrypt(key + off);
      for (int i = 0; i < 16; ++i)
        key[off + i] ^= iv[i];
      std::memcpy(iv, cipher, 16);
    }

    // Each block is ECB decrypted, then XORed with its encrypted index.
    const Aes128 file(key);
    for (size_t block = 0; block < data.size() / 16; ++block) {
      uint8_t counter[16] = {};
      for (int i = 0; i < 8; ++i)
        counter[i] = uint8_t(uint64_t(block) >> (56 - 8 * i));
      file.Encrypt(counter);
      uint8_t* b = &data[block * 16];
      file.Decrypt(b);
      for (int i = 0; i < 16; ++i)
        b[i] ^= counter[i];
    }
    data.resize(size);
    return true;
  }
  return false;
}

}  // namespace eternalsonata
