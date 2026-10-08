#include "ps3_convert.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <list>
#include <set>
#include <stdexcept>
#include <string_view>

#include "ps3_bytes.h"

namespace eternalsonata::ps3 {
namespace {

std::string Hex(size_t v) {
  char text[24];
  std::snprintf(text, sizeof(text), "%#zx", v);
  return text;
}

std::string Tag(const Bytes& d, size_t o) {
  return std::string(reinterpret_cast<const char*>(d.data() + o), 4);
}

bool HasTag(const Bytes& d, size_t o, const char* tag) {
  return o + 4 <= d.size() && std::memcmp(d.data() + o, tag, 4) == 0;
}

Bytes Chunk(const char* tag, const Bytes& payload) {
  Bytes out(tag, tag + 4);
  Put32(out, 8 + payload.size());
  Append(out, payload);
  return out;
}

Bytes Npad(size_t size) {
  return Chunk("NPAD", Bytes(size - 8));
}

// Python's -x % n for a positive n.
size_t PadTo(int64_t x, int64_t n) {
  return size_t(((-x) % n + n) % n);
}

// ---------------------------------------------------------------------------
// Colours: RSX vertex and material colours are RGBA, Xenos D3DCOLOR is ARGB.
// ---------------------------------------------------------------------------
void Argb(uint8_t* w) {
  std::rotate(w, w + 3, w + 4);
}

void RotateWord(Bytes& buf, int64_t o) {
  if (o >= 0 && size_t(o) + 4 <= buf.size())
    Argb(buf.data() + o);
}

// ---------------------------------------------------------------------------
// NTX3 (a one-texture GTF) -> NTEX (a PC DDS)
// ---------------------------------------------------------------------------
constexpr uint8_t kGtfA8R8G8B8 = 0x85;
constexpr uint8_t kGtfLinear = 0x20;
constexpr uint8_t kGtfNormalized = 0x40;

const char* GtfDxt(uint8_t base) {
  switch (base) {
    case 0x86:
      return "DXT1";
    case 0x87:
      return "DXT3";
    case 0x88:
      return "DXT5";
  }
  return nullptr;
}

Bytes DdsHeader(uint32_t width, uint32_t height, uint32_t mips, const char* fourcc,
                uint32_t top_size) {
  const uint32_t flags = 0x81007 | (mips > 1 ? 0x20000 : 0);
  const uint32_t caps = mips > 1 ? 0x401008 : 0x1000;
  Bytes out{'D', 'D', 'S', ' '};
  for (uint32_t v : {124u, flags, height, width, top_size, 0u, mips > 1 ? mips : 0u})
    PutLe32(out, v);
  out.resize(out.size() + 44);
  PutLe32(out, 32);
  if (fourcc) {
    PutLe32(out, 4);
    out.insert(out.end(), fourcc, fourcc + 4);
    for (int i = 0; i < 5; ++i)
      PutLe32(out, 0);
  } else {
    PutLe32(out, 0x41);
    out.resize(out.size() + 4);
    for (uint32_t v : {32u, 0xFF0000u, 0xFF00u, 0xFFu, 0xFF000000u})
      PutLe32(out, v);
  }
  for (uint32_t v : {caps, 0u, 0u, 0u})
    PutLe32(out, v);
  out.resize(out.size() + 4);
  return out;
}

// Linear GTF levels keep the base level's row pitch all the way down the
// chain; DDS packs every level tight.
Bytes Unpitch(const Bytes& pixels, uint32_t width, uint32_t height, uint32_t mips, size_t pitch,
              uint32_t block, size_t block_bytes) {
  Bytes out;
  size_t at = 0;
  for (uint32_t level = 0; level < std::max(1u, mips); ++level) {
    const uint32_t w = std::max(1u, width >> level), h = std::max(1u, height >> level);
    const size_t cols = (w + block - 1) / block;
    const size_t rows = (h + block - 1) / block;
    const size_t row = cols * block_bytes;
    for (size_t r = 0; r < rows; ++r)
      Append(out, Slice(pixels, at + r * pitch, at + r * pitch + row));
    at += rows * pitch;
  }
  return out;
}

std::optional<Bytes> ConvertNtx3(const Bytes& d, size_t o, Report& report) {
  const uint32_t count = Rd32(d, o + 8), off = Rd32(d, o + 16), tsz = Rd32(d, o + 20);
  const uint8_t fmt = At(d, o + 24), mips = At(d, o + 25), dim = At(d, o + 26),
                cube = At(d, o + 27);
  const uint32_t width = Rd16(d, o + 32), height = Rd16(d, o + 34), depth = Rd16(d, o + 36);
  const uint32_t pitch = Rd32(d, o + 40);
  const uint8_t base = fmt & ~(kGtfLinear | kGtfNormalized);
  // The texture offset counts from the chunk tag, not from the GTF header.
  Bytes pixels = Slice(d, o + off, size_t(o) + off + tsz);
  if (count != 1 || dim != 2 || cube || depth != 1) {
    report.Warn("NTX3 at " + Hex(o) + ": unsupported layout count=" + std::to_string(count) +
                " dim=" + std::to_string(dim) + " cube=" + std::to_string(cube));
    return std::nullopt;
  }
  const bool linear = (fmt & kGtfLinear) && pitch;
  Bytes out;
  if (const char* fourcc = GtfDxt(base)) {
    const size_t block = base == 0x86 ? 8 : 16;
    if (linear)
      pixels = Unpitch(pixels, width, height, mips, pitch, 4, block);
    const size_t top = size_t(std::max(1u, (width + 3) / 4)) * std::max(1u, (height + 3) / 4) * block;
    out = DdsHeader(width, height, mips, fourcc, uint32_t(top));
    Append(out, pixels);
  } else if (base == kGtfA8R8G8B8) {
    if (linear)
      pixels = Unpitch(pixels, width, height, mips, pitch, 1, 4);
    // ARGB big endian to the DDS's little endian BGRA.
    if (pixels.size() % 4)
      throw std::runtime_error("NTX3 at " + Hex(o) + ": pixels are not whole words");
    for (size_t i = 0; i < pixels.size(); i += 4)
      std::reverse(pixels.begin() + i, pixels.begin() + i + 4);
    out = DdsHeader(width, height, mips, nullptr, width * height * 4);
    Append(out, pixels);
  } else {
    report.Warn("NTX3 at " + Hex(o) + ": unsupported GTF format " + Hex(fmt));
    return std::nullopt;
  }
  ++report.counts["NTX3->NTEX"];
  return Chunk("NTEX", out);
}

// ---------------------------------------------------------------------------
// NSHP vertices. The element order and Xenos types are the ones sub_82131148
// builds from the format word at +0x1C; RSX stores the same elements in the
// same order, only packed differently.
// ---------------------------------------------------------------------------
enum class Element { kCopy, kWeights, kNormal, kColour };

struct Layout {
  Element kind;
  size_t size;
  size_t ps3_size;
};

std::optional<std::vector<Layout>> VertexLayout(uint32_t fmt) {
  std::vector<Layout> els;
  auto add = [&](Element kind, size_t size, size_t ps3_size = 0) {
    els.push_back({kind, size, ps3_size ? ps3_size : size});
  };
  if (fmt & 0x1)
    add(Element::kCopy, 12);  // position FLOAT3
  if (fmt & 0x400) {
    add(Element::kWeights, 12, 4);  // FLOAT3 here, one CMP dword on RSX
    add(Element::kCopy, 4);         // blend indices UBYTE4
  }
  if (fmt & 0x2)
    add(Element::kNormal, 4);  // DEC3N here, CMP on RSX
  if (fmt & 0x4)
    add(Element::kNormal, 4);  // tangent, same packing
  if (fmt & 0x8)
    add(Element::kColour, 4);
  for (uint32_t i = 0; i < ((fmt & 0xFF) >> 4); ++i)
    add(Element::kCopy, 4);  // FLOAT16_2 texcoords
  for (uint32_t i = 0; i < ((fmt >> 16) & 3); ++i)
    add(Element::kCopy, 8);  // FLOAT2 texcoords
  if (fmt & 0x4000)
    add(Element::kCopy, 4);
  if (fmt & 0x8000)
    add(Element::kCopy, 24);
  if (fmt & 0x41000)
    add(Element::kCopy, 12);
  if (fmt & 0x40000)
    return std::nullopt;  // morph layout, never seen shipped
  return els;
}

int32_t Sext(uint32_t value, int bits) {
  return (value >> (bits - 1) & 1) ? int32_t(value) - (1 << bits) : int32_t(value);
}

// RSX CELL_GCM_VERTEX_CMP: signed 11:11:10, x in the low bits.
std::array<double, 3> CmpUnpack(uint32_t word) {
  return {Sext(word & 0x7FF, 11) / 1023.0, Sext((word >> 11) & 0x7FF, 11) / 1023.0,
          Sext(word >> 22, 10) / 511.0};
}

uint32_t Dec3n(const std::array<double, 3>& v) {
  auto q = [](double x) {
    const double r = std::floor(x * 511.0 + 0.5);
    return uint32_t(int32_t(std::max(-511.0, std::min(511.0, r)))) & 0x3FF;
  };
  return q(v[0]) | q(v[1]) << 10 | q(v[2]) << 20;
}

float F32(double v) {
  return static_cast<float>(v);
}

// The 360 ships the authored weights (0.9, 0.1) with the last nonzero one
// computed as 1 minus the rest in float32; the RSX copy is quantised to
// 1/1023, so snap back to the nearest thousandth and redo that sum.
std::array<float, 3> SkinWeights(uint32_t word) {
  std::array<float, 3> ws;
  const auto unpacked = CmpUnpack(word);
  for (int i = 0; i < 3; ++i) {
    const double w = unpacked[i];
    // Python's round(w, 3); no w = k/1023 lies near a tie.
    const double r = std::nearbyint(w * 1000.0) / 1000.0;
    ws[i] = F32(std::abs(r - w) < 0.5 / 1023 ? r : w);
  }
  int last = 0;
  for (int i = 0; i < 3; ++i) {
    if (ws[i] != 0.0f)
      last = i;
  }
  if (last) {
    float rest = 1.0f;
    for (int i = 0; i < last; ++i)
      rest = F32(double(rest) - double(ws[i]));
    if (std::abs(double(rest) - double(ws[last])) < 1.0 / 1023)
      ws[last] = rest;
  }
  return ws;
}

void PutFloat(Bytes& out, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  Put32(out, bits);
}

std::optional<Bytes> ConvertNshp(const Bytes& d, size_t o, Report& report) {
  const size_t size = Rd32(d, o + 4);
  const uint32_t flags = Rd16(d, o + 0x18), nv = Rd16(d, o + 0x1A), fmt = Rd16(d, o + 0x1C);
  const uint32_t bones = At(d, o + 0x20);
  const auto els = VertexLayout(fmt);
  if (!els) {
    report.Warn("NSHP at " + Hex(o) + ": unsupported vertex format " + Hex(fmt));
    return std::nullopt;
  }
  size_t v0 = 0x38 + (flags & 0x80 ? 32 : 0);
  if (bones)
    v0 += (bones * 2 + 3) & ~3u;
  size_t ps3_stride = 0, x_stride = 0;
  for (const Layout& e : *els) {
    ps3_stride += e.ps3_size;
    x_stride += e.size;
  }
  if (v0 + nv * ps3_stride > size) {
    report.Warn("NSHP at " + Hex(o) + ": " + std::to_string(nv) + " vertices of " +
                std::to_string(ps3_stride) + " bytes overrun the chunk");
    return std::nullopt;
  }
  Bytes out = Slice(d, o, o + v0);
  size_t src = o + v0;
  for (uint32_t i = 0; i < nv; ++i) {
    for (const Layout& e : *els) {
      Bytes v = Slice(d, src, src + e.ps3_size);
      switch (e.kind) {
        case Element::kCopy:
          Append(out, v);
          break;
        case Element::kColour:
          // v[3:] + v[:3]: a short read past the chunk stays as it is.
          if (v.size() == 4)
            Argb(v.data());
          Append(out, v);
          break;
        case Element::kNormal:
          Put32(out, Dec3n(CmpUnpack(Rd32(v, 0))));
          break;
        case Element::kWeights:
          for (float w : SkinWeights(Rd32(v, 0)))
            PutFloat(out, w);
          break;
      }
      src += e.ps3_size;
    }
  }
  Append(out, Slice(d, src, o + size));
  Wr32(out, 4, out.size());
  ++report.counts["NSHP"];
  if (x_stride != ps3_stride)
    ++report.counts["NSHP resized"];
  return out;
}

// A morph stream (sub_82117018): per NSHP of the model, in order, one record
// per vertex, then a u16 bone count, the u16 bone ids and a u16 pad when the
// count is even. 360 records are float3 normal, float3 position, DEC3N
// normal, float3 weights, UBYTE4 indices (44 bytes); the PS3 packs the
// normals and weights into CMP dwords (28 bytes).
std::optional<Bytes> ConvertNmr2(const Bytes& d, size_t o, const std::vector<uint32_t>& nvs,
                                 Report& report) {
  const size_t size = Rd32(d, o + 4);
  Bytes out = Slice(d, o, o + 8);
  size_t p = o + 8;
  for (uint32_t nv : nvs) {
    if (p + 28 * size_t(nv) + 2 > o + size) {
      report.Warn("NMR2 at " + Hex(o) + ": records overrun the chunk");
      return std::nullopt;
    }
    for (uint32_t i = 0; i < nv; ++i) {
      for (double v : CmpUnpack(Rd32(d, p)))
        PutFloat(out, F32(v));
      Append(out, Slice(d, p + 4, p + 16));
      Put32(out, Dec3n(CmpUnpack(Rd32(d, p + 16))));
      for (float w : SkinWeights(Rd32(d, p + 20)))
        PutFloat(out, w);
      Append(out, Slice(d, p + 24, p + 28));
      p += 28;
    }
    const uint32_t bones = Rd16(d, p);
    const size_t tail = 2 + 2 * size_t(bones) + (bones % 2 == 0 ? 2 : 0);
    Append(out, Slice(d, p, p + tail));
    p += tail;
  }
  if (p != o + size) {
    report.Warn("NMR2 at " + Hex(o) + ": " + std::to_string(int64_t(o + size) - int64_t(p)) +
                " bytes left over");
    return std::nullopt;
  }
  Wr32(out, 4, out.size());
  ++report.counts["NMR2"];
  return out;
}

// ---------------------------------------------------------------------------
// Small chunks whose only difference is colour byte order.
// ---------------------------------------------------------------------------
std::optional<Bytes> ConvertColours(const Bytes& d, size_t o, const std::string& tag,
                                    Report& report) {
  const int64_t size = Rd32(d, o + 4);
  Bytes buf = Slice(d, o, o + size_t(size));
  if (tag == "NMTR") {
    // 96 byte materials; +8 is a flags word. With its bit 0 set, +4 is the
    // texture index rather than a colour.
    for (int64_t m = 8; m < size - 95; m += 96) {
      for (int64_t k : {4, 36, 40, 44}) {
        if (k != 4 || !(At(buf, size_t(m + 11)) & 1))
          RotateWord(buf, m + k);
      }
    }
  } else if (tag == "NLIT") {
    for (int64_t r = 8; r < size - 47; r += 48) {
      RotateWord(buf, r + 20);
      RotateWord(buf, r + 28);
    }
  } else if (tag == "NFOG") {
    RotateWord(buf, 12);
  } else if (tag == "NCLC") {
    RotateWord(buf, 20);
    RotateWord(buf, 36);
  } else if (tag == "NOL2") {
    // Type 0 records keep their colour at +8, the typed ones at +24.
    for (int64_t r = 8; r < size - 31; r += 32)
      RotateWord(buf, r + (At(buf, size_t(r)) & 3 ? 24 : 8));
  } else if (tag == "NATR") {
    // Layout from sub_821100D8: after 8 byte nodes, 12 byte triangles, 6 byte
    // records and a u16 list, one colour per record when +24 is nonzero.
    const int64_t nodes = Rd16(buf, 16), words = Rd16(buf, 18), tris = Rd16(buf, 20),
                  recs = Rd16(buf, 22), scale = Rd16(buf, 24);
    if (scale) {
      const int64_t at = (32 + 8 * nodes + 12 * tris + 6 * recs + 2 * words + 3) & ~int64_t(3);
      if (at + 4 * recs != size) {
        report.Warn("NATR at " + Hex(o) + ": colours do not end the chunk");
        return std::nullopt;
      }
      for (int64_t w = at; w < size; w += 4)
        RotateWord(buf, w);
    }
  }
  ++report.counts[tag];
  return buf;
}

const std::set<std::string> kColourChunks = {"NMTR", "NLIT", "NFOG", "NCLC", "NOL2", "NATR"};
const std::set<std::string> kPassthrough = {"NPAD", "NCAM", "NLC2", "NBN2", "NMTN",
                                            "NMTB", "NCLS", "NTXA", "NDYN", "NMRP",
                                            "NSIG", "NRTE", "NAIR"};

// ---------------------------------------------------------------------------
// Chunk trees
// ---------------------------------------------------------------------------
// Accumulates converted bytes and an old -> new offset map.
struct Rebuild {
  Bytes out;
  std::vector<std::pair<size_t, size_t>> anchors;  // (old, new)

  void Mark(size_t old) { anchors.emplace_back(old, out.size()); }
};

struct Child {
  size_t o;
  std::string tag;
  size_t size;
};

std::vector<Child> Children(const Bytes& d, size_t start, size_t end) {
  std::vector<Child> out;
  for (size_t o = start; o + 8 <= end;) {
    const size_t size = Rd32(d, o + 4);
    if (size < 8 || o + size > end)
      break;
    out.push_back({o, Tag(d, o), size});
    o += size;
  }
  return out;
}

bool ChainOk(const Bytes& d, size_t start, size_t end) {
  size_t o = start;
  while (o + 8 <= end) {
    const size_t size = Rd32(d, o + 4);
    if (size < 8 || size & 3 || o + size > end || d[o] != 'N')
      return false;
    o += size;
  }
  return o == end;
}

size_t NmdlHeader(const Bytes& d, size_t o) {
  return Rd16(d, o + 32) & 2 ? 96 : 64;
}

bool IsNobj(const Bytes& d, size_t o) {
  if (o + 16 > d.size() || !HasTag(d, o, "NOBJ"))
    return false;
  const size_t size = Rd32(d, o + 4);
  return 16 <= size && size <= d.size() - o && size % 4 == 0 && ChainOk(d, o + 8, o + size);
}

bool IsNmdl(const Bytes& d, size_t o, size_t end) {
  if (o + 64 > end || !HasTag(d, o, "NMDL") || d[o + 8] != 0x83)
    return false;
  const size_t size = Rd32(d, o + 4);
  const size_t hdr = NmdlHeader(d, o);
  return hdr < size && size <= end - o && size % 4 == 0 && ChainOk(d, o + hdr, o + size);
}

bool IsNtx3(const Bytes& d, size_t o, size_t end) {
  return HasTag(d, o, "NTX3") && o + 0x30 <= end && Rd32(d, o + 8) == 1 &&
         Rd32(d, o + 16) == 0x80 && o + Rd32(d, o + 4) <= end;
}

// The first "NTX3" wholly inside [at, end), as bytes.find does.
std::optional<size_t> FindNtx3(const Bytes& d, size_t at, size_t end) {
  end = std::min(end, d.size());
  for (size_t i = at; i + 4 <= end; ++i) {
    if (std::memcmp(d.data() + i, "NTX3", 4) == 0)
      return i;
  }
  return std::nullopt;
}

// Converts every NTX3 nested in an opaque chunk without moving anything. For
// chunks whose layout is not understood: each NTEX has to land exactly where
// its NTX3 was.
std::optional<Bytes> TexturesInPlace(const Bytes& d, size_t o, size_t size, Report& report) {
  std::optional<Bytes> buf;
  const size_t end = o + size;
  size_t at = o + 8;
  while (at < end) {
    const auto found = FindNtx3(d, at, end);
    if (!found)
      break;
    at = *found;
    if (at % 4 == 0 && IsNtx3(d, at, end)) {
      const size_t csize = Rd32(d, at + 4);
      const auto converted = ConvertNtx3(d, at, report);
      if (converted && csize >= converted->size() + 8) {
        if (!buf)
          buf = Slice(d, o, end);
        Bytes replacement = *converted;
        Append(replacement, Npad(csize - converted->size()));
        Splice(*buf, at - o, at - o + csize, replacement);
        at += csize;
        continue;
      }
      if (converted)
        report.Warn("NTX3 at " + Hex(at) + ": no room to convert in place");
    }
    at += 4;
  }
  return buf;
}

std::optional<Bytes> ConvertLeaf(const Bytes& d, size_t o, const std::string& tag, Report& report) {
  if (tag == "NTX3")
    return ConvertNtx3(d, o, report);
  if (tag == "NSHP")
    return ConvertNshp(d, o, report);
  if (kColourChunks.count(tag))
    return ConvertColours(d, o, tag, report);
  if (!kPassthrough.count(tag))
    ++report.counts["unconverted " + tag];
  return std::nullopt;
}

void EmitNobj(Rebuild& rb, const Bytes& d, size_t o, size_t size, Report& report);

void EmitLeaf(Rebuild& rb, const Bytes& d, size_t o, const std::string& tag, size_t size,
              Report& report) {
  rb.Mark(o);
  const auto converted = ConvertLeaf(d, o, tag, report);
  if (!converted) {
    const auto in_place = TexturesInPlace(d, o, size, report);
    Append(rb.out, in_place ? *in_place : Slice(d, o, o + size));
    return;
  }
  Append(rb.out, *converted);
  // An NTX3 is larger than its NTEX: keep the remainder as padding so nothing
  // after it moves. The texture list loop skips NPAD.
  if (tag == "NTX3" && converted->size() < size) {
    const size_t gap = size - converted->size();
    if (gap >= 8)
      Append(rb.out, Npad(gap));
    else
      report.Warn("NTX3 at " + Hex(o) + ": " + std::to_string(gap) +
                  " byte gap cannot hold an NPAD");
  }
}

// Python's bisect.bisect_right, which the anchor lists are searched with.
size_t BisectRight(const std::vector<size_t>& a, size_t x) {
  size_t lo = 0, hi = a.size();
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (x < a[mid])
      hi = mid;
    else
      lo = mid + 1;
  }
  return lo;
}

// A map's placed objects: u32 count, u32 offsets from +8, then NOBJs.
void EmitNlob(Rebuild& rb, const Bytes& d, size_t o, size_t size, Report& report) {
  rb.Mark(o);
  const size_t start = rb.out.size();
  const size_t count = Rd32(d, o + 8);
  std::vector<uint32_t> offsets;
  for (size_t i = 0; i < count; ++i)
    offsets.push_back(Rd32(d, o + 12 + 4 * i));
  const size_t body = o + 12 + 4 * count;
  Append(rb.out, Slice(d, o, body));
  std::map<size_t, size_t> new_pos;
  for (const Child& c : Children(d, body, o + size)) {
    new_pos[c.o - (o + 8)] = rb.out.size() - (start + 8);
    if (c.tag == "NOBJ")
      EmitNobj(rb, d, c.o, c.size, report);
    else
      EmitLeaf(rb, d, c.o, c.tag, c.size, report);
  }
  for (size_t i = 0; i < offsets.size(); ++i) {
    const auto it = new_pos.find(offsets[i]);
    if (it == new_pos.end()) {
      report.Warn("NLOB at " + Hex(o) + ": entry " + std::to_string(i) + " does not start a chunk");
      continue;
    }
    Wr32(rb.out, start + 12 + 4 * i, it->second);
  }
  Wr32(rb.out, start + 4, rb.out.size() - start);
  ++report.counts["NLOB"];
}

std::optional<std::vector<size_t>> MefcEntries(const Bytes& d, size_t o, size_t end);
Bytes ConvertRegionBody(const Bytes& d, size_t start, size_t end, Report& report,
                        const AudioConverter* audio, std::optional<size_t> origin);

// A model's effects: u32 count, u32 offsets from +8, then Mefcs. The offsets
// are rewritten, so each effect may grow with its models.
void EmitNlef(Rebuild& rb, const Bytes& d, size_t o, size_t size, Report& report) {
  rb.Mark(o);
  const size_t start = rb.out.size();
  const size_t end = o + size;
  const size_t count = Rd32(d, o + 8);
  std::vector<uint32_t> offsets;
  for (size_t i = 0; i < count; ++i)
    offsets.push_back(Rd32(d, o + 12 + 4 * i));
  size_t copied = o + 12 + 4 * count;
  Append(rb.out, Slice(d, o, copied));
  std::map<size_t, size_t> new_pos;
  for (uint32_t off : std::set<uint32_t>(offsets.begin(), offsets.end())) {
    const size_t m = o + 8 + off;
    if (m < copied || m + 8 > end) {
      report.Warn("NLEF at " + Hex(o) + ": entry at " + Hex(off) + " overlaps");
      continue;
    }
    const size_t msize = Rd32(d, m + 4);
    Append(rb.out, Slice(d, copied, m));
    new_pos[off] = rb.out.size() - (start + 8);
    if (MefcEntries(d, m, std::min(m + msize, end))) {
      Append(rb.out, ConvertRegionBody(d, m, m + msize, report, nullptr, rb.out.size()));
    } else {
      report.Warn("NLEF at " + Hex(o) + ": entry at " + Hex(off) + " is not an effect");
      const auto in_place = TexturesInPlace(d, m, msize, report);
      Append(rb.out, in_place ? *in_place : Slice(d, m, m + msize));
    }
    copied = m + msize;
  }
  Append(rb.out, Slice(d, copied, end));
  for (size_t i = 0; i < offsets.size(); ++i) {
    const auto it = new_pos.find(offsets[i]);
    if (it != new_pos.end())
      Wr32(rb.out, start + 12 + 4 * i, it->second);
  }
  Wr32(rb.out, start + 4, rb.out.size() - start);
  ++report.counts["NLEF"];
}

void EmitNmdl(Rebuild& rb, const Bytes& d, size_t o, size_t size, Report& report) {
  rb.Mark(o);
  const size_t start = rb.out.size();
  const size_t hdr = NmdlHeader(d, o);
  Append(rb.out, Slice(d, o, o + hdr));
  if (rb.out.at(start + 8) == 0x83)
    rb.out[start + 8] = 0x82;
  ++report.counts["NMDL"];
  const auto children = Children(d, o + hdr, o + size);
  std::vector<uint32_t> nvs;
  for (const Child& c : children) {
    if (c.tag == "NSHP")
      nvs.push_back(Rd16(d, c.o + 0x1A));
  }
  for (const Child& c : children) {
    if (c.tag == "NLOB") {
      EmitNlob(rb, d, c.o, c.size, report);
    } else if (c.tag == "NLEF") {
      EmitNlef(rb, d, c.o, c.size, report);
    } else if (c.tag == "NMR2") {
      rb.Mark(c.o);
      const auto converted = ConvertNmr2(d, c.o, nvs, report);
      if (!converted)
        ++report.counts["unconverted NMR2"];
      Append(rb.out, converted && !converted->empty() ? *converted : Slice(d, c.o, c.o + c.size));
    } else {
      EmitLeaf(rb, d, c.o, c.tag, c.size, report);
    }
  }
  // Keep everything after this model on its PS3 alignment: the RSX layout
  // aligns texture payloads to 128 bytes and some consumers may rely on it.
  const int64_t grown = int64_t(rb.out.size() - start) - int64_t(size);
  const size_t pad = PadTo(grown, 128);
  if (pad)
    Append(rb.out, Npad(pad >= 8 ? pad : pad + 128));
  Wr32(rb.out, start + 4, rb.out.size() - start);
}

void EmitNobj(Rebuild& rb, const Bytes& d, size_t o, size_t size, Report& report) {
  rb.Mark(o);
  const size_t start = rb.out.size();
  Append(rb.out, Slice(d, o, o + 8));
  ++report.counts["NOBJ"];
  for (const Child& c : Children(d, o + 8, o + size)) {
    if (c.tag == "NMDL")
      EmitNmdl(rb, d, c.o, c.size, report);
    else if (c.tag == "NOBJ")
      EmitNobj(rb, d, c.o, c.size, report);
    else
      EmitLeaf(rb, d, c.o, c.tag, c.size, report);
  }
  Wr32(rb.out, start + 4, rb.out.size() - start);
}

// Directory entry positions of an effect, or none. Header u16 +0x0C points at
// "CK", entry size, count; each entry is a tag, a type and a u32 offset from
// the effect start to its section.
std::optional<std::vector<size_t>> MefcEntries(const Bytes& d, size_t o, size_t end) {
  if (!HasTag(d, o, "Mefc") || o + 10 > d.size() || d[o + 8] != 'X' || d[o + 9] != 'B' ||
      o + 0x30 > end)
    return std::nullopt;
  const size_t size = Rd32(d, o + 4);
  const size_t at = o + Rd16(d, o + 12);
  if (o + size > end || at + 2 > d.size() || d[at] != 'C' || d[at + 1] != 'K' ||
      At(d, at + 2) < 12)
    return std::nullopt;
  const size_t entry_size = At(d, at + 2), count = At(d, at + 3);
  std::vector<size_t> entries;
  for (size_t i = 0; i < count; ++i)
    entries.push_back(at + 4 + entry_size * i);
  const size_t first = at + 4 + entry_size * count - o;
  if (entries.empty())
    return std::nullopt;
  for (size_t e : entries) {
    const size_t v = Rd32(d, e + 8);
    if (!(first <= v && v < size))
      return std::nullopt;
  }
  return entries;
}

// Converts every NOBJ, loose NMDL, loose NTX3 and effect in d[start:end], and
// with audio every sound bank, copying the rest. Padding aligns to origin +
// output position, the region start by default.
class Region {
 public:
  Region(const Bytes& d, Report& report, const AudioConverter* audio, size_t origin)
      : d_(d), report_(report), audio_(audio), origin_(origin) {}

  void Run(size_t start, size_t end) {
    Scan(start, end);
    rb_.Mark(end);
    for (const auto& [old, _] : rb_.anchors)
      olds_.push_back(old);
    for (size_t t : tables_) {
      const int64_t at = Remap(t);
      const size_t n = Rd32(d_, t + 4);
      for (size_t i = 0; i < n; ++i)
        Wr32(rb_.out, size_t(at) + 8 + 4 * i, uint32_t(Remap(t + Rd32(d_, t + 8 + 4 * i)) - at));
    }
  }

  // An old absolute offset in the region -> its new offset from the region start.
  int64_t Remap(size_t old) const {
    const size_t i = BisectRight(olds_, old);
    // Python's anchors[-1] when nothing precedes.
    const auto& [a, n] = rb_.anchors[i ? i - 1 : rb_.anchors.size() - 1];
    return int64_t(n) + (int64_t(old) - int64_t(a));
  }

  Bytes& out() { return rb_.out; }

 private:
  void Flush(size_t a, size_t b) {
    rb_.Mark(a);
    Append(rb_.out, Slice(d_, a, b));
  }

  void Pad(size_t n) { rb_.out.resize(rb_.out.size() + PadTo(int64_t(origin_ + rb_.out.size()), n)); }

  // Sections are addressed through the directory, so they may move as long
  // as it is rewritten.
  void EmitMefc(size_t o, size_t size, const std::vector<size_t>& entries) {
    std::set<size_t> cut_set;
    for (size_t e : entries)
      cut_set.insert(Rd32(d_, e + 8));
    std::vector<size_t> cuts(cut_set.begin(), cut_set.end());
    cuts.push_back(size);
    Flush(o, o + cuts[0]);
    const int64_t begin = int64_t(rb_.out.size()) - int64_t(cuts[0]);
    std::map<size_t, int64_t> moved;
    for (size_t k = 0; k + 1 < cuts.size(); ++k) {
      const size_t a = cuts[k], b = cuts[k + 1];
      Pad(128);
      const size_t first = rb_.anchors.size();
      Scan(o + a, o + b);
      std::optional<size_t> best;
      for (size_t j = first; j < rb_.anchors.size(); ++j) {
        if (rb_.anchors[j].first == o + a)
          best = std::max(best.value_or(0), rb_.anchors[j].second);
      }
      if (!best)
        throw std::runtime_error("Mefc at " + Hex(o) + ": a section left no anchor");
      moved[a] = int64_t(*best) - begin;
    }
    for (size_t e : entries)
      Wr32(rb_.out, size_t(begin + int64_t(e - o) + 8), uint32_t(moved[Rd32(d_, e + 8)]));
    Wr32(rb_.out, size_t(begin) + 4, uint32_t(int64_t(rb_.out.size()) - begin));
    ++report_.counts["Mefc"];
  }

  void Scan(size_t o, size_t end) {
    size_t copied = o;
    while (o + 8 <= end) {
      const std::string tag = Tag(d_, o);
      if (audio_ && IsCsl(d_, o, end)) {
        // A bank directory: offsets from itself to the banks after it.
        Flush(copied, o);
        Pad(0x1000);
        rb_.Mark(o);
        tables_.push_back(o);
        const auto offs = CslBanks(d_, o);
        groups_.emplace_back();
        auto& group = groups_.back();
        for (size_t b : offs)
          group.push_back(Slice(d_, b, b + Rd32(d_, b + 4)));
        for (size_t i = 0; i < offs.size(); ++i)
          members_[offs[i]] = {o, i, &group};
        copied = o;
        o += 8 + 4 * size_t(Rd32(d_, o + 4));
        continue;
      }
      if (audio_ && IsCsf(d_, o, end)) {
        const size_t size = Rd32(d_, o + 4);
        Flush(copied, o);
        // Banks start on a 0x1000 boundary of the file, as on the 360, since
        // their clip payloads are aligned from the bank start.
        Pad(0x1000);
        rb_.Mark(o);
        const auto member = members_.find(o);
        Append(rb_.out, (*audio_)(banks_, Slice(d_, o, o + size),
                                  member == members_.end() ? nullptr : &member->second));
        ++banks_;
        o = copied = o + size;
        continue;
      }
      if (tag == "NOBJ" && IsNobj(d_, o) && o + Rd32(d_, o + 4) <= end) {
        const size_t size = Rd32(d_, o + 4);
        Flush(copied, o);
        EmitNobj(rb_, d_, o, size, report_);
        o = copied = o + size;
        continue;
      }
      // Events also hand bare models to native 1062.
      if (tag == "NMDL" && IsNmdl(d_, o, end)) {
        const size_t size = Rd32(d_, o + 4);
        Flush(copied, o);
        EmitNmdl(rb_, d_, o, size, report_);
        o = copied = o + size;
        continue;
      }
      if (tag == "NTX3" && IsNtx3(d_, o, end)) {
        const size_t size = Rd32(d_, o + 4);
        Flush(copied, o);
        EmitLeaf(rb_, d_, o, tag, size, report_);
        o = copied = o + size;
        continue;
      }
      if (tag == "Mefc") {
        if (const auto entries = MefcEntries(d_, o, end)) {
          const size_t size = Rd32(d_, o + 4);
          Flush(copied, o);
          EmitMefc(o, size, *entries);
          o = copied = o + size;
          continue;
        }
      }
      o += 4;
    }
    Flush(copied, end);
  }

  const Bytes& d_;
  Report& report_;
  const AudioConverter* audio_;
  size_t origin_;
  Rebuild rb_;
  size_t banks_ = 0;
  std::vector<size_t> tables_;
  std::vector<size_t> olds_;
  // Stable addresses: members point into these.
  std::list<std::vector<Bytes>> groups_;
  std::map<size_t, BankMember> members_;
};

Bytes ConvertRegionBody(const Bytes& d, size_t start, size_t end, Report& report,
                        const AudioConverter* audio, std::optional<size_t> origin) {
  Region region(d, report, audio, origin.value_or(start));
  region.Run(start, end);
  return std::move(region.out());
}

// ---------------------------------------------------------------------------
// Containers
// ---------------------------------------------------------------------------
Bytes ConvertE(const Bytes& d, Report& report, const AudioConverter* audio) {
  uint32_t hdr[6];
  for (int i = 0; i < 6; ++i)
    hdr[i] = Rd32(d, 4 * i);
  const size_t image_end = 0x18 + size_t(hdr[4]);
  const size_t reloc_base = image_end + hdr[5];
  Region region(d, report, audio, image_end);
  region.Run(image_end, reloc_base);
  const Bytes& bulk = region.out();
  Bytes image = Slice(d, 0, image_end);
  // List B: image dwords holding bulk relative offsets.
  const size_t na = Rd32(d, reloc_base);
  const size_t nb_at = reloc_base + 4 + 4 * na;
  const size_t nb = Rd32(d, nb_at);
  for (size_t i = 0; i < nb; ++i) {
    const size_t entry = Rd32(d, nb_at + 4 + 4 * i);
    Wr32(image, entry, uint32_t(region.Remap(image_end + Rd32(image, entry))));
  }
  const int64_t delta = int64_t(bulk.size()) - int64_t(reloc_base - image_end);
  Wr32(image, 0x0C, uint32_t(int64_t(hdr[3]) + delta));
  Wr32(image, 0x14, uint32_t(int64_t(hdr[5]) + delta));
  Bytes out = std::move(image);
  Append(out, bulk);
  Append(out, Slice(d, reloc_base, d.size()));
  return out;
}

Bytes ConvertBmd(const Bytes& d, Report& report, const AudioConverter* audio) {
  const size_t count = Rd32(d, 8);
  // title.bmd has one entry past its count, which the title screen reads, so
  // the table runs up to the first entry.
  size_t table_end = 12 + 4 * count;
  size_t first = d.size();
  for (size_t o = 12; o < table_end; o += 4) {
    const size_t v = Rd32(d, o);
    if (v >= table_end)
      first = std::min(first, v);
  }
  table_end = first & ~size_t(3);
  Region region(d, report, audio, table_end);
  region.Run(table_end, d.size());
  const Bytes& body = region.out();
  Bytes head = Slice(d, 0, table_end);
  for (size_t at = 12; at < table_end; at += 4) {
    const size_t v = Rd32(head, at);
    if (table_end <= v && v < d.size())
      Wr32(head, at, uint32_t(int64_t(table_end) + region.Remap(v)));
  }
  Wr32(head, 4, head.size() + body.size());
  Append(head, body);
  return head;
}

Bytes ConvertBop(const Bytes& d, Report& report, const AudioConverter* audio) {
  const size_t dir_at = Rd32(d, 12);
  const size_t count = Rd32(d, dir_at);
  const size_t head_end = dir_at + 4 + 4 * count;
  Region region(d, report, audio, head_end);
  region.Run(head_end, d.size());
  const Bytes& body = region.out();
  Bytes head = Slice(d, 0, head_end);
  for (size_t i = 0; i < count; ++i) {
    const size_t at = dir_at + 4 + 4 * i;
    const size_t v = Rd32(head, at);
    if (head_end <= v && v < d.size())
      Wr32(head, at, uint32_t(int64_t(head_end) + region.Remap(v)));
  }
  Wr32(head, 4, head.size() + body.size());
  Append(head, body);
  return head;
}

// The camp menu loads scp.bmd whole into a buffer of this size (sub_8222BDE8).
constexpr size_t kScpBuffer = 0x12A0000;

// Score piece strips: "SCP ", total size, offset of the bank directory,
// texture count, then one NTX3 per piece. Laid out as the 360's: textures 16
// byte aligned, banks from the next 0x1000.
Bytes ConvertScp(const Bytes& d, Report& report, const AudioConverter* audio) {
  const size_t banks_at = Rd32(d, 8);
  const size_t count = Rd32(d, 12);
  Bytes out = Slice(d, 0, 16 + 4 * count);
  for (size_t i = 0; i < count; ++i) {
    const size_t o = Rd32(d, 16 + 4 * i);
    auto tex = ConvertNtx3(d, o, report);
    if (!tex || tex->empty())
      tex = Slice(d, o, o + Rd32(d, o + 4));
    out.resize(out.size() + PadTo(int64_t(out.size()), 16));
    Wr32(out, 16 + 4 * i, out.size());
    Append(out, *tex);
  }
  out.resize(out.size() + PadTo(int64_t(out.size()), 0x1000));
  Wr32(out, 8, out.size());
  Append(out, ConvertRegionBody(d, banks_at, d.size(), report, audio, std::nullopt));
  out.resize(out.size() + PadTo(int64_t(out.size()), 0x1000));
  Wr32(out, 4, out.size());
  if (out.size() > kScpBuffer)
    report.Warn("scp.bmd: " + Hex(out.size()) + " bytes overflows its " + Hex(kScpBuffer) +
                " byte buffer");
  return out;
}

// BattleKeep.bop is addressed by slot. The PS3 dropped the effects at 360
// slots 26..33 and moved every later slot down by eight, but its battle files
// still name those eight; the game reorders the slots into the 360's layout
// (ps3_battlekeep.cpp) and finds them appended after the PS3's own.
constexpr size_t kBattleKeepDroppedFirst = 26;
constexpr size_t kBattleKeepDropped = 8;

std::pair<size_t, std::vector<std::optional<Bytes>>> BopEntries(const Bytes& d) {
  const size_t dir_at = Rd32(d, 12);
  const size_t n = Rd32(d, dir_at);
  std::vector<size_t> offsets;
  for (size_t i = 0; i < n; ++i)
    offsets.push_back(Rd32(d, dir_at + 4 + 4 * i));
  std::set<size_t> end_set(offsets.begin(), offsets.end());
  end_set.erase(0);
  end_set.insert(d.size());
  const std::vector<size_t> ends(end_set.begin(), end_set.end());
  std::vector<std::optional<Bytes>> entries;
  for (size_t o : offsets) {
    if (!o) {
      entries.emplace_back();
      continue;
    }
    const size_t i = BisectRight(ends, o);
    if (i >= ends.size())
      throw std::runtime_error("BattleKeep entry past the end of the file");
    entries.emplace_back(Slice(d, o, ends[i]));
  }
  return {dir_at, std::move(entries)};
}

Bytes ConvertBare(const Bytes& d, Report& report) {
  return ConvertRegionBody(d, 0, d.size(), report, nullptr, std::nullopt);
}

bool EndsWith(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

}  // namespace

const char* const kBattleKeep = "btldata/battlekeep.bop";

Bytes AppendDroppedBattleKeep(const Bytes& ps3, const Bytes& x360, Report& report) {
  auto [dir_at, slots] = BopEntries(ps3);
  auto [_, want] = BopEntries(x360);
  if (slots.size() + kBattleKeepDropped != want.size()) {
    report.Warn(std::string(kBattleKeep) + ": " + std::to_string(slots.size()) +
                " entries, expected " + std::to_string(want.size() - 8) + "; kept as is");
    return ps3;
  }
  for (size_t i = 0; i < kBattleKeepDropped; ++i)
    slots.push_back(want[kBattleKeepDroppedFirst + i]);
  Bytes out = Slice(ps3, 0, dir_at);
  Put32(out, slots.size());
  out.resize(out.size() + 4 * slots.size());
  for (size_t i = 0; i < slots.size(); ++i) {
    if (!slots[i])
      continue;
    out.resize(out.size() + PadTo(int64_t(out.size()), 0x80));
    Wr32(out, dir_at + 4 + 4 * i, out.size());
    Append(out, *slots[i]);
  }
  out.resize(out.size() + PadTo(int64_t(out.size()), 0x1000));
  Wr32(out, 4, out.size());
  report.counts["BattleKeep effects appended from the 360"] += kBattleKeepDropped;
  return out;
}

bool IsCsf(const Bytes& d, size_t o, size_t end) {
  if (!HasTag(d, o, "CSF ") || o + 16 > end)
    return false;
  const uint64_t total = Rd32(d, o + 4), head = Rd32(d, o + 8), payload = Rd32(d, o + 12);
  return head && total == head + payload && o + total <= end;
}

std::vector<size_t> CslBanks(const Bytes& d, size_t o) {
  std::vector<size_t> out;
  const size_t n = Rd32(d, o + 4);
  for (size_t i = 0; i < n; ++i)
    out.push_back(o + Rd32(d, o + 8 + 4 * i));
  return out;
}

bool IsCsl(const Bytes& d, size_t o, size_t end) {
  if (!HasTag(d, o, "CSL ") || o + 8 > end)
    return false;
  const size_t n = Rd32(d, o + 4);
  if (n == 0 || n > 256)
    return false;
  for (size_t i = 0; i < n; ++i) {
    if (!IsCsf(d, o + Rd32(d, o + 8 + 4 * i), end))
      return false;
  }
  return true;
}

std::optional<Output> ConvertFile(const std::string& rel, const Bytes& d, Report& report,
                                  const AudioConverter* audio) {
  std::string low = rel;
  std::transform(low.begin(), low.end(), low.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  // Formats nothing converts yet: the 360 file is kept for these.
  if (EndsWith(low, ".fnt") || EndsWith(low, ".tex"))
    return std::nullopt;
  // Credits: same records on both releases, read through the same three of
  // its ten lists (sub_82131E18, PS3 0x32287C), and nothing to convert.
  if (low == "op.bmd" || low == "ed1.bmd" || low == "ed2.bmd")
    return Output{rel, d};
  // Battle effects and title.bmd carry banks too: one ATRAC3 clip left in
  // them stalls the XMA decoder and silences all audio after it.
  if (EndsWith(low, ".e") && d.size() >= 4 && d[0] == 0 && d[1] == 0 && d[2] == 1 &&
      (d[3] == 0x81 || d[3] == 0x80))
    return Output{rel, ConvertE(d, report, audio)};
  if (HasTag(d, 0, "BMD ") || HasTag(d, 0, "CAMP"))
    return Output{rel, ConvertBmd(d, report, audio)};
  if (HasTag(d, 0, "SCP "))
    return Output{rel, ConvertScp(d, report, audio)};
  if (HasTag(d, 0, "BOP "))
    return Output{rel, ConvertBop(d, report, audio)};
  if (EndsWith(low, ".p3tex"))
    return Output{rel.substr(0, rel.size() - 6) + ".x3tex", ConvertBare(d, report)};
  if (EndsWith(low, ".p3obj"))
    return Output{rel, ConvertBare(d, report)};
  return std::nullopt;
}

}  // namespace eternalsonata::ps3
