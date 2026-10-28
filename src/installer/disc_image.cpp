#include "disc_image.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <utility>
#include <unordered_set>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/logging.h>
#include <rex/ui/progress_window.h>

#include "aes128.h"
#include "icon.generated.h"
#include "progress_theme.h"
#include "ui_text.h"

// Ported from the SDK's GameDataSelector (src/system/game_data_selector.cpp).

namespace eternalsonata {
namespace {

namespace fs = std::filesystem;

std::string FormatBytes(uint64_t bytes) {
  constexpr uint64_t kMiB = 1024ull * 1024;
  constexpr uint64_t kGiB = kMiB * 1024;
  const bool gib = bytes >= kGiB;
  const uint64_t unit = gib ? kGiB : kMiB;
  const uint64_t tenths = (bytes * 10 + unit / 2) / unit;
  return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + (gib ? " GiB" : " MiB");
}

class Progress {
 public:
  Progress(const ExtractProgress& callback, std::string label, uint64_t total)
      : callback_(callback), label_(std::move(label)), total_(total) {
    if (!callback_)
      window_.emplace("Preparing game files", ProgressTheme(), kIconPNG, kIconPNGSize);
    Draw();
  }

  ~Progress() {
    bytes_ = total_;
    Draw();
  }

  void Add(uint64_t n) {
    bytes_ += n;
    const uint64_t now = SDL_GetTicks();
    if (now - last_draw_ms_ >= 33)
      Draw();
    if (now - last_log_ms_ >= 2000) {
      last_log_ms_ = now;
      REXLOG_INFO("{}: {}", label_, Detail());
    }
  }

 private:
  std::string Detail() const {
    if (total_ == 0)
      return FormatBytes(bytes_);
    const uint64_t pct = std::min<uint64_t>(100, bytes_ * 100 / total_);
    return std::to_string(pct) + "%   " + FormatBytes(bytes_) + " of " + FormatBytes(total_);
  }

  void Draw() {
    last_draw_ms_ = SDL_GetTicks();
    const float fraction =
        total_ ? std::min(1.0f, float(double(bytes_) / double(total_))) : -1.0f;
    if (callback_)
      callback_(label_, fraction, Detail());
    else
      window_->Draw(label_, fraction, Detail());
  }

  ExtractProgress callback_;
  std::optional<rex::ui::ProgressWindow> window_;
  std::string label_;
  uint64_t total_ = 0;
  uint64_t bytes_ = 0;
  uint64_t last_draw_ms_ = 0;
  uint64_t last_log_ms_ = 0;
};

constexpr uint64_t kSectorSize = 2048;

// Bounds checked reads over an untrusted image. SDL_IOStream so an Android
// content URI is read where it lives. Once given a disc key, the sectors of
// an encrypted PS3 image come out decrypted.
class FileReader {
 public:
  explicit FileReader(const std::string& path) {
    io_ = SDL_IOFromFile(path.c_str(), "rb");
    if (!io_) {
      REXLOG_ERROR("Failed to open {}: {}", path, SDL_GetError());
      return;
    }
    const Sint64 size = SDL_GetIOSize(io_);
    if (size > 0)
      size_ = uint64_t(size);
  }

  ~FileReader() {
    if (io_)
      SDL_CloseIO(io_);
  }

  FileReader(const FileReader&) = delete;
  FileReader& operator=(const FileReader&) = delete;

  bool ok() const { return size_ > 0; }

  // Inclusive sector ranges.
  using Regions = std::vector<std::pair<uint64_t, uint64_t>>;

  void SetDiscKey(Regions encrypted, const uint8_t key[16]) {
    encrypted_ = std::move(encrypted);
    key_.emplace(key);
  }

  bool Read(uint64_t off, void* dst, uint64_t len) {
    if (len == 0)
      return true;
    if (off > size_ || len > size_ - off)
      return false;
    const uint64_t first = off / kSectorSize, last = (off + len - 1) / kSectorSize;
    if (!key_ || !Encrypted(first, last))
      return RawRead(off, dst, len);
    // CBC needs each sector from its start, so whole sectors are read.
    const uint64_t base = first * kSectorSize;
    const uint64_t span = std::min((last + 1) * kSectorSize, size_) - base;
    scratch_.resize(span);
    if (!RawRead(base, scratch_.data(), span))
      return false;
    for (uint64_t sector = first; sector <= last; ++sector) {
      const uint64_t at = (sector - first) * kSectorSize;
      if (at + kSectorSize > span || !Encrypted(sector, sector))
        continue;
      uint8_t iv[16];
      SectorIv(sector, iv);
      key_->DecryptCbc(scratch_.data() + at, kSectorSize, iv);
    }
    std::memcpy(dst, scratch_.data() + (off - base), len);
    return true;
  }

  // Ignores the disc key.
  bool RawRead(uint64_t off, void* dst, uint64_t len) {
    if (len == 0)
      return true;
    if (off > size_ || len > size_ - off)
      return false;
    if (SDL_SeekIO(io_, Sint64(off), SDL_IO_SEEK_SET) < 0)
      return false;
    auto* p = static_cast<char*>(dst);
    while (len > 0) {
      const size_t n = SDL_ReadIO(io_, p, size_t(len));
      if (n == 0)
        return false;
      p += n;
      len -= n;
    }
    return true;
  }

  std::optional<uint8_t> U8(uint64_t off) {
    uint8_t v;
    return Read(off, &v, 1) ? std::optional(v) : std::nullopt;
  }

  std::optional<uint16_t> U16LE(uint64_t off) {
    uint8_t b[2];
    if (!Read(off, b, sizeof(b)))
      return std::nullopt;
    return uint16_t(b[0] | (b[1] << 8));
  }

  uint64_t size() const { return size_; }

  // Each encrypted sector is CBC on its own, its IV the sector number.
  static void SectorIv(uint64_t sector, uint8_t iv[16]) {
    std::memset(iv, 0, 16);
    for (int i = 0; i < 8; ++i)
      iv[15 - i] = uint8_t(sector >> (8 * i));
  }

  std::optional<uint32_t> U32LE(uint64_t off) {
    uint8_t b[4];
    if (!Read(off, b, sizeof(b)))
      return std::nullopt;
    return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
  }

  bool MagicAt(uint64_t off, std::string_view magic) {
    std::string buf(magic.size(), '\0');
    return Read(off, buf.data(), magic.size()) && buf == magic;
  }

  bool CopyTo(std::ostream& out, uint64_t off, uint64_t len, Progress* progress) {
    std::vector<char> chunk(1024 * 1024);
    while (len > 0) {
      const uint64_t n = std::min<uint64_t>(len, chunk.size());
      if (!Read(off, chunk.data(), n))
        return false;
      out.write(chunk.data(), std::streamsize(n));
      if (!out)
        return false;
      off += n;
      len -= n;
      if (progress)
        progress->Add(n);
    }
    return true;
  }

 private:
  bool Encrypted(uint64_t first, uint64_t last) const {
    return std::any_of(encrypted_.begin(), encrypted_.end(),
                       [&](const auto& r) { return r.first <= last && r.second >= first; });
  }

  SDL_IOStream* io_ = nullptr;
  uint64_t size_ = 0;
  std::optional<Aes128> key_;
  Regions encrypted_;
  std::vector<uint8_t> scratch_;
};

// Joins archive supplied names under `base`, refusing anything that escapes it.
std::optional<fs::path> SafeJoin(const fs::path& base, const std::string& name) {
  if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\:") != std::string::npos)
    return std::nullopt;
  const fs::path p(name);
  if (p.has_root_name() || p.has_root_directory() || std::distance(p.begin(), p.end()) != 1)
    return std::nullopt;
  return base / p;
}

// Partition offsets seen in the wild; the volume descriptor sits 32 sectors in.
constexpr uint64_t kXdvdfsPartitionOffsets[] = {0x00000000, 0x0000FB20, 0x00020600, 0x02080000,
                                                0x0FD90000};
constexpr std::string_view kXdvdfsMagic = "MICROSOFT*XBOX*MEDIA";
constexpr int kMaxDirectoryDepth = 64;

struct Xdvdfs {
  uint64_t game_offset;
  uint64_t root_offset;
};

std::optional<Xdvdfs> FindXdvdfs(FileReader& reader) {
  for (const uint64_t game_offset : kXdvdfsPartitionOffsets) {
    const uint64_t fs_off = game_offset + 32 * kSectorSize;
    if (!reader.MagicAt(fs_off, kXdvdfsMagic))
      continue;
    const auto root_sector = reader.U32LE(fs_off + 20);
    const auto root_size = reader.U32LE(fs_off + 24);
    if (!root_sector || !root_size || *root_size < 13 || *root_size > 32 * 1024 * 1024)
      continue;
    return Xdvdfs{game_offset, game_offset + uint64_t(*root_sector) * kSectorSize};
  }
  return std::nullopt;
}

struct Walk {
  // Sums sizes without writing, so the progress bar has a real total.
  bool measure_only = false;
  bool complete = true;
  uint32_t files = 0;
  uint64_t bytes = 0;
  std::vector<std::string> root_names;
  Progress* progress = nullptr;
};

// One directory is a binary tree of entries; a malformed image can make it
// cyclic, so visited ordinals are tracked.
void WalkDirectory(FileReader& reader, uint64_t game_offset, uint64_t dir_offset,
                   const fs::path& out_dir, int depth, Walk& walk) {
  if (depth > kMaxDirectoryDepth) {
    REXLOG_WARN("XDVDFS: directories nest deeper than {}", kMaxDirectoryDepth);
    walk.complete = false;
    return;
  }
  std::vector<uint32_t> pending = {0};
  std::unordered_set<uint32_t> visited;
  while (!pending.empty()) {
    const uint32_t ordinal = pending.back();
    pending.pop_back();
    if (!visited.insert(ordinal).second) {
      walk.complete = false;
      continue;
    }
    const uint64_t p = dir_offset + uint64_t(ordinal) * 4;
    const auto left = reader.U16LE(p);
    const auto right = reader.U16LE(p + 2);
    const auto sector = reader.U32LE(p + 4);
    const auto length = reader.U32LE(p + 8);
    const auto attributes = reader.U8(p + 12);
    const auto name_length = reader.U8(p + 13);
    if (!left || !right || !sector || !length || !attributes || !name_length || !*name_length) {
      REXLOG_WARN("XDVDFS: truncated directory entry at 0x{:X}", p);
      walk.complete = false;
      continue;
    }
    std::string name(*name_length, '\0');
    if (!reader.Read(p + 14, name.data(), *name_length)) {
      walk.complete = false;
      continue;
    }
    if (*left)
      pending.push_back(*left);
    if (*right)
      pending.push_back(*right);

    const auto dest = SafeJoin(out_dir, name);
    if (!dest) {
      REXLOG_WARN("XDVDFS: rejected entry name '{}'", name);
      walk.complete = false;
      continue;
    }
    if (depth == 0)
      walk.root_names.push_back(name);

    const uint64_t entry_offset = game_offset + uint64_t(*sector) * kSectorSize;
    if (*attributes & 0x10) {
      std::error_code ec;
      if (!walk.measure_only)
        fs::create_directories(*dest, ec);
      if (*length)
        WalkDirectory(reader, game_offset, entry_offset, *dest, depth + 1, walk);
      continue;
    }
    ++walk.files;
    if (walk.measure_only) {
      walk.bytes += *length;
      continue;
    }
    std::ofstream out(*dest, std::ios::binary | std::ios::trunc);
    if (!out || !reader.CopyTo(out, entry_offset, *length, walk.progress)) {
      REXLOG_WARN("XDVDFS: could not write {}", dest->string());
      walk.complete = false;
    }
  }
}

// A root directory entry by name: its offset and length.
std::optional<std::pair<uint64_t, uint32_t>> FindRootFile(FileReader& reader, const Xdvdfs& fs,
                                                          std::string_view wanted) {
  std::vector<uint32_t> pending = {0};
  std::unordered_set<uint32_t> visited;
  while (!pending.empty()) {
    const uint32_t ordinal = pending.back();
    pending.pop_back();
    if (!visited.insert(ordinal).second)
      continue;
    const uint64_t p = fs.root_offset + uint64_t(ordinal) * 4;
    const auto left = reader.U16LE(p);
    const auto right = reader.U16LE(p + 2);
    const auto sector = reader.U32LE(p + 4);
    const auto length = reader.U32LE(p + 8);
    const auto attributes = reader.U8(p + 12);
    const auto name_length = reader.U8(p + 13);
    if (!left || !right || !sector || !length || !attributes || !name_length)
      continue;
    std::string name(*name_length, '\0');
    if (!reader.Read(p + 14, name.data(), *name_length))
      continue;
    if (!(*attributes & 0x10) && SameFileName(name, wanted))
      return std::pair(fs.game_offset + uint64_t(*sector) * kSectorSize, *length);
    if (*left)
      pending.push_back(*left);
    if (*right)
      pending.push_back(*right);
  }
  return std::nullopt;
}

// ISO 9660, as a decrypted PS3 disc image is laid out.
constexpr uint64_t kIsoDescriptorOffset = 16 * kSectorSize;

struct IsoEntry {
  std::string name;
  uint64_t offset;
  uint64_t length;
  bool directory;
  // A file over 1 GiB is several extents; these follow the first.
  std::vector<std::pair<uint64_t, uint32_t>> more;
};

bool IsIso9660(FileReader& reader) {
  return reader.MagicAt(kIsoDescriptorOffset + 1, "CD001");
}

// The entries of the directory at `extent`, without "." and "..". Names lose
// their ";1" version suffix.
std::vector<IsoEntry> ReadIsoDirectory(FileReader& reader, uint32_t extent, uint32_t length) {
  std::vector<IsoEntry> entries;
  if (length > 64 * 1024 * 1024)
    return entries;
  std::vector<uint8_t> data(length);
  if (!reader.Read(uint64_t(extent) * kSectorSize, data.data(), length))
    return entries;
  size_t at = 0;
  bool continued = false;
  while (at < data.size()) {
    const uint8_t size = data[at];
    if (size == 0) {
      at = (at / kSectorSize + 1) * kSectorSize;
      continue;
    }
    if (at + size > data.size() || size < 34)
      break;
    const uint8_t* r = &data[at];
    const uint8_t name_length = r[32];
    at += size;
    if (continued && !entries.empty()) {
      const uint32_t part = r[2] | (r[3] << 8) | (r[4] << 16) | (uint32_t(r[5]) << 24);
      const uint32_t part_size = r[10] | (r[11] << 8) | (r[12] << 16) | (uint32_t(r[13]) << 24);
      entries.back().more.emplace_back(uint64_t(part) * kSectorSize, part_size);
      entries.back().length += part_size;
      continued = (r[25] & 0x80) != 0;
      continue;
    }
    if (33u + name_length > size || (name_length == 1 && r[33] <= 1))
      continue;
    continued = (r[25] & 0x80) != 0;
    std::string name(reinterpret_cast<const char*>(r + 33), name_length);
    if (const size_t version = name.find(';'); version != std::string::npos)
      name.resize(version);
    if (!name.empty() && name.back() == '.')
      name.pop_back();
    const uint32_t sector = r[2] | (r[3] << 8) | (r[4] << 16) | (uint32_t(r[5]) << 24);
    const uint32_t size_bytes = r[10] | (r[11] << 8) | (r[12] << 16) | (uint32_t(r[13]) << 24);
    entries.push_back({std::move(name), uint64_t(sector) * kSectorSize, size_bytes,
                       (r[25] & 2) != 0, {}});
  }
  return entries;
}

std::optional<IsoEntry> IsoRoot(FileReader& reader) {
  uint8_t root[34];
  if (!reader.Read(kIsoDescriptorOffset + 156, root, sizeof(root)))
    return std::nullopt;
  const uint32_t sector = root[2] | (root[3] << 8) | (root[4] << 16) | (uint32_t(root[5]) << 24);
  const uint32_t length = root[10] | (root[11] << 8) | (root[12] << 16) | (uint32_t(root[13]) << 24);
  return IsoEntry{"", uint64_t(sector) * kSectorSize, length, true, {}};
}

std::optional<IsoEntry> FindIsoPath(FileReader& reader, std::string_view path) {
  auto at = IsoRoot(reader);
  while (at && !path.empty()) {
    const size_t slash = path.find('/');
    const std::string_view part = path.substr(0, slash);
    path = slash == std::string_view::npos ? std::string_view() : path.substr(slash + 1);
    if (!at->directory)
      return std::nullopt;
    std::optional<IsoEntry> next;
    for (IsoEntry& e : ReadIsoDirectory(reader, uint32_t(at->offset / kSectorSize), uint32_t(at->length))) {
      if (SameFileName(e.name, part)) {
        next = std::move(e);
        break;
      }
    }
    at = std::move(next);
  }
  return at;
}

// Redump's disc keys for BLES00444, BLUS30161 and BLJS10017.
constexpr uint8_t kDiscKeys[][16] = {
    {0x40, 0x12, 0x3B, 0xA7, 0xE7, 0xAC, 0xC6, 0x2E, 0xDC, 0xBF, 0x78, 0xAC, 0x19, 0x3F, 0x03, 0x2D},
    {0xCB, 0x61, 0xF9, 0xB3, 0x02, 0xA9, 0x4D, 0x76, 0x31, 0x14, 0x12, 0xA5, 0x83, 0x44, 0xA0, 0x80},
    {0x59, 0xAB, 0x76, 0x3C, 0x1E, 0x57, 0x28, 0xCB, 0xB1, 0x55, 0x38, 0x14, 0xF0, 0xE4, 0x01, 0xA3},
};

// Sector 0 of a raw PS3 image lists its plain regions as inclusive start and
// end sector pairs; the gaps between them are encrypted.
FileReader::Regions ReadEncryptedRegions(FileReader& reader) {
  uint8_t s[kSectorSize];
  FileReader::Regions encrypted;
  if (!reader.RawRead(0, s, sizeof(s)))
    return encrypted;
  auto be = [&](size_t at) {
    return (uint32_t(s[at]) << 24) | (uint32_t(s[at + 1]) << 16) | (uint32_t(s[at + 2]) << 8) | s[at + 3];
  };
  const uint32_t count = be(0);
  if (count < 2 || count > 127)
    return encrypted;
  const uint64_t sectors = reader.size() / kSectorSize;
  uint64_t previous_end = 0;
  for (uint32_t k = 0; k < count; ++k) {
    const uint64_t start = be(8 + 8 * k), end = be(12 + 8 * k);
    if ((k == 0 && start != 0) || (k > 0 && start <= previous_end + 1) || end < start || end >= sectors)
      return {};
    if (k > 0)
      encrypted.emplace_back(previous_end + 1, start - 1);
    previous_end = end;
  }
  return encrypted;
}

// A key file as RPCS3 takes them: a .dkey of 32 hex digits or a .key of 16
// raw bytes. SDL, so a content URI works.
bool ReadKeyFile(const std::string& path, uint8_t key[16]) {
  size_t size = 0;
  void* data = SDL_LoadFile(path.c_str(), &size);
  if (!data)
    return false;
  const std::string text(static_cast<const char*>(data), size);
  SDL_free(data);
  if (size == 16) {
    std::memcpy(key, text.data(), 16);
    return true;
  }
  bool hex = size >= 32;
  for (int i = 0; i < 16 && hex; ++i) {
    const char pair[3] = {text[2 * i], text[2 * i + 1], 0};
    hex = std::isxdigit(uint8_t(pair[0])) && std::isxdigit(uint8_t(pair[1]));
    key[i] = uint8_t(std::strtoul(pair, nullptr, 16));
  }
  return hex;
}

// Keys the player picked, for as long as the game runs.
std::mutex g_key_mutex;
std::vector<std::array<uint8_t, 16>> g_picked_keys;

// What a disc key is tested against: EBOOT.BIN's first encrypted block. Its
// filesystem sits in a plain region, so the file can be found first.
struct Ps3Lock {
  FileReader::Regions encrypted;
  uint64_t sector;
  uint8_t cipher[16];

  bool Fits(const uint8_t key[16]) const {
    uint8_t block[16], iv[16];
    std::memcpy(block, cipher, sizeof(block));
    FileReader::SectorIv(sector, iv);
    Aes128(key).DecryptCbc(block, sizeof(block), iv);
    return std::memcmp(block, "SCE\0", 4) == 0;
  }
};

// Empty for an image that is not an encrypted PS3 one.
std::optional<Ps3Lock> FindPs3Lock(FileReader& reader) {
  Ps3Lock lock;
  lock.encrypted = ReadEncryptedRegions(reader);
  if (lock.encrypted.empty())
    return std::nullopt;
  const auto eboot = FindIsoPath(reader, "PS3_GAME/USRDIR/EBOOT.BIN");
  if (!eboot || eboot->directory || !reader.RawRead(eboot->offset, lock.cipher, sizeof(lock.cipher)) ||
      std::memcmp(lock.cipher, "SCE\0", 4) == 0)
    return std::nullopt;
  lock.sector = eboot->offset / kSectorSize;
  return lock;
}

// Gives the reader of an encrypted PS3 image the disc key that turns
// EBOOT.BIN back into a SELF: from a key file beside it, one the player
// picked, or a known one.
void UnlockPs3Image(FileReader& reader, const std::string& image) {
  std::optional<Ps3Lock> lock = FindPs3Lock(reader);
  if (!lock)
    return;
  uint8_t key[16];
  if (!image.starts_with("content://")) {
    for (const char* ext : {".dkey", ".key"}) {
      if (ReadKeyFile(fs::path(image).replace_extension(ext).string(), key) && lock->Fits(key)) {
        REXLOG_INFO("Decrypting the PS3 disc image with the key file beside it");
        reader.SetDiscKey(std::move(lock->encrypted), key);
        return;
      }
    }
  }
  {
    std::lock_guard guard(g_key_mutex);
    for (const auto& picked : g_picked_keys) {
      if (lock->Fits(picked.data())) {
        REXLOG_INFO("Decrypting the PS3 disc image with the picked key file");
        reader.SetDiscKey(std::move(lock->encrypted), picked.data());
        return;
      }
    }
  }
  for (const auto& known : kDiscKeys) {
    if (lock->Fits(known)) {
      REXLOG_INFO("Decrypting the PS3 disc image with a known disc key");
      reader.SetDiscKey(std::move(lock->encrypted), known);
      return;
    }
  }
  REXLOG_WARN("Encrypted PS3 disc image, and no disc key fits it");
}

// An ISO 9660 image, unlocked if it is an encrypted PS3 one.
bool OpenIso(FileReader& reader, const std::string& image) {
  if (!reader.ok() || !IsIso9660(reader))
    return false;
  UnlockPs3Image(reader, image);
  return true;
}

uint64_t MoreBytes(const IsoEntry& e) {
  uint64_t n = 0;
  for (const auto& part : e.more)
    n += part.second;
  return n;
}

void CountIso(FileReader& reader, const IsoEntry& dir, Walk& walk, int depth) {
  if (depth > kMaxDirectoryDepth) {
    walk.complete = false;
    return;
  }
  for (const IsoEntry& e : ReadIsoDirectory(reader, uint32_t(dir.offset / kSectorSize), uint32_t(dir.length))) {
    if (e.directory) {
      CountIso(reader, e, walk, depth + 1);
    } else {
      ++walk.files;
      walk.bytes += e.length;
    }
  }
}

void CopyIso(FileReader& reader, const IsoEntry& dir, const fs::path& out_dir, Walk& walk,
             int depth) {
  if (depth > kMaxDirectoryDepth) {
    walk.complete = false;
    return;
  }
  std::error_code ec;
  fs::create_directories(out_dir, ec);
  for (const IsoEntry& e : ReadIsoDirectory(reader, uint32_t(dir.offset / kSectorSize), uint32_t(dir.length))) {
    const auto dest = SafeJoin(out_dir, e.name);
    if (!dest) {
      REXLOG_WARN("ISO: rejected entry name '{}'", e.name);
      walk.complete = false;
      continue;
    }
    if (e.directory) {
      CopyIso(reader, e, *dest, walk, depth + 1);
      continue;
    }
    std::ofstream out(*dest, std::ios::binary | std::ios::trunc);
    bool copied = out && reader.CopyTo(out, e.offset, e.more.empty() ? e.length : e.length - MoreBytes(e),
                                       walk.progress);
    for (const auto& [offset, length] : e.more)
      copied = copied && reader.CopyTo(out, offset, length, walk.progress);
    if (!copied) {
      REXLOG_WARN("ISO: could not write {}", dest->string());
      walk.complete = false;
    }
  }
}

}  // namespace

bool MoveAside(const fs::path& dir) {
  std::error_code ec;
  if (fs::is_directory(dir, ec) && fs::is_empty(dir, ec))
    return fs::remove(dir, ec);
  for (int i = 0; i < 100; ++i) {
    fs::path aside = dir;
    aside += i ? ".old" + std::to_string(i) : ".old";
    if (fs::exists(aside, ec))
      continue;
    fs::rename(dir, aside, ec);
    if (ec)
      return false;
    REXLOG_WARN("Moved the unusable {} to {}", dir.string(), aside.string());
    return true;
  }
  return false;
}

bool SameFileName(std::string_view a, std::string_view b) {
  return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
    return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
  });
}

std::string ExtractDiscImage(const std::string& image, const fs::path& out_dir,
                             std::string_view required_entry, const ExtractProgress& progress_callback) {
  FileReader reader(image);
  if (!reader.ok())
    return TrErr("err_read");
  const auto info = FindXdvdfs(reader);
  if (!info)
    return TrErr("err_not_xbox");

  Walk measured;
  measured.measure_only = true;
  WalkDirectory(reader, info->game_offset, info->root_offset, out_dir, 0, measured);
  const bool ours = std::any_of(measured.root_names.begin(), measured.root_names.end(),
                                [&](const std::string& n) { return SameFileName(n, required_entry); });
  if (!ours)
    return TrErr("err_not_es");
  REXLOG_INFO("Disc image holds {} files, {}", measured.files, FormatBytes(measured.bytes));

  fs::path partial = out_dir;
  partial += ".partial";
  std::error_code ec;
  fs::remove_all(partial, ec);
  fs::create_directories(partial, ec);
  if (ec)
    return TrErr("err_create", partial.string(), ec.message());

  Walk walk;
  {
    Progress progress(progress_callback, "Extracting game files...", measured.bytes);
    walk.progress = &progress;
    WalkDirectory(reader, info->game_offset, info->root_offset, partial, 0, walk);
  }
  if (!walk.complete) {
    fs::remove_all(partial, ec);
    return TrErr("err_extract_failed");
  }
  if (fs::exists(out_dir, ec) && !MoveAside(out_dir))
    return TrErr("err_move_aside", out_dir.string());
  fs::rename(partial, out_dir, ec);
  if (ec)
    return TrErr("err_rename_extracted", ec.message());
  REXLOG_INFO("Extracted {} files into {}", walk.files, out_dir.string());
  return {};
}

std::string ReadDiscImageFile(const std::string& image, std::string_view name,
                              std::vector<uint8_t>& out) {
  FileReader reader(image);
  if (!reader.ok())
    return TrErr("err_read");
  const auto info = FindXdvdfs(reader);
  if (!info)
    return TrErr("err_not_xbox");
  const auto file = FindRootFile(reader, *info, name);
  if (!file)
    return TrErr("err_not_es");
  out.resize(file->second);
  if (!reader.Read(file->first, out.data(), out.size()))
    return TrErr("err_truncated");
  return {};
}

uint64_t MeasureDiscImage(const std::string& image) {
  FileReader reader(image);
  if (!reader.ok())
    return 0;
  const auto info = FindXdvdfs(reader);
  if (!info)
    return 0;
  Walk measured;
  measured.measure_only = true;
  WalkDirectory(reader, info->game_offset, info->root_offset, {}, 0, measured);
  return measured.bytes;
}

uint64_t MeasureIsoFolder(const std::string& image, std::string_view folder) {
  FileReader reader(image);
  if (!OpenIso(reader, image))
    return 0;
  const auto dir = FindIsoPath(reader, folder);
  if (!dir || !dir->directory)
    return 0;
  Walk measured;
  CountIso(reader, *dir, measured, 0);
  return measured.bytes;
}

bool IsIsoImage(const std::string& image) {
  FileReader reader(image);
  // Xbox 360 images carry a stub ISO 9660 descriptor too.
  return reader.ok() && IsIso9660(reader) && !FindXdvdfs(reader);
}

std::string ReadIsoImageFile(const std::string& image, std::string_view path,
                             std::vector<uint8_t>& out) {
  FileReader reader(image);
  if (!reader.ok())
    return TrErr("err_read");
  const auto file = OpenIso(reader, image) ? FindIsoPath(reader, path) : std::nullopt;
  if (!file || file->directory)
    return TrErr("err_not_es");
  out.resize(file->length);
  if (!file->more.empty() ||
      !reader.Read(file->offset, out.data(), out.size()))
    return TrErr("err_truncated");
  return {};
}

std::string WithIsoFiles(const std::string& image, std::string_view folder, std::string_view suffix,
                         const std::function<std::string(const std::vector<ImageFile>&)>& use) {
  FileReader reader(image);
  if (!reader.ok())
    return TrErr("err_read");
  const auto dir = OpenIso(reader, image) ? FindIsoPath(reader, folder) : std::nullopt;
  if (!dir || !dir->directory)
    return TrErr("err_not_es");
  std::vector<IsoEntry> entries;
  std::vector<ImageFile> files;
  for (IsoEntry& e : ReadIsoDirectory(reader, uint32_t(dir->offset / kSectorSize), uint32_t(dir->length))) {
    if (e.directory || e.name.size() < suffix.size() ||
        !SameFileName(std::string_view(e.name).substr(e.name.size() - suffix.size()), suffix))
      continue;
    entries.push_back(std::move(e));
  }
  for (const IsoEntry& e : entries) {
    files.push_back({e.name, e.length, [&reader, &e](uint64_t off, void* dst, uint64_t len) {
                       // The first extent, then the ones that follow it.
                       uint64_t start = e.offset, extent = e.length - MoreBytes(e);
                       size_t next = 0;
                       auto* out = static_cast<char*>(dst);
                       while (len) {
                         if (off >= extent) {
                           if (next >= e.more.size())
                             return false;
                           off -= extent;
                           start = e.more[next].first;
                           extent = e.more[next++].second;
                           continue;
                         }
                         const uint64_t n = std::min(len, extent - off);
                         if (!reader.Read(start + off, out, n))
                           return false;
                         out += n;
                         off += n;
                         len -= n;
                       }
                       return true;
                     }});
  }
  return use(files);
}

std::string AddPs3DiscKey(const std::string& image, const std::string& key_file) {
  std::array<uint8_t, 16> key;
  if (!ReadKeyFile(key_file, key.data()))
    return TrErr("err_bad_dkey");
  FileReader reader(image);
  if (!reader.ok() || !IsIso9660(reader))
    return TrErr("err_read");
  const std::optional<Ps3Lock> lock = FindPs3Lock(reader);
  if (lock && !lock->Fits(key.data()))
    return TrErr("err_wrong_dkey");
  std::lock_guard guard(g_key_mutex);
  g_picked_keys.push_back(key);
  return {};
}

std::string ExtractIsoFolder(const std::string& image, std::string_view folder,
                             const fs::path& out_dir, const ExtractProgress& progress_callback) {
  FileReader reader(image);
  if (!reader.ok())
    return TrErr("err_read");
  const auto dir = OpenIso(reader, image) ? FindIsoPath(reader, folder) : std::nullopt;
  if (!dir || !dir->directory)
    return TrErr("err_not_es");
  Walk measured;
  CountIso(reader, *dir, measured, 0);
  REXLOG_INFO("Disc image holds {} files, {}", measured.files, FormatBytes(measured.bytes));

  std::error_code ec;
  fs::remove_all(out_dir, ec);
  Walk walk;
  {
    Progress progress(progress_callback, "Extracting game files...", measured.bytes);
    walk.progress = &progress;
    CopyIso(reader, *dir, out_dir, walk, 0);
  }
  if (!walk.complete) {
    fs::remove_all(out_dir, ec);
    return TrErr("err_extract_failed");
  }
  return {};
}

}  // namespace eternalsonata
