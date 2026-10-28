#pragma once

#include <cstddef>
#include <cstdint>

namespace eternalsonata {

// AES-128 with T tables: a whole PS3 disc goes through it, so it has to keep
// up with the drive.
class Aes128 {
 public:
  explicit Aes128(const uint8_t key[16]);

  void Encrypt(uint8_t block[16]) const;
  void Decrypt(uint8_t block[16]) const;
  // `size` is a multiple of 16; `iv` is left at the last ciphertext block.
  void DecryptCbc(uint8_t* data, size_t size, uint8_t iv[16]) const;

 private:
  uint32_t enc_[44];
  uint32_t dec_[44];
};

}  // namespace eternalsonata
