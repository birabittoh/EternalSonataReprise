// eternalsonata - ReXGlue Recompiled Project
//
// See eternalsonata_asset_container.h.

#include "eternalsonata_asset_container.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#include <rex/logging.h>

namespace eternalsonata::assets {
namespace {

uint32_t ReadBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void WriteBE32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

bool ReadBE32At(const std::vector<uint8_t>& d, size_t off, uint32_t* out) {
  if (off + 4 > d.size())
    return false;
  *out = ReadBE32(d.data() + off);
  return true;
}

void AppendBE32(std::string& s, uint32_t v) {
  s.push_back(char(v >> 24));
  s.push_back(char(v >> 16));
  s.push_back(char(v >> 8));
  s.push_back(char(v));
}

// ---------------------------------------------------------------------------
// Codecs (port of scripts/unpack_e.c)
// ---------------------------------------------------------------------------
class Source {
 public:
  bool Init(const uint8_t* data, size_t size, bool coded) {
    data_ = data;
    size_ = size;
    coded_ = coded;
    if (!coded_)
      return true;
    if (size_ < 260)
      return false;
    std::memcpy(freq_, data_, 256);
    pos_ = 256;
    cum_[0] = 0;
    for (int i = 0; i < 256; ++i)
      cum_[i + 1] = uint16_t(cum_[i] + freq_[i]);
    total_ = cum_[256];
    if (!total_)
      return false;
    lut_.resize(total_);
    for (uint32_t v = 0, sym = 0; sym < 256; ++sym)
      while (v < cum_[sym + 1])
        lut_[v++] = uint8_t(sym);
    low_ = 0;
    range_ = 0xFFFFFFFFu;
    code_ = 0;
    for (int i = 0; i < 4; ++i)
      code_ = (code_ << 8) | uint32_t(Byte());
    return true;
  }

  int Get() {
    if (!coded_)
      return Byte();
    int b;
    while (((low_ + range_) ^ low_) < 0x1000000u) {
      if ((b = Byte()) < 0)
        return -1;
      low_ <<= 8;
      range_ <<= 8;
      code_ = (code_ << 8) | uint32_t(b);
    }
    while (range_ < 0x2000u) {
      const uint32_t old = low_;
      if ((b = Byte()) < 0)
        return -1;
      low_ = old << 8;
      range_ = (0u - (old << 8)) & 0x1FFF00u;
      code_ = (code_ << 8) | uint32_t(b);
    }
    const uint32_t r = range_ / total_;
    const uint32_t slot = (code_ - low_) / r;
    if (slot >= total_)
      return -1;
    const uint8_t sym = lut_[slot];
    low_ += uint32_t(cum_[sym]) * r;
    range_ = uint32_t(freq_[sym]) * r;
    return sym;
  }

 private:
  int Byte() { return pos_ < size_ ? data_[pos_++] : -1; }

  const uint8_t* data_ = nullptr;
  size_t size_ = 0, pos_ = 0;
  bool coded_ = false;
  uint32_t low_ = 0, range_ = 0, code_ = 0, total_ = 0;
  uint8_t freq_[256] = {};
  uint16_t cum_[257] = {};
  std::vector<uint8_t> lut_;
};

}  // namespace

const char* const kBtxLanguages[7] = {"JPN ", "USA ", "GBR ", "FRA ", "ITA ", "DEU ", "ESP "};

std::string NormalizeGuestPath(std::string_view path) {
  std::string out;
  out.reserve(path.size());
  for (char c : path) {
    if (c == '\\')
      c = '/';
    out.push_back(char(std::tolower(static_cast<unsigned char>(c))));
  }
  // "game:/foo" and a leading slash both name the same asset as "foo".
  if (out.rfind("game:/", 0) == 0)
    out.erase(0, 6);
  while (!out.empty() && out.front() == '/')
    out.erase(0, 1);
  return out;
}

bool DecodeAsset(const uint8_t* data, size_t size, uint32_t decoded_size, uint8_t flag,
                 std::vector<uint8_t>& out) {
  out.assign(decoded_size, 0);
  Source src;
  if (!src.Init(data, size, (flag & 2) != 0))
    return false;

  size_t o = 0;
  if (!(flag & 1)) {
    while (o < out.size()) {
      const int v = src.Get();
      if (v < 0)
        break;
      out[o++] = uint8_t(v);
    }
  } else {
    uint8_t ring[4096] = {};
    unsigned pos = 4078, mask = 0;
    int state = 0, flagbyte = 0, matchlo = 0;
    while (o < out.size()) {
      const int v = src.Get();
      if (v < 0)
        break;
      if (state == 0) {
        flagbyte = v;
        mask = 1;
        state = (v & 1) ? 1 : 2;
        continue;
      }
      if (state == 1) {
        out[o++] = uint8_t(v);
        ring[pos] = uint8_t(v);
        pos = (pos + 1) & 0xFFF;
      } else if (state == 2) {
        matchlo = v;
        state = 3;
        continue;
      } else {
        const int len = (v & 0xF) + 3;
        unsigned off = unsigned(((v << 4) & 0xF00) | matchlo) & 0xFFF;
        for (int i = 0; i < len && o < out.size(); ++i) {
          const uint8_t c = ring[off];
          off = (off + 1) & 0xFFF;
          out[o++] = c;
          ring[pos] = c;
          pos = (pos + 1) & 0xFFF;
        }
      }
      mask = (mask << 1) & 0xFF;
      state = mask ? ((flagbyte & mask) ? 1 : 2) : 0;
    }
  }
  out.resize(o);
  return o == decoded_size;
}

// ---------------------------------------------------------------------------
// index.vmtoc
// ---------------------------------------------------------------------------
bool Toc::Load(const std::filesystem::path& file) {
  raw_.clear();
  entries_.clear();
  index_.clear();

  std::ifstream in(file, std::ios::binary);
  if (!in)
    return false;
  raw_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  if (raw_.empty() || raw_.size() % 48 != 0) {
    raw_.clear();
    return false;
  }

  const size_t count = raw_.size() / 48;
  entries_.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const uint8_t* rec = raw_.data() + i * 48;
    const size_t len = strnlen(reinterpret_cast<const char*>(rec), 32);
    TocEntry e;
    e.path = NormalizeGuestPath(std::string_view(reinterpret_cast<const char*>(rec), len));
    e.size = ReadBE32(rec + 32);
    e.flag = rec[36];
    if (!e.path.empty())
      index_.emplace(e.path, i);
    entries_.push_back(std::move(e));
  }
  return true;
}

const TocEntry* Toc::Find(std::string_view guest_path) const {
  auto it = index_.find(NormalizeGuestPath(guest_path));
  return it == index_.end() ? nullptr : &entries_[it->second];
}

bool Toc::SetStored(std::string_view guest_path, uint32_t decoded_size) {
  auto it = index_.find(NormalizeGuestPath(guest_path));
  if (it == index_.end())
    return false;
  uint8_t* rec = raw_.data() + it->second * 48;
  WriteBE32(rec + 32, decoded_size);
  rec[36] = 0;
  entries_[it->second].size = decoded_size;
  entries_[it->second].flag = 0;
  return true;
}

// ---------------------------------------------------------------------------
// BTX
// ---------------------------------------------------------------------------
const BtxLang* BtxBlob::Find(std::string_view fourcc) const {
  for (const auto& l : langs)
    if (l.fourcc == fourcc)
      return &l;
  return nullptr;
}

namespace {

bool IsKnownLanguage(const std::string& fourcc) {
  for (const char* l : kBtxLanguages)
    if (fourcc == l)
      return true;
  return false;
}

// One blob at `base`. Mirrors scripts/btx.py's parse_btx, including its
// validation: a bad parse is how a false positive from the magic scan is
// rejected.
bool ParseBtx(const std::vector<uint8_t>& d, size_t base, BtxBlob* out) {
  uint32_t first = 0, size = 0, nlangs = 0;
  if (!ReadBE32At(d, base + 4, &first) || !ReadBE32At(d, base + 8, &size) ||
      !ReadBE32At(d, base + 0x0C, &nlangs))
    return false;
  if (nlangs == 0 || nlangs > 16)
    return false;
  if (size == 0 || size > d.size() - base)
    return false;

  out->offset = base;
  out->size = size;
  out->langs.clear();

  const size_t end = base + size;
  size_t q = base + first;
  for (uint32_t i = 0; i < nlangs; ++i) {
    if (q + 0x14 > d.size())
      return false;
    BtxLang lang;
    lang.fourcc.assign(reinterpret_cast<const char*>(d.data() + q), 4);
    if (!IsKnownLanguage(lang.fourcc))
      return false;
    uint32_t etab = 0, next = 0, count = 0;
    if (!ReadBE32At(d, q + 4, &etab) || !ReadBE32At(d, q + 8, &next) ||
        !ReadBE32At(d, q + 0x10, &count) || !ReadBE32At(d, q + 0x0C, &lang.f12))
      return false;
    if (count > 0x10000)
      return false;

    lang.sub_offset = q;
    // The chain may or may not self-terminate, so the last block's length comes
    // from the blob size rather than from `next`. docs/asset-formats.md 3.4.
    lang.sub_length = (i + 1 < nlangs) ? next : (end - q);

    for (uint32_t k = 0; k < count; ++k) {
      const size_t e = q + etab + 8 * k;
      uint32_t sid = 0, soff = 0;
      if (!ReadBE32At(d, e, &sid) || !ReadBE32At(d, e + 4, &soff))
        return false;
      const size_t start = q + soff;
      if (start < base || start >= end)
        return false;
      size_t stop = start;
      while (stop < end && d[stop] != 0)
        ++stop;
      if (stop >= end)
        return false;
      lang.entries.emplace(sid, std::string(reinterpret_cast<const char*>(d.data() + start),
                                            stop - start));
    }
    if (i == 0)
      out->f12 = lang.f12;
    if (i + 1 == nlangs)
      out->terminated = next == 0;
    out->langs.push_back(std::move(lang));
    q += next;
  }
  return true;
}

// Serialise one sub-block into exactly `size` bytes. Identical strings share a
// copy so a longer string can be paid for out of the slack that creates; entry
// offsets are arbitrary, so the reader cannot tell. Returns false when even
// that does not free enough room.
bool BuildSubBlock(const BtxLang& lang, const std::map<uint32_t, std::string>& entries,
                   uint32_t next, size_t size, std::string* out) {
  const uint32_t etab_off = 0x14;
  const size_t data_off = etab_off + 8 * entries.size();
  std::string table, blob;
  std::map<std::string, uint32_t> seen;
  for (const auto& [sid, value] : entries) {
    auto it = seen.find(value);
    uint32_t off;
    if (it != seen.end()) {
      off = it->second;
    } else {
      off = uint32_t(data_off + blob.size());
      seen.emplace(value, off);
      blob += value;
      blob.push_back('\0');
    }
    AppendBE32(table, sid);
    AppendBE32(table, off);
  }
  if (data_off + blob.size() > size)
    return false;

  out->assign(lang.fourcc);
  AppendBE32(*out, etab_off);
  AppendBE32(*out, next);
  AppendBE32(*out, lang.f12);
  AppendBE32(*out, uint32_t(entries.size()));
  *out += table;
  *out += blob;
  out->append(size - out->size(), '\0');
  return true;
}

// Whole blob, packed with no slack: what a resizing edit needs. Byte-exact
// against the shipped blobs when no edit is applied (scripts/btx_edit.py's
// build_btx, verified over all 758).
std::string BuildBlob(const BtxBlob& blob,
                      const std::vector<std::map<uint32_t, std::string>>& entries) {
  std::vector<std::string> blocks;
  blocks.reserve(blob.langs.size());
  for (size_t i = 0; i < blob.langs.size(); ++i) {
    const BtxLang& lang = blob.langs[i];
    const uint32_t etab_off = 0x14;
    const size_t data_off = etab_off + 8 * entries[i].size();
    std::string table, data;
    for (const auto& [sid, value] : entries[i]) {
      AppendBE32(table, sid);
      AppendBE32(table, uint32_t(data_off + data.size()));
      data += value;
      data.push_back('\0');
    }
    std::string block(lang.fourcc);
    AppendBE32(block, etab_off);
    AppendBE32(block, 0);  // next, patched below once every length is known
    AppendBE32(block, lang.f12);
    AppendBE32(block, uint32_t(entries[i].size()));
    block += table;
    block += data;
    blocks.push_back(std::move(block));
  }

  std::string out("BTX ");
  AppendBE32(out, 0x10);
  AppendBE32(out, 0);  // blob size, patched below
  AppendBE32(out, uint32_t(blocks.size()));
  for (size_t i = 0; i < blocks.size(); ++i) {
    const bool last = i + 1 == blocks.size();
    const uint32_t next = (last && blob.terminated) ? 0 : uint32_t(blocks[i].size());
    WriteBE32(reinterpret_cast<uint8_t*>(blocks[i].data()) + 8, next);
    out += blocks[i];
  }
  WriteBE32(reinterpret_cast<uint8_t*>(out.data()) + 8, uint32_t(out.size()));
  return out;
}

// Adjust the list-B raw dwords that point past the edited blob.
//
// The loader does `*(image + off) += bulk_base` for every list B entry, and 47
// of t0001.e's 64 entries point into the debug string pool that follows the
// BTX blob. Growing the blob shifts that data, so the stored offsets have to
// move with it or the script VM dereferences garbage.
// docs/asset-formats.md 3.4.2 has the post-mortem.
void FixRelocations(std::vector<uint8_t>& out, size_t blob_base, uint32_t old_blob_size,
                    int64_t delta) {
  uint32_t magic = 0;
  if (!ReadBE32At(out, 0, &magic) || (magic != 0x180 && magic != 0x181))
    return;

  uint32_t image_size = 0, reloc_off = 0;
  if (!ReadBE32At(out, 0x10, &image_size) || !ReadBE32At(out, 0x14, &reloc_off))
    return;
  const size_t image_end = 0x18 + image_size;
  // The tables have already moved with the splice; the header field still
  // holds the pre-splice value, which the caller updates after us.
  const size_t reloc_base = size_t(int64_t(image_end + reloc_off) + delta);

  uint32_t count_a = 0;
  if (!ReadBE32At(out, reloc_base, &count_a))
    return;
  const size_t list_b = reloc_base + 4 + 4 * size_t(count_a);
  uint32_t count_b = 0;
  if (!ReadBE32At(out, list_b, &count_b))
    return;

  // The image allocation starts at file offset 0 (the memcpy includes the
  // 24-byte header), so a table entry IS a file offset and the dword it points
  // at holds a bulk offset.
  const int64_t threshold = int64_t(blob_base + old_blob_size) - int64_t(image_end);
  size_t patched = 0;
  for (uint32_t i = 0; i < count_b; ++i) {
    uint32_t entry = 0;
    if (!ReadBE32At(out, list_b + 4 + 4 * size_t(i), &entry))
      return;
    uint32_t raw = 0;
    if (!ReadBE32At(out, entry, &raw))
      continue;
    if (int64_t(raw) >= threshold) {
      WriteBE32(out.data() + entry, uint32_t(int64_t(raw) + delta));
      ++patched;
    }
  }
  if (patched) {
    REXLOG_DEBUG("assets: adjusted {} list B relocations by {} (post-BTX data shift)", patched,
                 delta);
  }
}

void ShiftHeader(std::vector<uint8_t>& data, int64_t delta) {
  uint32_t total = 0, reloc_off = 0;
  if (ReadBE32At(data, 0x0C, &total))
    WriteBE32(data.data() + 0x0C, uint32_t(int64_t(total) + delta));
  if (ReadBE32At(data, 0x14, &reloc_off))
    WriteBE32(data.data() + 0x14, uint32_t(int64_t(reloc_off) + delta));
}

}  // namespace

std::vector<BtxBlob> FindBtxBlobs(const std::vector<uint8_t>& data) {
  std::vector<BtxBlob> found;
  static const uint8_t kMagic[4] = {'B', 'T', 'X', ' '};
  if (data.size() < 4)
    return found;
  for (size_t i = 0; i + 4 <= data.size();) {
    auto it = std::search(data.begin() + ptrdiff_t(i), data.end(), kMagic, kMagic + 4);
    if (it == data.end())
      break;
    const size_t at = size_t(it - data.begin());
    BtxBlob blob;
    if (ParseBtx(data, at, &blob))
      found.push_back(std::move(blob));
    i = at + 4;
  }
  return found;
}

bool ApplyTextEdits(std::vector<uint8_t>& data, std::vector<TextEdit>& edits) {
  auto blobs = FindBtxBlobs(data);
  if (blobs.empty()) {
    for (auto& e : edits)
      e.status = EditStatus::kNotFound;
    return false;
  }

  bool modified = false;
  // Blobs come back in ascending file order, and each splice shifts everything
  // after it, so walking them in that order keeps the offsets of the ones we
  // have not reached yet valid.
  for (size_t bi = 0; bi < blobs.size(); ++bi) {
    std::vector<TextEdit*> mine;
    for (auto& e : edits)
      if (e.blob == bi)
        mine.push_back(&e);
    if (mine.empty())
      continue;

    const BtxBlob& blob = blobs[bi];
    std::vector<std::map<uint32_t, std::string>> entries;
    entries.reserve(blob.langs.size());
    for (const auto& l : blob.langs)
      entries.push_back(l.entries);

    // Which language indices an edit touches, and whether it wants a resize.
    std::vector<std::vector<TextEdit*>> per_lang(blob.langs.size());
    bool any_applied = false;
    for (TextEdit* e : mine) {
      bool hit = false;
      for (size_t li = 0; li < blob.langs.size(); ++li) {
        if (!e->lang.empty() && blob.langs[li].fourcc != e->lang)
          continue;
        auto it = entries[li].find(e->id);
        if (it == entries[li].end())
          continue;
        it->second = e->value;
        per_lang[li].push_back(e);
        hit = true;
      }
      e->status = hit ? EditStatus::kOk : EditStatus::kNotFound;
      any_applied |= hit;
    }
    if (!any_applied)
      continue;

    // Preserve-size first: rebuild only the touched sub-blocks at exactly their
    // original length, so nothing outside them moves.
    bool need_resize = false;
    std::vector<std::pair<size_t, std::string>> preserved;  // (offset, bytes)
    for (size_t li = 0; li < blob.langs.size() && !need_resize; ++li) {
      if (per_lang[li].empty())
        continue;
      const BtxLang& lang = blob.langs[li];
      uint32_t next = 0;
      ReadBE32At(data, lang.sub_offset + 8, &next);
      std::string built;
      if (!BuildSubBlock(lang, entries[li], next, lang.sub_length, &built)) {
        // The dedup pool is shared between mods, so attribute the overflow to
        // the specific patches rather than letting one corrupt the block.
        if (std::any_of(per_lang[li].begin(), per_lang[li].end(),
                        [](const TextEdit* e) { return e->allow_resize; })) {
          need_resize = true;
        } else {
          for (TextEdit* e : per_lang[li]) {
            e->status = EditStatus::kTooLarge;
            entries[li][e->id] = lang.entries.at(e->id);
          }
          per_lang[li].clear();
        }
        continue;
      }
      if (!need_resize && !per_lang[li].empty())
        preserved.emplace_back(lang.sub_offset, std::move(built));
    }

    if (!need_resize) {
      for (auto& [off, bytes] : preserved) {
        std::memcpy(data.data() + off, bytes.data(), bytes.size());
        modified = true;
      }
      continue;
    }

    const std::string rebuilt = BuildBlob(blob, entries);
    const int64_t delta = int64_t(rebuilt.size()) - int64_t(blob.size);
    std::vector<uint8_t> out;
    out.reserve(data.size() + size_t(std::max<int64_t>(delta, 0)));
    out.insert(out.end(), data.begin(), data.begin() + ptrdiff_t(blob.offset));
    out.insert(out.end(), rebuilt.begin(), rebuilt.end());
    out.insert(out.end(), data.begin() + ptrdiff_t(blob.offset + blob.size), data.end());
    data.swap(out);
    if (delta) {
      FixRelocations(data, blob.offset, blob.size, delta);
      ShiftHeader(data, delta);
      // Everything after this blob has moved, so re-read the ones we have not
      // handled yet rather than trusting stale offsets.
      auto refreshed = FindBtxBlobs(data);
      if (refreshed.size() == blobs.size())
        blobs = std::move(refreshed);
    }
    modified = true;
  }
  return modified;
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------
namespace {

// The cp1252 codepoints that are not their own byte value. Everything else in
// 0x20..0xFF is Latin-1, i.e. codepoint == byte.
struct Cp1252Special {
  uint32_t cp;
  uint8_t byte;
};
constexpr Cp1252Special kCp1252[] = {
    {0x20AC, 0x80}, {0x201A, 0x82}, {0x0192, 0x83}, {0x201E, 0x84}, {0x2026, 0x85},
    {0x2020, 0x86}, {0x2021, 0x87}, {0x02C6, 0x88}, {0x2030, 0x89}, {0x0160, 0x8A},
    {0x2039, 0x8B}, {0x0152, 0x8C}, {0x017D, 0x8E}, {0x2018, 0x91}, {0x2019, 0x92},
    {0x201C, 0x93}, {0x201D, 0x94}, {0x2022, 0x95}, {0x2013, 0x96}, {0x2014, 0x97},
    {0x02DC, 0x98}, {0x2122, 0x99}, {0x0161, 0x9A}, {0x203A, 0x9B}, {0x0153, 0x9C},
    {0x017E, 0x9E}, {0x0178, 0x9F},
};

bool NextCodepoint(std::string_view s, size_t& i, uint32_t* out) {
  const uint8_t c = uint8_t(s[i]);
  size_t extra;
  uint32_t cp;
  if (c < 0x80) {
    *out = c;
    ++i;
    return true;
  } else if ((c & 0xE0) == 0xC0) {
    extra = 1;
    cp = c & 0x1Fu;
  } else if ((c & 0xF0) == 0xE0) {
    extra = 2;
    cp = c & 0x0Fu;
  } else if ((c & 0xF8) == 0xF0) {
    extra = 3;
    cp = c & 0x07u;
  } else {
    return false;
  }
  if (i + extra >= s.size())
    return false;
  for (size_t k = 1; k <= extra; ++k) {
    const uint8_t b = uint8_t(s[i + k]);
    if ((b & 0xC0) != 0x80)
      return false;
    cp = (cp << 6) | (b & 0x3Fu);
  }
  i += extra + 1;
  *out = cp;
  return true;
}

}  // namespace

bool TranscodeToGameEncoding(std::string_view utf8, std::string_view lang, std::string& out,
                             std::string* error) {
  out.clear();
  // JPN is Shift-JIS. Nothing here knows how to produce it, so the bytes are
  // passed through and the mod is responsible for shipping them already encoded.
  if (lang.rfind("JPN", 0) == 0) {
    out.assign(utf8);
    return true;
  }

  for (size_t i = 0; i < utf8.size();) {
    uint32_t cp = 0;
    const size_t at = i;
    if (!NextCodepoint(utf8, i, &cp)) {
      if (error)
        *error = "malformed UTF-8 at byte " + std::to_string(at);
      return false;
    }
    if (cp == '\r')
      continue;
    if (cp == '\n') {
      // A newline in the game's text is the two characters '\' 'n'.
      out += "\\n";
      continue;
    }
    if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF)) {
      out.push_back(char(uint8_t(cp)));
      continue;
    }
    bool mapped = false;
    for (const auto& s : kCp1252) {
      if (s.cp == cp) {
        out.push_back(char(s.byte));
        mapped = true;
        break;
      }
    }
    if (!mapped) {
      if (error) {
        char buf[64];
        snprintf(buf, sizeof(buf), "U+%04X has no single-byte mapping", cp);
        *error = buf;
      }
      return false;
    }
  }
  return true;
}

}  // namespace eternalsonata::assets
