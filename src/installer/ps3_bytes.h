#pragma once

// Byte helpers with Python's semantics, so the port of ps3_convert.py reads
// like it: slices clamp, reads past the end throw like struct.error.

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace eternalsonata::ps3 {

using Bytes = std::vector<uint8_t>;

inline void Need(const Bytes& d, size_t o, size_t n) {
  if (o > d.size() || d.size() - o < n)
    throw std::out_of_range("read past the end of a PS3 file");
}

inline uint8_t At(const Bytes& d, size_t o) {
  Need(d, o, 1);
  return d[o];
}

inline uint32_t Rd16(const Bytes& d, size_t o) {
  Need(d, o, 2);
  return uint32_t(d[o]) << 8 | d[o + 1];
}

inline uint32_t Rd32(const Bytes& d, size_t o) {
  Need(d, o, 4);
  return uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | d[o + 3];
}

inline void Wr32(Bytes& d, size_t o, uint64_t v) {
  Need(d, o, 4);
  for (int i = 0; i < 4; ++i)
    d[o + i] = uint8_t(v >> (24 - 8 * i));
}

inline void Put32(Bytes& d, uint64_t v) {
  for (int i = 0; i < 4; ++i)
    d.push_back(uint8_t(v >> (24 - 8 * i)));
}

inline void Put16(Bytes& d, uint32_t v) {
  d.push_back(uint8_t(v >> 8));
  d.push_back(uint8_t(v));
}

inline void PutLe32(Bytes& d, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    d.push_back(uint8_t(v >> (8 * i)));
}

inline void PutLe16(Bytes& d, uint32_t v) {
  d.push_back(uint8_t(v));
  d.push_back(uint8_t(v >> 8));
}

// d[a:b]
inline Bytes Slice(const Bytes& d, size_t a, size_t b) {
  a = std::min(a, d.size());
  b = std::min(b, d.size());
  return a < b ? Bytes(d.begin() + a, d.begin() + b) : Bytes();
}

inline void Append(Bytes& d, const Bytes& more) {
  d.insert(d.end(), more.begin(), more.end());
}

inline void Append(Bytes& d, const char* text, size_t n) {
  d.insert(d.end(), text, text + n);
}

// d[a:b] = with
inline void Splice(Bytes& d, size_t a, size_t b, const Bytes& with) {
  a = std::min(a, d.size());
  b = std::max(a, std::min(b, d.size()));
  d.erase(d.begin() + a, d.begin() + b);
  d.insert(d.begin() + a, with.begin(), with.end());
}

}  // namespace eternalsonata::ps3
