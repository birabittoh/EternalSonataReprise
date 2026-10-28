#include "ps3_pfd.h"

#include <cstring>

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

// A small table AES-128; saves are a few megabytes, so speed does not matter.
uint8_t Mul(uint8_t a, uint8_t b) {
  uint8_t r = 0;
  while (b) {
    if (b & 1)
      r ^= a;
    a = uint8_t((a << 1) ^ ((a & 0x80) ? 0x1B : 0));
    b >>= 1;
  }
  return r;
}

struct SBoxes {
  uint8_t fwd[256];
  uint8_t inv[256];
  SBoxes() {
    for (int x = 0; x < 256; ++x) {
      // x^254 is the multiplicative inverse, 0 for 0.
      uint8_t v = 1, base = uint8_t(x);
      for (int e = 254; e; e >>= 1, base = Mul(base, base))
        if (e & 1)
          v = Mul(v, base);
      if (!x)
        v = 0;
      uint8_t s = 0x63 ^ v;
      for (int r = 1; r <= 4; ++r)
        s ^= uint8_t((v << r) | (v >> (8 - r)));
      fwd[x] = s;
      inv[s] = uint8_t(x);
    }
  }
};
const SBoxes& Boxes() {
  static const SBoxes boxes;
  return boxes;
}

class Aes128 {
 public:
  explicit Aes128(const uint8_t key[16]) {
    const SBoxes& sb = Boxes();
    std::memcpy(round_keys_, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
      uint8_t t[4];
      std::memcpy(t, round_keys_ + i - 4, 4);
      if (i % 16 == 0) {
        const uint8_t first = t[0];
        t[0] = sb.fwd[t[1]] ^ rcon;
        t[1] = sb.fwd[t[2]];
        t[2] = sb.fwd[t[3]];
        t[3] = sb.fwd[first];
        rcon = Mul(rcon, 2);
      }
      for (int j = 0; j < 4; ++j)
        round_keys_[i + j] = round_keys_[i - 16 + j] ^ t[j];
    }
  }

  void Encrypt(uint8_t b[16]) const {
    const SBoxes& sb = Boxes();
    AddRoundKey(b, 0);
    for (int round = 1; round <= 10; ++round) {
      uint8_t t[16];
      for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
          t[r + 4 * c] = sb.fwd[b[r + 4 * ((c + r) % 4)]];
      if (round != 10) {
        for (int c = 0; c < 4; ++c) {
          uint8_t* a = t + 4 * c;
          const uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
          a[0] = Mul(a0, 2) ^ Mul(a1, 3) ^ a2 ^ a3;
          a[1] = a0 ^ Mul(a1, 2) ^ Mul(a2, 3) ^ a3;
          a[2] = a0 ^ a1 ^ Mul(a2, 2) ^ Mul(a3, 3);
          a[3] = Mul(a0, 3) ^ a1 ^ a2 ^ Mul(a3, 2);
        }
      }
      std::memcpy(b, t, 16);
      AddRoundKey(b, round);
    }
  }

  void Decrypt(uint8_t b[16]) const {
    const SBoxes& sb = Boxes();
    AddRoundKey(b, 10);
    for (int round = 9; round >= 0; --round) {
      uint8_t t[16];
      for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
          t[r + 4 * ((c + r) % 4)] = sb.inv[b[r + 4 * c]];
      std::memcpy(b, t, 16);
      AddRoundKey(b, round);
      if (round) {
        for (int c = 0; c < 4; ++c) {
          uint8_t* a = b + 4 * c;
          const uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
          a[0] = Mul(a0, 14) ^ Mul(a1, 11) ^ Mul(a2, 13) ^ Mul(a3, 9);
          a[1] = Mul(a0, 9) ^ Mul(a1, 14) ^ Mul(a2, 11) ^ Mul(a3, 13);
          a[2] = Mul(a0, 13) ^ Mul(a1, 9) ^ Mul(a2, 14) ^ Mul(a3, 11);
          a[3] = Mul(a0, 11) ^ Mul(a1, 13) ^ Mul(a2, 9) ^ Mul(a3, 14);
        }
      }
    }
  }

 private:
  void AddRoundKey(uint8_t b[16], int round) const {
    for (int i = 0; i < 16; ++i)
      b[i] ^= round_keys_[16 * round + i];
  }

  uint8_t round_keys_[176];
};

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
