#include "release_patch.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <rex/logging.h>

#include "eternalsonata_asset_container.h"
#include "japanese_title.h"

namespace eternalsonata {
namespace {

namespace fs = std::filesystem;

constexpr const char* kToc = "index.vmtoc";
constexpr const char* kStamp = "release-patches.stamp";
constexpr const char* kOriginal = ".orig";
// PS3 data is converted, not patched (target.cpp has the same probe).
constexpr const char* kPs3Probe = "pcalg_v1.p3obj";
// Not a release's file: the Japanese title screen, built from the
// release's title.bmd (japanese_title.h).
constexpr std::string_view kJapaneseTitle = "title_jpn.bmd";
// Bumped whenever the patch output changes, so installs repatch.
constexpr std::string_view kRecipe = "own-data-1";

std::string Fingerprint() {
  return std::string(kRecipe);
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

uint32_t Be32(const std::vector<uint8_t>& d, size_t off) {
  return uint32_t(d[off]) << 24 | uint32_t(d[off + 1]) << 16 | uint32_t(d[off + 2]) << 8 |
         uint32_t(d[off + 3]);
}

void PutBe32(std::vector<uint8_t>& d, size_t off, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    d[off + i] = uint8_t(v >> (24 - 8 * i));
}

// BattleKeep.bop is addressed by slot. USA and JP keep the results screen and
// level up animations in it, where PAL's code loads them from
// btl_exit_text.tex and levelup_JPN.tex (sub_821A0178; ids -1 and -2 of the
// slot accessor sub_821A5148). JP also lacks the copies of eight effects that
// the other two hold at slots 26..33; its own records name the originals.
struct BattleKeepLayout {
  uint32_t count;
  uint32_t results;
  uint32_t level_up;
  bool copies;
};
constexpr uint32_t kPalBattleKeep = 106;
constexpr std::array kBattleKeepLayouts = {BattleKeepLayout{108, 41, 43, true},
                                           BattleKeepLayout{100, 33, 35, false}};
constexpr size_t kFirstCopy = 26;
constexpr std::array<uint32_t, 8> kCopiedSlots = {12, 17, 20, 14, 15, 18, 19, 21};
constexpr uint8_t kTexMagic[4] = {0x03, 0x33, 0x90, 0x10};

struct BattleKeep {
  std::vector<uint8_t> keep, results, level_up;
};

// Rebuilt in PAL's slot order, entries keeping their alignment: effects and
// banks on 4 KiB, the rest on 32 bytes. Nothing when it is PAL's already.
std::optional<BattleKeep> BuildBattleKeep(const std::vector<uint8_t>& d, std::string& error) {
  error = "BattleKeep.bop is not the USA, JP or PAL layout.";
  if (d.size() < 16 || std::memcmp(d.data(), "BOP ", 4) != 0)
    return std::nullopt;
  const size_t dir = Be32(d, 12);
  if (dir + 4 > d.size())
    return std::nullopt;
  const uint32_t count = Be32(d, dir);
  if (count == kPalBattleKeep) {
    error.clear();
    return std::nullopt;
  }
  const auto layout = std::find_if(kBattleKeepLayouts.begin(), kBattleKeepLayouts.end(),
                                   [&](const BattleKeepLayout& l) { return l.count == count; });
  if (layout == kBattleKeepLayouts.end() || dir + 4 + 4 * size_t(count) > d.size())
    return std::nullopt;

  struct Entry {
    size_t begin = 0, end = 0;
  };
  std::vector<size_t> offsets(count), ends;
  for (uint32_t i = 0; i < count; ++i) {
    offsets[i] = Be32(d, dir + 4 + 4 * i);
    if (offsets[i])
      ends.push_back(offsets[i]);
  }
  ends.push_back(d.size());
  std::sort(ends.begin(), ends.end());
  std::vector<Entry> slots;
  for (size_t at : offsets) {
    if (!at || at >= d.size()) {
      slots.push_back({});
      continue;
    }
    slots.push_back({at, *std::upper_bound(ends.begin(), ends.end(), at)});
  }
  auto is_tex = [&](uint32_t slot) {
    return slots[slot].end - slots[slot].begin >= 8 &&
           std::memcmp(d.data() + slots[slot].begin, kTexMagic, 4) == 0;
  };
  if (!is_tex(layout->results) || !is_tex(layout->level_up))
    return std::nullopt;

  BattleKeep out;
  auto slice = [&](const Entry& e) {
    return std::vector<uint8_t>(d.begin() + e.begin, d.begin() + e.end);
  };
  out.results = slice(slots[layout->results]);
  out.level_up = slice(slots[layout->level_up]);
  slots.erase(slots.begin() + layout->level_up);
  slots.erase(slots.begin() + layout->results);
  if (!layout->copies) {
    std::vector<Entry> copies;
    for (uint32_t slot : kCopiedSlots)
      copies.push_back(slots[slot]);
    slots.insert(slots.begin() + kFirstCopy, copies.begin(), copies.end());
  }
  if (slots.size() != kPalBattleKeep)
    return std::nullopt;

  out.keep.assign(d.begin(), d.begin() + dir);
  out.keep.resize(dir + 4 + 4 * slots.size());
  PutBe32(out.keep, dir, uint32_t(slots.size()));
  for (size_t i = 0; i < slots.size(); ++i) {
    if (!slots[i].begin)
      continue;
    const size_t align = slots[i].begin % 0x1000 == 0 ? 0x1000 : 0x20;
    out.keep.resize((out.keep.size() + align - 1) / align * align);
    PutBe32(out.keep, dir + 4 + 4 * i, uint32_t(out.keep.size()));
    out.keep.insert(out.keep.end(), d.begin() + slots[i].begin, d.begin() + slots[i].end);
  }
  out.keep.resize((out.keep.size() + 0xFFF) / 0x1000 * 0x1000);
  PutBe32(out.keep, 4, uint32_t(out.keep.size()));
  error.clear();
  return out;
}

// Files an earlier patch replaced, with their shipped copy kept as ".orig",
// keyed by guest path.
std::vector<std::pair<std::string, fs::path>> ReplacedFiles(const fs::path& dir) {
  std::vector<std::pair<std::string, fs::path>> found;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(dir, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    const fs::path& path = it->path();
    if (!it->is_regular_file(ec) || path.extension() != kOriginal)
      continue;
    std::string rel = fs::relative(path, dir, ec).replace_extension().generic_string();
    std::transform(rel.begin(), rel.end(), rel.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    if (rel != kToc)
      found.emplace_back(std::move(rel), path);
  }
  return found;
}

// Converted PS3 data is PAL's already, so only the Japanese title screen is
// added, from the converted title.bmd.
std::string PatchPs3Title(const fs::path& dir) {
  const fs::path toc_path = dir / kToc;
  assets::Toc toc;
  std::vector<uint8_t> title;
  if (!toc.Load(toc_path) || !ReadFile(Resolve(dir, "title.bmd"), title))
    return "The converted game files are incomplete: title.bmd could not be read.";
  const std::vector<uint8_t> japanese = BuildJapaneseTitle(title);
  if (!japanese.empty()) {
    if (!WriteFile(Resolve(dir, kJapaneseTitle), japanese))
      return "Could not write title_jpn.bmd.";
    const uint32_t size = uint32_t(japanese.size());
    if (!(toc.Find(kJapaneseTitle) ? toc.SetStored(kJapaneseTitle, size)
                                   : toc.AddStored(kJapaneseTitle, size)))
      return "title_jpn.bmd does not fit an index.vmtoc record.";
    if (!WriteFile(toc_path, toc.bytes()))
      return "Could not write index.vmtoc.";
  } else {
    REXLOG_WARN("This PS3 copy's title.bmd is not the PAL layout; no Japanese title screen");
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
  std::error_code ec;
  if (IsReleasePatched(dir))
    return {};
  if (fs::is_regular_file(dir / kPs3Probe, ec))
    return PatchPs3Title(dir);

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

  report("btldata/battlekeep.bop");
  std::vector<uint8_t> battle_keep;
  if (!LoadShipped(dir, shipped, "btldata/battlekeep.bop", battle_keep))
    return "The game files are incomplete: BattleKeep.bop could not be read.";
  std::string error;
  if (auto rebuilt = BuildBattleKeep(battle_keep, error)) {
    outputs.emplace_back("btldata/battlekeep.bop", std::move(rebuilt->keep));
    outputs.emplace_back("btldata/btl_exit_text.tex", std::move(rebuilt->results));
    outputs.emplace_back("btldata/levelup_jpn.tex", std::move(rebuilt->level_up));
  } else if (!error.empty()) {
    return error;
  }

  std::vector<uint8_t> title;
  LoadShipped(dir, shipped, "title.bmd", title);
  std::vector<uint8_t> japanese = BuildJapaneseTitle(title);
  if (!japanese.empty())
    outputs.emplace_back(std::string(kJapaneseTitle), std::move(japanese));
  else
    REXLOG_WARN("title.bmd is not the PAL layout; no Japanese title screen");

  // Older builds replaced more files; put back the shipped ones.
  for (const auto& [path, original] : ReplacedFiles(dir)) {
    if (std::any_of(outputs.begin(), outputs.end(), [&](const auto& o) { return o.first == path; }))
      continue;
    fs::path file = original;
    file.replace_extension();
    fs::remove(file, ec);
    fs::rename(original, file, ec);
    if (ec)
      return "Could not restore the original " + path + " (" + ec.message() + ").";
  }

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
  REXLOG_INFO("Patched {}: {} files written", dir.string(), outputs.size());
  return {};
}

}  // namespace eternalsonata
