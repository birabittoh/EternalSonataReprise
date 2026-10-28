#include "save_slots.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <system_error>

#include <SDL3/SDL.h>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/platform.h>

#include "ps3_pfd.h"
#include "save_text.generated.h"
#include "ui_text.h"

namespace eternalsonata {
namespace {

namespace fs = std::filesystem;

constexpr char kTitleDir[] = "4E4D07E2";
constexpr uint32_t kTitleId = 0x4E4D07E2;
// The SDK's default profile, where the game files its saves on a fresh install.
constexpr char kDefaultProfile[] = "B13EBABEBABEBABE";
constexpr uint32_t kTicksPerSecond = 300;
constexpr uint32_t kMaxPlaySeconds = 359999;

constexpr size_t kThumbSize = size_t(kSaveThumbWidth) * kSaveThumbHeight * 4;
constexpr size_t kHeaderSize = 24;
constexpr size_t kCharSize = 213140;
constexpr size_t kChars = 12;
constexpr size_t kPs3Size = 2772900;
constexpr size_t k360Body = 2772432;
constexpr size_t kStatsStride = 48;
constexpr size_t kRetailChars = 10;
// The 360 container's "SAV\0", version, key and end offset.
constexpr size_t kWrapHead = 16;

struct Block {
  const char* name;
  size_t size;
};
constexpr Block kPs3Blocks[] = {{"party", 2604},     {"inventory", 2048}, {"stats_base", 576},
                                {"stats_live", 576}, {"map", 4412},       {"b80", 80}};
constexpr Block k360Blocks[] = {{"owned", 4},        {"party", 2324},     {"inventory", 2048},
                                {"stats_base", 480}, {"stats_live", 480}, {"map", 4412},
                                {"b80", 80}};

// Costume state in the PS3 party block: unlock byte per (character, variant),
// then the variant each of Allegretto, Polka and Beat wears (1 is the default).
struct Unlock {
  size_t at;
  const char* name;
  int variant;
};
constexpr Unlock kPs3Unlocks[] = {
    {0x919, "allegretto", 2}, {0x91A, "polka", 2}, {0x91B, "beat", 2}, {0x91C, "polka", 3}};
struct Worn {
  size_t at;
  const char* name;
};
constexpr Worn kPs3Worn[] = {{0x91D, "allegretto"}, {0x91E, "polka"}, {0x91F, "beat"}};
// The score piece marks, u32 bit n - 1 per piece (sub_232698), after six
// bytes of padding.
constexpr size_t kPs3Marks = 0x928;

using Bytes = std::vector<uint8_t>;
using Blocks = std::map<std::string, std::pair<size_t, size_t>>;

uint32_t U32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void PutU32(Bytes& out, uint32_t v) {
  for (int shift = 24; shift >= 0; shift -= 8)
    out.push_back(uint8_t(v >> shift));
}

std::string Hex(const uint8_t* p, size_t n) {
  static const char kDigits[] = "0123456789ABCDEF";
  std::string s;
  for (size_t i = 0; i < n; ++i) {
    s += kDigits[p[i] >> 4];
    s += kDigits[p[i] & 15];
  }
  return s;
}

Bytes ReadFile(const fs::path& path, size_t limit = SIZE_MAX) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return {};
  Bytes bytes;
  char buffer[1 << 16];
  while (bytes.size() < limit && file) {
    file.read(buffer, std::streamsize(std::min(sizeof(buffer), limit - bytes.size())));
    bytes.insert(bytes.end(), buffer, buffer + file.gcount());
  }
  return bytes;
}

bool WriteFile(const fs::path& path, const void* data, size_t size) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  return file && file.write(static_cast<const char*>(data), std::streamsize(size)) && file.flush();
}

std::string ReadText(const fs::path& path) {
  const Bytes bytes = ReadFile(path);
  return std::string(bytes.begin(), bytes.end());
}

// The profile folder holding the game's saves, else the SDK's default one.
fs::path TitleRoot(const fs::path& root) {
  std::error_code ec;
  if (fs::is_directory(root / kDefaultProfile / kTitleDir, ec))
    return root / kDefaultProfile / kTitleDir;
  std::vector<fs::path> found;
  for (const auto& entry : fs::directory_iterator(root, ec)) {
    std::error_code entry_ec;
    if (entry.is_directory(entry_ec) && fs::is_directory(entry.path() / kTitleDir / "00000001", entry_ec))
      found.push_back(entry.path() / kTitleDir);
  }
  std::sort(found.begin(), found.end());
  return found.empty() ? root / kDefaultProfile / kTitleDir : found.front();
}

std::string ContainerName(int slot) {
  char name[16];
  std::snprintf(name, sizeof(name), "savecontent%02d", slot);
  return name;
}

fs::path ContainerDir(const fs::path& root, int slot) {
  return TitleRoot(root) / "00000001" / ContainerName(slot);
}

// file_time_type's epoch is unspecified and clock_cast is not everywhere this builds.
std::string FormatModified(fs::file_time_type when) {
  const auto system = std::chrono::system_clock::now() +
                      std::chrono::duration_cast<std::chrono::system_clock::duration>(
                          when - fs::file_time_type::clock::now());
  const std::time_t t = std::chrono::system_clock::to_time_t(system);
  std::tm local{};
#if REX_PLATFORM_WIN32
  localtime_s(&local, &t);
#else
  localtime_r(&t, &local);
#endif
  char text[32];
  std::strftime(text, sizeof(text), "%Y/%m/%d %H:%M", &local);
  return text;
}

std::string RecordValue(const std::string& record, std::string_view key) {
  size_t at = 0;
  while (at < record.size()) {
    size_t end = record.find('\n', at);
    if (end == std::string::npos)
      end = record.size();
    std::string_view line(record.data() + at, end - at);
    if (line.starts_with(key) && line.size() > key.size() &&
        (line[key.size()] == ' ' || line[key.size()] == '=')) {
      line.remove_prefix(key.size());
      while (!line.empty() && (line.front() == ' ' || line.front() == '='))
        line.remove_prefix(1);
      while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
        line.remove_suffix(1);
      return std::string(line);
    }
    at = end + 1;
  }
  return {};
}

uint32_t PlaySeconds(const uint8_t* body) {
  return std::min(U32(body + 12) / kTicksPerSecond, kMaxPlaySeconds);
}

std::vector<uint8_t> ThumbnailRgba(const uint8_t* argb) {
  std::vector<uint8_t> rgba(kThumbSize);
  for (size_t i = 0; i < kThumbSize; i += 4) {
    rgba[i + 0] = argb[i + 1];
    rgba[i + 1] = argb[i + 2];
    rgba[i + 2] = argb[i + 3];
    rgba[i + 3] = 255;
  }
  return rgba;
}

// Splits a save body (no 360 wrapper) into named blocks, as offsets into it.
std::string ParseBody(const Bytes& body, bool ps3, Blocks& blocks) {
  if (body.size() < kHeaderSize + kThumbSize || U32(body.data()) != kHeaderSize + kThumbSize)
    return Tr("saves", "err_not_save");
  if (const uint32_t version = U32(body.data() + 16); version != (ps3 ? 3u : 2u))
    return Tr("saves", "err_version", std::to_string(version));
  size_t off = kHeaderSize + kThumbSize;
  const Block* layout = ps3 ? kPs3Blocks : k360Blocks;
  const size_t count = ps3 ? std::size(kPs3Blocks) : std::size(k360Blocks);
  for (size_t i = 0; i < count; ++i) {
    blocks[layout[i].name] = {off, layout[i].size};
    off += layout[i].size;
  }
  blocks["chars"] = {off, kChars * kCharSize};
  off += kChars * kCharSize;
  blocks["b100"] = {off, 100};
  if (off + 100 != body.size())
    return Tr("saves", "err_damaged");
  return {};
}

// sub_8223D4D8's check over "SAV\0" for a given key.
uint32_t CheckKey(uint32_t key) {
  uint64_t rem = 0;
  for (const uint8_t b : {'S', 'A', 'V', '\0'})
    rem = ((rem << 8) + b) % key;
  rem = (rem << 16) % key;
  uint64_t check = 0;
  if (rem) {
    for (int bit = 0; bit < 16; ++bit) {
      if (((check + rem) ^ key) & (1ull << bit))
        check |= 1ull << bit;
    }
  }
  return uint32_t(check);
}

Bytes Wrap360(const Bytes& body, uint32_t key = 42) {
  Bytes out{'S', 'A', 'V', '\0'};
  PutU32(out, 2);
  PutU32(out, key);
  PutU32(out, uint32_t(kWrapHead + body.size()));
  out.insert(out.end(), body.begin(), body.end());
  PutU32(out, CheckKey(key));
  return out;
}

std::string Convert360(const Bytes& data, PreparedSave& out) {
  if (data.size() < kWrapHead + 4)
    return Tr("saves", "err_not_save");
  const uint32_t version = U32(data.data() + 4);
  const uint32_t key = U32(data.data() + 8);
  const uint32_t end = U32(data.data() + 12);
  if (version != 2 || end != data.size() - 4)
    return Tr("saves", "err_damaged");
  if (key && U32(data.data() + end) != CheckKey(key))
    REXLOG_WARN("Saves: the 360 container check does not match");
  const Bytes body(data.begin() + kWrapHead, data.begin() + end);
  Blocks blocks;
  if (std::string error = ParseBody(body, false, blocks); !error.empty())
    return error;
  out.savedata = data;
  out.kind = "360";
  out.play_seconds = PlaySeconds(body.data());
  return {};
}

// The PS3 party block widens position[] and slotbytes[] to twelve and adds
// costume and camp menu state; what the 360 layout has no room for goes to reprise.txt.
std::string ConvertPs3(const Bytes& body, PreparedSave& out) {
  Blocks blocks;
  if (std::string error = ParseBody(body, true, blocks); !error.empty())
    return error;
  auto at = [&](const char* name) { return body.data() + blocks[name].first; };
  const uint8_t* p = at("party");
  Bytes party(2324, 0);
  std::memcpy(&party[0x000], p + 0x000, 0x820);
  std::memcpy(&party[0x820], p + 0x820, 0x28);  // position, characters 1..10
  std::memcpy(&party[0x848], p + 0x850, 0x0A);  // slotbytes 1..10
  std::memcpy(&party[0x852], p + 0x85C, 0x86);  // 3 bytes, Item Set, its scratch copy
  std::memcpy(&party[0x8D8], p + 0x8E4, 0x35);  // Item Set count and budget .. score piece Mark
  // The EBOOT never reads +0x918, +0x922..+0x927 or anything past +0x92C.

  const size_t split = kRetailChars * kStatsStride;
  Bytes converted;
  converted.reserve(k360Body);
  auto append = [&](const uint8_t* src, size_t n) { converted.insert(converted.end(), src, src + n); };
  append(body.data(), kHeaderSize);
  converted[19] = 2;  // the header's version
  append(body.data() + kHeaderSize, kThumbSize);
  converted.insert(converted.end(), 4, 0);
  append(party.data(), party.size());
  append(at("inventory"), 2048);
  append(at("stats_base"), split);
  append(at("stats_live"), split);
  append(at("map"), 4412);
  append(at("b80"), 80);
  append(at("chars"), kChars * kCharSize);
  append(at("b100"), 100);
  if (converted.size() != k360Body)
    return Tr("saves", "err_damaged");

  std::map<std::string, std::string> record{
      {"version", "1"},
      {"target", "ps3"},
      {"party.position", Hex(p + 0x848, 8)},
      {"party.slotbytes", Hex(p + 0x85A, 2)},
      {"party.stats_base", Hex(at("stats_base") + split, 576 - split)},
      {"party.stats_live", Hex(at("stats_live") + split, 576 - split)},
      {"ps3.camp_menu_flags", std::string(p[0x920] ? "1" : "0") + (p[0x921] ? "1" : "0")},
      {"score_pieces.marked", Hex(p + kPs3Marks, 4)},
  };
  for (const Unlock& unlock : kPs3Unlocks) {
    if (p[unlock.at])
      record["unlocked." + std::string(unlock.name) + ".ps3/" + std::to_string(unlock.variant)] = "1";
  }
  for (const Worn& worn : kPs3Worn) {
    if (p[worn.at] > 1)
      record["costume." + std::string(worn.name)] = "ps3/" + std::to_string(p[worn.at]);
  }
  out.record.clear();
  for (const auto& [key, value] : record)
    out.record += key + " = " + value + "\n";
  out.savedata = Wrap360(converted);
  out.kind = "ps3";
  out.play_seconds = PlaySeconds(converted.data());
  return {};
}

bool IsSaveFileName(const fs::path& path) {
  std::string name = path.filename().string();
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return name == "savedata.txt" || name == "savedata";
}

// The save inside an extracted archive: the shallowest, then the first by name.
fs::path FindSaveIn(const fs::path& dir) {
  std::vector<fs::path> found;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code entry_ec;
    if (it->is_regular_file(entry_ec) && IsSaveFileName(it->path()))
      found.push_back(it->path());
  }
  std::sort(found.begin(), found.end(), [](const fs::path& a, const fs::path& b) {
    const auto depth = [](const fs::path& p) { return std::distance(p.begin(), p.end()); };
    return depth(a) != depth(b) ? depth(a) < depth(b) : a < b;
  });
  return found.empty() ? fs::path() : found.front();
}

// A console's protected SAVEDATA is padded to whole AES blocks, a plain one never is.
bool IsEncryptedPs3(const Bytes& data) {
  return data.size() == (kPs3Size + 15) / 16 * 16;
}

// `dir` is where a 360 save's reprise.txt may sit.
std::string PrepareData(const Bytes& data, const fs::path& dir, PreparedSave& out) {
  std::string error;
  if (data.size() >= 4 && std::memcmp(data.data(), "SAV", 4) == 0)
    error = Convert360(data, out);
  else if (data.size() == kPs3Size)
    error = ConvertPs3(data, out);
  else
    return Tr("saves", "err_not_save");
  if (!error.empty())
    return error;
  if (out.kind == "ps3")
    return {};
  // A 360 save keeps the record it was written with, when it sits beside it.
  std::error_code ec;
  const fs::path record = dir / "reprise.txt";
  out.record = fs::is_regular_file(record, ec) ? ReadText(record) : std::string();
  if (!out.record.empty() && out.record.back() != '\n')
    out.record += '\n';
  if (RecordValue(out.record, "version").empty())
    out.record += "version = 1\n";
  // Older records name no target; only PS3 mode wrote the camp flags.
  std::string target = RecordValue(out.record, "target");
  if (target.empty()) {
    target = RecordValue(out.record, "ps3.camp_menu_flags").empty() ? "360" : "ps3";
    out.record += "target = " + target + "\n";
  }
  out.kind = target;
  return {};
}

std::string PrepareFrom(const fs::path& file, PreparedSave& out) {
  Bytes data = ReadFile(file);
  if (data.empty())
    return Tr("saves", "err_read");
  if (IsEncryptedPs3(data)) {
    std::error_code ec;
    const fs::path pfd = file.parent_path() / "PARAM.PFD";
    // Not beside it (a single picked file on Android): the caller asks for it.
    if (!fs::is_regular_file(pfd, ec)) {
      out.savedata = std::move(data);
      out.needs_pfd = true;
      return {};
    }
    if (!DecryptPs3ProtectedFile(ReadFile(pfd), "SAVEDATA", data))
      return Tr("saves", "err_wrong_pfd");
  }
  return PrepareData(data, file.parent_path(), out);
}

std::vector<uint8_t> ContentHeader(int slot) {
  std::vector<uint8_t> header(328, 0);
  header[3] = 1;
  header[7] = 1;
  char display[32];
  std::snprintf(display, sizeof(display), "Save File %02d", slot + 1);
  for (size_t i = 0; display[i]; ++i)
    header[8 + i * 2 + 1] = uint8_t(display[i]);
  const std::string name = ContainerName(slot);
  std::memcpy(&header[0x108], name.data(), name.size());
  for (int i = 0; i < 4; ++i)
    header[0x140 + i] = uint8_t(kTitleId >> (24 - 8 * i));
  return header;
}

bool WriteThumbnail(const fs::path& path, const uint8_t* argb) {
  std::vector<uint8_t> rgba = ThumbnailRgba(argb);
  SDL_Surface* surface = SDL_CreateSurfaceFrom(kSaveThumbWidth, kSaveThumbHeight,
                                               SDL_PIXELFORMAT_RGBA32, rgba.data(),
                                               kSaveThumbWidth * 4);
  if (!surface)
    return false;
  const bool ok = SDL_SavePNG(surface, rex::path_to_utf8(path).c_str());
  SDL_DestroySurface(surface);
  return ok;
}

}  // namespace

std::vector<SaveSlotInfo> ReadSaveSlots(const fs::path& user_data_root) {
  std::vector<SaveSlotInfo> slots(kSaveSlotCount);
  for (int i = 0; i < kSaveSlotCount; ++i) {
    const fs::path dir = ContainerDir(user_data_root, i);
    const fs::path file = dir / "savedata.txt";
    std::error_code ec;
    if (!fs::is_regular_file(file, ec))
      continue;
    SaveSlotInfo& slot = slots[size_t(i)];
    slot.used = true;
    if (const auto when = fs::last_write_time(file, ec); !ec)
      slot.modified = FormatModified(when);
    // Older records name no target; only PS3 mode wrote the camp flags.
    const std::string record = ReadText(dir / "reprise.txt");
    slot.target = RecordValue(record, "target");
    if (slot.target.empty())
      slot.target = RecordValue(record, "ps3.camp_menu_flags").empty() ? "360" : "ps3";
    const Bytes head = ReadFile(file, kWrapHead + kHeaderSize + kThumbSize);
    if (head.size() == kWrapHead + kHeaderSize + kThumbSize &&
        std::memcmp(head.data(), "SAV", 4) == 0 &&
        U32(head.data() + kWrapHead) == kHeaderSize + kThumbSize) {
      const uint8_t* body = head.data() + kWrapHead;
      slot.play_seconds = PlaySeconds(body);
      // sub_82241190 copies the location's low byte to +9, the chapter to +10
      // and the Encore counter to +20; sub_82243C68 reads them from there.
      slot.location = body[9];
      slot.chapter = body[10];
      slot.beaten = U32(body + 20) != 0 || slot.location == 1;
      slot.thumbnail = ThumbnailRgba(head.data() + kWrapHead + kHeaderSize);
    }
  }
  return slots;
}

std::string ExportSaveSlot(const fs::path& user_data_root, int slot, const fs::path& archive) {
  const fs::path dir = ContainerDir(user_data_root, slot);
  std::error_code ec;
  if (!fs::is_regular_file(dir / "savedata.txt", ec))
    return Tr("saves", "err_read");
  fs::remove(archive, ec);
  std::string error;
  if (!rex::filesystem::CreateZip(dir, archive, error))
    return error.empty() ? Tr("saves", "err_write", rex::path_to_utf8(archive)) : error;
  return {};
}

std::string PrepareSaveImport(const fs::path& user_data_root, const fs::path& source,
                              PreparedSave& out) {
  const Bytes magic = ReadFile(source, 4);
  if (magic.size() < 4)
    return Tr("saves", "err_read");
  if (std::memcmp(magic.data(), "PK\x03\x04", 4) != 0)
    return PrepareFrom(source, out);
  const fs::path staging = user_data_root / "cache" / "save_import";
  std::error_code ec;
  fs::remove_all(staging, ec);
  std::string error;
  if (!rex::filesystem::ExtractZip(source, staging, error)) {
    fs::remove_all(staging, ec);
    return error;
  }
  const fs::path found = FindSaveIn(staging);
  error = found.empty() ? Tr("saves", "err_no_save_in_zip") : PrepareFrom(found, out);
  fs::remove_all(staging, ec);
  return error;
}

std::string FinishEncryptedImport(const fs::path& pfd, PreparedSave& out) {
  Bytes data = std::move(out.savedata);
  out = {};
  const Bytes table = ReadFile(pfd);
  if (table.empty())
    return Tr("saves", "err_read");
  // Another save's PFD decrypts to noise, which fails the header check.
  if (!DecryptPs3ProtectedFile(table, "SAVEDATA", data) || data.size() != kPs3Size ||
      U32(data.data()) != kHeaderSize + kThumbSize)
    return Tr("saves", "err_wrong_pfd");
  return PrepareData(data, {}, out);
}

std::string WriteSaveSlot(const fs::path& user_data_root, int slot, const PreparedSave& save) {
  const fs::path dir = ContainerDir(user_data_root, slot);
  const fs::path header = dir.parent_path().parent_path() / "Headers" / "00000001" /
                          (ContainerName(slot) + ".header");
  // Written aside first, so a failure leaves the old save as it was.
  fs::path temp = dir;
  temp += ".importing";
  std::error_code ec;
  fs::remove_all(temp, ec);
  fs::create_directories(temp, ec);
  fs::create_directories(header.parent_path(), ec);
  const std::vector<uint8_t> head = ContentHeader(slot);
  if (!WriteFile(temp / "savedata.txt", save.savedata.data(), save.savedata.size()) ||
      !WriteFile(temp / "reprise.txt", save.record.data(), save.record.size())) {
    fs::remove_all(temp, ec);
    return Tr("saves", "err_write", rex::path_to_utf8(temp));
  }
  WriteThumbnail(temp / "__thumbnail.png", save.savedata.data() + kWrapHead + kHeaderSize);
  fs::remove_all(dir, ec);
  fs::rename(temp, dir, ec);
  if (ec) {
    fs::remove_all(temp, ec);
    return Tr("saves", "err_write", rex::path_to_utf8(dir));
  }
  if (!WriteFile(header, head.data(), head.size()))
    return Tr("saves", "err_write", rex::path_to_utf8(header));
  return {};
}

std::string DeleteSaveSlot(const fs::path& user_data_root, int slot) {
  const fs::path dir = ContainerDir(user_data_root, slot);
  const fs::path header = dir.parent_path().parent_path() / "Headers" / "00000001" /
                          (ContainerName(slot) + ".header");
  std::error_code ec;
  fs::remove_all(dir, ec);
  if (ec)
    return ec.message();
  fs::remove(header, ec);
  return {};
}

std::string SaveSlotExportName(int slot) {
  char name[48];
  std::snprintf(name, sizeof(name), "eternalsonata-save-%02d.zip", slot + 1);
  return name;
}

namespace {

const char* SaveText(const SaveTextTable* tables, int index) {
  const char* code = IntroLanguageCodeAt(IntroLanguage());
  size_t language = 1;  // English, for a language the game has no text in
  for (size_t i = 0; i < std::size(kSaveTextLanguages); ++i) {
    if (code && std::strcmp(code, kSaveTextLanguages[i]) == 0)
      language = i;
  }
  const SaveTextTable& table = tables[language];
  return index >= 0 && index < table.count ? table.text[index] : "";
}

}  // namespace

std::string SaveLocationName(int location) {
  return SaveText(kSaveLocations, location);
}

std::string SaveChapterName(int chapter) {
  return SaveText(kSaveChapters, chapter);
}

std::string SaveTextGlyphs() {
  std::string text;
  for (const auto* tables : {kSaveChapters, kSaveLocations}) {
    for (size_t i = 0; i < std::size(kSaveTextLanguages); ++i) {
      for (int j = 0; j < tables[i].count; ++j)
        text += tables[i].text[j];
    }
  }
  return text;
}

std::string FormatPlayTime(uint32_t seconds) {
  char text[16];
  std::snprintf(text, sizeof(text), "%02u:%02u:%02u", seconds / 3600, seconds / 60 % 60, seconds % 60);
  return text;
}

}  // namespace eternalsonata
