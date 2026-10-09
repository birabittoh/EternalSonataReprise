#include "release_patch.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <rex/logging.h>
#include <rex/system/game_data_selector.h>

#include "eternalsonata_asset_container.h"

// The USA and JP to PAL patch bundle, linked in by release-patches.S.
extern "C" {
extern const uint8_t kReleasePatchData[];
extern const uint8_t kReleasePatchDataEnd[];
}

namespace eternalsonata {
namespace {

namespace fs = std::filesystem;

constexpr const char* kToc = "index.vmtoc";
constexpr const char* kStamp = "release-patches.stamp";
constexpr const char* kOriginal = ".orig";
// PS3 data is converted, not patched (target.cpp has the same probe).
constexpr const char* kPs3Probe = "pcalg_v1.p3obj";
// Not a release's file: the Japanese title screen, patched from PAL's
// title.bmd, since PAL's has no Japanese logo.
constexpr std::string_view kJapaneseTitle = "title_jpn.bmd";

struct Patch {
  std::string_view path;
  std::span<const uint8_t> data;
};

// See scripts/gen-release-patches.py for the layout.
std::vector<Patch> Bundle() {
  const std::span<const uint8_t> bundle(kReleasePatchData, kReleasePatchDataEnd);
  std::vector<Patch> patches;
  if (bundle.size() < 8 || std::memcmp(bundle.data(), "RXDB", 4) != 0)
    return patches;
  for (size_t at = 8; at + 68 <= bundle.size();) {
    const auto* rec = reinterpret_cast<const char*>(bundle.data() + at);
    const uint32_t size = uint32_t(bundle[at + 64]) | uint32_t(bundle[at + 65]) << 8 |
                          uint32_t(bundle[at + 66]) << 16 | uint32_t(bundle[at + 67]) << 24;
    if (at + 68 + size > bundle.size())
      break;
    patches.push_back({std::string_view(rec, strnlen(rec, 64)), bundle.subspan(at + 68, size)});
    at += 68 + size;
  }
  return patches;
}

std::string Fingerprint() {
  uint64_t h = 0xCBF29CE484222325ull;
  for (const uint8_t* p = kReleasePatchData; p != kReleasePatchDataEnd; ++p)
    h = (h ^ *p) * 0x100000001B3ull;
  char text[17];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(h));
  return text;
}

bool ReadFile(const fs::path& path, std::vector<uint8_t>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return !in.bad();
}

// Through a temporary, so an interrupted write never leaves a short file.
bool WriteFile(const fs::path& path, std::span<const uint8_t> bytes) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  fs::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    if (!out)
      return false;
  }
  fs::rename(tmp, path, ec);
  return !ec;
}

fs::path WithSuffix(fs::path path, const char* suffix) {
  path += suffix;
  return path;
}

bool SameName(const std::string& a, const std::string& b) {
  return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
    return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
  });
}

// `rel` under `dir` in the spelling on disk; parts that do not exist yet are
// kept as given. Disc names are mixed case and Android's storage is not
// case insensitive.
fs::path Resolve(const fs::path& dir, std::string_view rel) {
  std::error_code ec;
  fs::path at = dir;
  for (const auto& part : fs::path(rel)) {
    fs::path next = at / part;
    if (!fs::exists(next, ec)) {
      for (const auto& entry : fs::directory_iterator(at, ec)) {
        if (SameName(entry.path().filename().string(), part.string())) {
          next = entry.path();
          break;
        }
      }
    }
    at = std::move(next);
  }
  return at;
}

// The shipped bytes of `rel`, decoded, from before any earlier patch.
bool LoadShipped(const fs::path& dir, const assets::Toc& toc, std::string_view rel,
                 std::vector<uint8_t>& out) {
  const assets::TocEntry* entry = toc.Find(rel);
  if (!entry)
    return false;
  const fs::path file = Resolve(dir, rel);
  std::error_code ec;
  const fs::path original = WithSuffix(file, kOriginal);
  std::vector<uint8_t> encoded;
  if (!ReadFile(fs::is_regular_file(original, ec) ? original : file, encoded))
    return false;
  if (entry->flag == 0) {
    out = std::move(encoded);
    return true;
  }
  return assets::DecodeAsset(encoded.data(), encoded.size(), entry->size, entry->flag, out);
}

// The first of `path`'s patches that accepts `source`.
bool ApplyAny(const std::vector<Patch>& bundle, std::string_view path,
              const std::vector<uint8_t>& source, std::vector<uint8_t>& out) {
  for (const Patch& patch : bundle) {
    if (patch.path == path && rex::system::ApplyReleasePatch(patch.data, source, out))
      return true;
  }
  return false;
}

// The camp menu loads its art from campdata/camp_grpN.bmd, one per language
// (sub_821E8E28), and waits forever for one that is missing. Only PAL ships
// them: USA and JP keep their own language's textures in AppKeep.bmd, in the
// same slots, which PAL left empty. So each is rebuilt from those slots, in
// the order camp_grp1 lists them, with that one language's art standing in
// for every language. JP's are byte for byte PAL's camp_grp0.
constexpr std::array kCampGroupSlots = {251, 268, 269, 267, 304, 305, 306};
constexpr size_t kCampGroupHeader = 0x30;

bool BuildCampGroup(const std::vector<uint8_t>& keep, std::vector<uint8_t>& group) {
  auto be32 = [&](size_t off) {
    return uint32_t(keep[off]) << 24 | uint32_t(keep[off + 1]) << 16 |
           uint32_t(keep[off + 2]) << 8 | uint32_t(keep[off + 3]);
  };
  if (keep.size() < 16 || std::memcmp(keep.data(), "BMD ", 4) != 0)
    return false;
  const uint32_t count = be32(8);
  if (count <= kCampGroupSlots.back() + 1 || 12 + 4 * size_t(count) > keep.size())
    return false;
  group.assign(kCampGroupHeader, 0);
  std::memcpy(group.data(), "CAMP", 4);
  auto put32 = [&](size_t off, uint32_t v) {
    for (int i = 0; i < 4; ++i)
      group[off + i] = uint8_t(v >> (24 - 8 * i));
  };
  put32(8, uint32_t(kCampGroupSlots.size()));
  for (size_t i = 0; i < kCampGroupSlots.size(); ++i) {
    const uint32_t begin = be32(12 + 4 * kCampGroupSlots[i]);
    const uint32_t end = be32(12 + 4 * (kCampGroupSlots[i] + 1));
    if (end <= begin || end > keep.size() || std::memcmp(keep.data() + begin, "NTEX", 4) != 0)
      return false;
    put32(12 + 4 * i, uint32_t(group.size()));
    group.insert(group.end(), keep.begin() + begin, keep.begin() + end);
  }
  put32(4, uint32_t(group.size()));
  return true;
}

// Converted PS3 data is PAL's already, so only the Japanese title screen is
// added, from the converted title.bmd, which has no Japanese logo either.
std::string PatchPs3Title(const fs::path& dir, const std::vector<Patch>& bundle) {
  const fs::path toc_path = dir / kToc;
  assets::Toc toc;
  std::vector<uint8_t> title, japanese;
  if (!toc.Load(toc_path) || !ReadFile(Resolve(dir, "title.bmd"), title))
    return "The converted game files are incomplete: title.bmd could not be read.";
  if (ApplyAny(bundle, kJapaneseTitle, title, japanese)) {
    if (!WriteFile(Resolve(dir, kJapaneseTitle), japanese))
      return "Could not write title_jpn.bmd.";
    const uint32_t size = uint32_t(japanese.size());
    if (!(toc.Find(kJapaneseTitle) ? toc.SetStored(kJapaneseTitle, size)
                                   : toc.AddStored(kJapaneseTitle, size)))
      return "title_jpn.bmd does not fit an index.vmtoc record.";
    if (!WriteFile(toc_path, toc.bytes()))
      return "Could not write index.vmtoc.";
  } else {
    REXLOG_WARN("No Japanese title screen matches this PS3 copy's title.bmd");
  }
  const std::string stamp = Fingerprint();
  WriteFile(dir / kStamp, std::span(reinterpret_cast<const uint8_t*>(stamp.data()), stamp.size()));
  return {};
}

}  // namespace

bool IsReleasePatched(const fs::path& dir) {
  std::vector<uint8_t> stamp;
  const std::string want = Fingerprint();
  return ReadFile(dir / kStamp, stamp) &&
         std::string_view(reinterpret_cast<const char*>(stamp.data()), stamp.size()) == want;
}

std::string PatchRelease(const fs::path& dir, const ExtractProgress& progress) {
  const std::vector<Patch> bundle = Bundle();
  std::error_code ec;
  if (bundle.empty()) {
    REXLOG_WARN("No release patches in this build; USA and JP copies will not run");
    return {};
  }
  if (IsReleasePatched(dir))
    return {};
  if (fs::is_regular_file(dir / kPs3Probe, ec))
    return PatchPs3Title(dir, bundle);

  const fs::path toc_path = dir / kToc;
  const fs::path shipped_toc_path = WithSuffix(toc_path, kOriginal);
  if (!fs::is_regular_file(shipped_toc_path, ec)) {
    fs::copy_file(toc_path, shipped_toc_path, ec);
    if (ec)
      return "The game folder is not writable (" + ec.message() + ").";
  }
  assets::Toc shipped, toc;
  if (!shipped.Load(shipped_toc_path) || !toc.Load(shipped_toc_path))
    return "index.vmtoc could not be read.";

  std::vector<std::string> paths;
  for (const Patch& patch : bundle) {
    if (std::find(paths.begin(), paths.end(), patch.path) == paths.end())
      paths.emplace_back(patch.path);
  }

  std::vector<std::pair<std::string, std::vector<uint8_t>>> outputs;
  auto report = [&](const std::string& what) {
    if (progress)
      progress("Patching game files...", -1.0f, what);
  };

  std::vector<uint8_t> keep;
  if (!shipped.Find("campdata/camp_grp1.bmd") && LoadShipped(dir, shipped, "appkeep.bmd", keep)) {
    std::vector<uint8_t> group;
    if (!BuildCampGroup(keep, group))
      return "AppKeep.bmd is not the USA or JP layout.";
    for (int n = 0; n < 6; ++n)
      outputs.emplace_back("campdata/camp_grp" + std::to_string(n) + ".bmd", group);
  }

  size_t converted = 0;
  for (const std::string& path : paths) {
    if (path == kJapaneseTitle)
      continue;
    report(path);
    // A file PAL has and this release lacks is patched from nothing.
    std::vector<uint8_t> source, target;
    if (shipped.Find(path) && !LoadShipped(dir, shipped, path, source))
      return "The game files are incomplete: " + path + " could not be read.";
    // PAL's own files, and USA's scp.bmd, which is PAL's, take no patch.
    if (!ApplyAny(bundle, path, source, target))
      continue;
    outputs.emplace_back(path, std::move(target));
    ++converted;
  }

  // From PAL's title.bmd, which this copy's has just become.
  std::vector<uint8_t> title, japanese;
  auto patched_title = std::find_if(outputs.begin(), outputs.end(),
                                    [](const auto& o) { return o.first == "title.bmd"; });
  if (patched_title != outputs.end())
    title = patched_title->second;
  else
    LoadShipped(dir, shipped, "title.bmd", title);
  if (ApplyAny(bundle, kJapaneseTitle, title, japanese))
    outputs.emplace_back(std::string(kJapaneseTitle), std::move(japanese));

  for (const auto& [path, bytes] : outputs) {
    report(path);
    const fs::path file = Resolve(dir, path);
    const bool shipped_file = shipped.Find(path) != nullptr;
    const fs::path original = WithSuffix(file, kOriginal);
    if (shipped_file && !fs::exists(original, ec)) {
      fs::rename(file, original, ec);
      if (ec)
        return "Could not keep the original " + path + " (" + ec.message() + ").";
    }
    if (!WriteFile(file, bytes))
      return "Could not write " + path + ".";
    const uint32_t size = uint32_t(bytes.size());
    if (!(shipped_file ? toc.SetStored(path, size) : toc.AddStored(path, size)))
      return path + " does not fit an index.vmtoc record.";
  }
  if (!WriteFile(toc_path, toc.bytes()))
    return "Could not write index.vmtoc.";
  const std::string stamp = Fingerprint();
  WriteFile(dir / kStamp, std::span(reinterpret_cast<const uint8_t*>(stamp.data()), stamp.size()));
  REXLOG_INFO("Patched {}: {} containers converted to the PAL layout, {} files written",
              dir.string(), converted, outputs.size());
  return {};
}

}  // namespace eternalsonata
