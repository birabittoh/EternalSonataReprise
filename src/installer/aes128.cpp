#include "aes128.h"

#include <cstring>

namespace eternalsonata {
namespace {

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

uint32_t Rotr8(uint32_t v) {
  return (v >> 8) | (v << 24);
}

struct Tables {
  uint8_t s[256];
  uint8_t si[256];
  uint32_t te[4][256];
  uint32_t td[4][256];

  Tables() {
    for (int x = 0; x < 256; ++x) {
      // x^254 is the multiplicative inverse, 0 for 0.
      uint8_t v = 1, base = uint8_t(x);
      for (int e = 254; e; e >>= 1, base = Mul(base, base))
        if (e & 1)
          v = Mul(v, base);
      if (!x)
        v = 0;
      uint8_t box = 0x63 ^ v;
      for (int r = 1; r <= 4; ++r)
        box ^= uint8_t((v << r) | (v >> (8 - r)));
      s[x] = box;
      si[box] = uint8_t(x);
    }
    for (int x = 0; x < 256; ++x) {
      const uint8_t f = s[x], i = si[x];
      te[0][x] = (uint32_t(Mul(f, 2)) << 24) | (uint32_t(f) << 16) | (uint32_t(f) << 8) | Mul(f, 3);
      td[0][x] = (uint32_t(Mul(i, 14)) << 24) | (uint32_t(Mul(i, 9)) << 16) |
                 (uint32_t(Mul(i, 13)) << 8) | Mul(i, 11);
      for (int t = 1; t < 4; ++t) {
        te[t][x] = Rotr8(te[t - 1][x]);
        td[t][x] = Rotr8(td[t - 1][x]);
      }
    }
  }
};

const Tables& T() {
  static const Tables tables;
  return tables;
}

uint32_t Load(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void Store(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

}  // namespace

Aes128::Aes128(const uint8_t key[16]) {
  const Tables& t = T();
  for (int i = 0; i < 4; ++i)
    enc_[i] = Load(key + 4 * i);
  uint32_t rcon = 0x01000000;
  for (int i = 4; i < 44; ++i) {
    uint32_t w = enc_[i - 1];
    if (i % 4 == 0) {
      w = (uint32_t(t.s[(w >> 16) & 0xFF]) << 24) | (uint32_t(t.s[(w >> 8) & 0xFF]) << 16) |
          (uint32_t(t.s[w & 0xFF]) << 8) | t.s[w >> 24];
      w ^= rcon;
      rcon = uint32_t(Mul(uint8_t(rcon >> 24), 2)) << 24;
    }
    enc_[i] = enc_[i - 4] ^ w;
  }
  // The equivalent inverse cipher: rounds reversed, inner ones through InvMixColumns.
  for (int r = 0; r <= 10; ++r)
    for (int j = 0; j < 4; ++j)
      dec_[4 * r + j] = enc_[4 * (10 - r) + j];
  for (int i = 4; i < 40; ++i) {
    const uint32_t w = dec_[i];
    dec_[i] = t.td[0][t.s[w >> 24]] ^ t.td[1][t.s[(w >> 16) & 0xFF]] ^
              t.td[2][t.s[(w >> 8) & 0xFF]] ^ t.td[3][t.s[w & 0xFF]];
  }
}

void Aes128::Encrypt(uint8_t block[16]) const {
  const Tables& t = T();
  const uint32_t* rk = enc_;
  uint32_t s0 = Load(block) ^ rk[0], s1 = Load(block + 4) ^ rk[1];
  uint32_t s2 = Load(block + 8) ^ rk[2], s3 = Load(block + 12) ^ rk[3];
  for (int round = 1; round < 10; ++round) {
    rk += 4;
    const uint32_t t0 = t.te[0][s0 >> 24] ^ t.te[1][(s1 >> 16) & 0xFF] ^ t.te[2][(s2 >> 8) & 0xFF] ^
                        t.te[3][s3 & 0xFF] ^ rk[0];
    const uint32_t t1 = t.te[0][s1 >> 24] ^ t.te[1][(s2 >> 16) & 0xFF] ^ t.te[2][(s3 >> 8) & 0xFF] ^
                        t.te[3][s0 & 0xFF] ^ rk[1];
    const uint32_t t2 = t.te[0][s2 >> 24] ^ t.te[1][(s3 >> 16) & 0xFF] ^ t.te[2][(s0 >> 8) & 0xFF] ^
                        t.te[3][s1 & 0xFF] ^ rk[2];
    const uint32_t t3 = t.te[0][s3 >> 24] ^ t.te[1][(s0 >> 16) & 0xFF] ^ t.te[2][(s1 >> 8) & 0xFF] ^
                        t.te[3][s2 & 0xFF] ^ rk[3];
    s0 = t0, s1 = t1, s2 = t2, s3 = t3;
  }
  rk += 4;
  auto last = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t k) {
    return ((uint32_t(t.s[a >> 24]) << 24) | (uint32_t(t.s[(b >> 16) & 0xFF]) << 16) |
            (uint32_t(t.s[(c >> 8) & 0xFF]) << 8) | t.s[d & 0xFF]) ^
           k;
  };
  Store(block, last(s0, s1, s2, s3, rk[0]));
  Store(block + 4, last(s1, s2, s3, s0, rk[1]));
  Store(block + 8, last(s2, s3, s0, s1, rk[2]));
  Store(block + 12, last(s3, s0, s1, s2, rk[3]));
}

void Aes128::Decrypt(uint8_t block[16]) const {
  const Tables& t = T();
  const uint32_t* rk = dec_;
  uint32_t s0 = Load(block) ^ rk[0], s1 = Load(block + 4) ^ rk[1];
  uint32_t s2 = Load(block + 8) ^ rk[2], s3 = Load(block + 12) ^ rk[3];
  for (int round = 1; round < 10; ++round) {
    rk += 4;
    const uint32_t t0 = t.td[0][s0 >> 24] ^ t.td[1][(s3 >> 16) & 0xFF] ^ t.td[2][(s2 >> 8) & 0xFF] ^
                        t.td[3][s1 & 0xFF] ^ rk[0];
    const uint32_t t1 = t.td[0][s1 >> 24] ^ t.td[1][(s0 >> 16) & 0xFF] ^ t.td[2][(s3 >> 8) & 0xFF] ^
                        t.td[3][s2 & 0xFF] ^ rk[1];
    const uint32_t t2 = t.td[0][s2 >> 24] ^ t.td[1][(s1 >> 16) & 0xFF] ^ t.td[2][(s0 >> 8) & 0xFF] ^
                        t.td[3][s3 & 0xFF] ^ rk[2];
    const uint32_t t3 = t.td[0][s3 >> 24] ^ t.td[1][(s2 >> 16) & 0xFF] ^ t.td[2][(s1 >> 8) & 0xFF] ^
                        t.td[3][s0 & 0xFF] ^ rk[3];
    s0 = t0, s1 = t1, s2 = t2, s3 = t3;
  }
  rk += 4;
  auto last = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t k) {
    return ((uint32_t(t.si[a >> 24]) << 24) | (uint32_t(t.si[(b >> 16) & 0xFF]) << 16) |
            (uint32_t(t.si[(c >> 8) & 0xFF]) << 8) | t.si[d & 0xFF]) ^
           k;
  };
  Store(block, last(s0, s3, s2, s1, rk[0]));
  Store(block + 4, last(s1, s0, s3, s2, rk[1]));
  Store(block + 8, last(s2, s1, s0, s3, rk[2]));
  Store(block + 12, last(s3, s2, s1, s0, rk[3]));
}

void Aes128::DecryptCbc(uint8_t* data, size_t size, uint8_t iv[16]) const {
  uint8_t cipher[16];
  for (size_t off = 0; off + 16 <= size; off += 16) {
    uint8_t* b = data + off;
    std::memcpy(cipher, b, 16);
    Decrypt(b);
    for (int i = 0; i < 16; ++i)
      b[i] ^= iv[i];
    std::memcpy(iv, cipher, 16);
  }
}

}  // namespace eternalsonata
