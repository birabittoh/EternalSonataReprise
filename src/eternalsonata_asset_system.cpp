// eternalsonata - Granular asset replacement.
//
// See eternalsonata_asset_system.h for how patches reach the guest, and
// eternalsonata_asset_api.h for the contract this implements. Text and textures
// are wired up; meshes and audio parse and report
// ETERNALSONATA_ASSET_UNSUPPORTED rather than pretending to work.

#include "eternalsonata_asset_system.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/devices/null_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/system/mod_plugin.h>

#include "eternalsonata_asset_api.h"
#include "eternalsonata_asset_container.h"
#include "eternalsonata_asset_texture.h"

namespace eternalsonata {
namespace {

using assets::EditStatus;
using assets::NormalizeGuestPath;

// Mod priority of a patch registered through the C ABI. Mods that decide at
// runtime cannot be attributed to a mods.toml slot from here, so they are
// treated as the highest priority: an explicit runtime decision beats a
// declarative file, and two of them fall back to first-registration-wins.
constexpr int kRuntimePriority = -1;

struct TextPatch {
  size_t blob = 0;
  std::string lang;  // fourcc + trailing space; empty = every language
  uint32_t id = 0;
  std::string value;  // already in the game's encoding
  bool allow_resize = false;
  std::string owner;
  int priority = 0;
};

struct RawPatch {
  std::filesystem::path host_file;
  std::string owner;
  int priority = 0;
};

// A texture arrives either as a file the host decodes at build time, or as
// pixels a mod handed over through the ABI. The file form is kept as a path
// rather than as pixels so that editing it and hitting Reload picks up the new
// image, which is what makes iteration on art bearable.
struct TexturePatch {
  std::string selector;  // chunk index, or its embedded name
  std::filesystem::path host_file;
  assets::SourceImage image;  // used when host_file is empty
  std::string owner;
  int priority = 0;
};

struct Container {
  std::map<std::string, TextPatch> text;  // key: canonical reference suffix
  std::map<std::string, TexturePatch> textures;
  std::optional<RawPatch> raw;
};

struct State {
  std::recursive_mutex mutex;
  rex::Runtime* runtime = nullptr;
  std::map<std::string, Container> containers;  // guest path -> patches
  std::vector<std::pair<uint32_t, std::pair<EternalSonataAssetProviderFn, void*>>> providers;
  uint32_t next_provider_token = 1;
  std::filesystem::path cache_dir;
  bool bound = false;
};

State& state() {
  static State s;
  return s;
}

// ---------------------------------------------------------------------------
// Reference parsing
// ---------------------------------------------------------------------------
struct Reference {
  std::string guest_path;
  std::string kind;      // "text", "tex", "mesh", "music", "sfx", or empty
  std::string selector;  // everything after the ':'
};

bool ParseReference(const char* ref, Reference* out) {
  if (!ref || !*ref)
    return false;
  std::string s(ref);
  const size_t hash = s.find('#');
  if (hash == std::string::npos) {
    out->guest_path = NormalizeGuestPath(s);
    out->kind.clear();
    return !out->guest_path.empty();
  }
  out->guest_path = NormalizeGuestPath(s.substr(0, hash));
  const std::string rest = s.substr(hash + 1);
  const size_t colon = rest.find(':');
  out->kind = rest.substr(0, colon);
  out->selector = colon == std::string::npos ? std::string() : rest.substr(colon + 1);
  return !out->guest_path.empty() && !out->kind.empty();
}

bool IsAllDigits(const std::string& s) {
  return !s.empty() &&
         std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit(uint8_t(c)) != 0; });
}

std::vector<std::string> Split(const std::string& s, char sep) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (true) {
    const size_t at = s.find(sep, start);
    parts.push_back(s.substr(start, at == std::string::npos ? at : at - start));
    if (at == std::string::npos)
      break;
    start = at + 1;
  }
  return parts;
}

// "[blob/]LANG/id" -> blob index, language fourcc (empty for ALL), string id.
bool ParseTextSelector(const std::string& selector, size_t* blob, std::string* lang, uint32_t* id) {
  auto parts = Split(selector, '/');
  if (parts.size() == 3) {
    if (!IsAllDigits(parts[0]))
      return false;
    *blob = size_t(std::stoul(parts[0]));
    parts.erase(parts.begin());
  } else if (parts.size() == 2) {
    *blob = 0;
  } else {
    return false;
  }
  if (!IsAllDigits(parts[1]))
    return false;
  *id = uint32_t(std::stoul(parts[1]));

  std::string want = parts[0];
  for (char& c : want)
    c = char(std::toupper(uint8_t(c)));
  if (want == "ALL") {
    lang->clear();
    return true;
  }
  want.resize(4, ' ');
  for (const char* known : assets::kBtxLanguages) {
    if (want == known) {
      *lang = want;
      return true;
    }
  }
  return false;
}

std::string TextKey(size_t blob, const std::string& lang, uint32_t id) {
  return "text:" + std::to_string(blob) + "/" + (lang.empty() ? "ALL" : lang.substr(0, 3)) + "/" +
         std::to_string(id);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
// Same reference twice: earlier mod wins, unless the later one forces. Both
// mods are named, and the loser's other patches in the container are untouched.
EternalSonataAssetResult RegisterText(const std::string& guest_path, const std::string& key,
                                      TextPatch patch, bool force) {
  auto& container = state().containers[guest_path];
  auto it = container.text.find(key);
  if (it != container.text.end()) {
    const bool wins = force || patch.priority < it->second.priority;
    REXLOG_WARN("assets: '{}' and '{}' both patch {}#{}; '{}' wins", it->second.owner, patch.owner,
                guest_path, key, wins ? patch.owner : it->second.owner);
    if (!wins)
      return ETERNALSONATA_ASSET_CONFLICT;
  }
  container.text[key] = std::move(patch);
  return ETERNALSONATA_ASSET_OK;
}

EternalSonataAssetResult RegisterTexture(const std::string& guest_path, TexturePatch patch,
                                         bool force) {
  auto& container = state().containers[guest_path];
  const std::string key = "tex:" + patch.selector;
  auto it = container.textures.find(key);
  if (it != container.textures.end()) {
    const bool wins = force || patch.priority < it->second.priority;
    REXLOG_WARN("assets: '{}' and '{}' both patch {}#{}; '{}' wins", it->second.owner, patch.owner,
                guest_path, key, wins ? patch.owner : it->second.owner);
    if (!wins)
      return ETERNALSONATA_ASSET_CONFLICT;
  }
  container.textures[key] = std::move(patch);
  return ETERNALSONATA_ASSET_OK;
}

// A chunk answers to its ordinal or to the name it carries, with or without the
// extension the artist's file had.
bool TextureMatches(const assets::TextureRef& ref, size_t index, const std::string& selector) {
  if (IsAllDigits(selector))
    return size_t(std::stoul(selector)) == index;
  if (ref.name.empty())
    return false;
  auto lower = [](std::string s) {
    for (char& c : s)
      c = char(std::tolower(uint8_t(c)));
    return s;
  };
  const std::string name = lower(ref.name);
  const std::string want = lower(selector);
  if (name == want)
    return true;
  const size_t dot = name.rfind('.');
  return dot != std::string::npos && name.compare(0, dot, want) == 0;
}

// ---------------------------------------------------------------------------
// Declarative discovery: mods/<name>/assets/
// ---------------------------------------------------------------------------
bool LooksLikeContainer(const std::string& component) {
  static const char* kExtensions[] = {".e", ".bmd", ".bop", ".csf", ".cxs"};
  for (const char* ext : kExtensions) {
    const size_t n = std::strlen(ext);
    if (component.size() > n && component.compare(component.size() - n, n, ext) == 0)
      return true;
  }
  return false;
}

std::string ReadFileText(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  // Trailing newlines are an artefact of the editor that wrote the file, not
  // part of the string the game draws.
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
    s.pop_back();
  if (s.size() >= 3 && uint8_t(s[0]) == 0xEF && uint8_t(s[1]) == 0xBB && uint8_t(s[2]) == 0xBF)
    s.erase(0, 3);
  return s;
}

// mods/<name>/assets.toml: per-reference flags. Hand-parsed rather than pulling
// in a TOML dependency for two keys.
std::map<std::string, bool> ReadAssetsToml(const std::filesystem::path& path) {
  std::map<std::string, bool> allow_resize;  // "" = the [defaults] value
  std::ifstream in(path);
  if (!in)
    return allow_resize;
  std::string line, section;
  while (std::getline(in, line)) {
    // A '#' inside quotes is part of a reference, not a comment.
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
      if (line[i] == '"')
        quoted = !quoted;
      else if (line[i] == '#' && !quoted) {
        line = line.substr(0, i);
        break;
      }
    }
    const size_t first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos)
      continue;
    line = line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
    if (line.empty())
      continue;
    if (line.front() == '[') {
      section = line.substr(1, line.size() - 2);
      if (section.size() >= 2 && section.front() == '"')
        section = section.substr(1, section.size() - 2);
      if (section == "defaults")
        section.clear();
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    std::string key = line.substr(0, eq);
    std::string value = line.substr(eq + 1);
    key.erase(key.find_last_not_of(" \t") + 1);
    const size_t vstart = value.find_first_not_of(" \t");
    if (vstart != std::string::npos)
      value = value.substr(vstart);
    if (key == "allow_resize")
      allow_resize[section] = value.rfind("true", 0) == 0;
  }
  return allow_resize;
}

bool AllowResizeFor(const std::map<std::string, bool>& table, const std::string& guest_path,
                    const std::string& kind_selector) {
  auto it = table.find(guest_path + "#" + kind_selector);
  if (it != table.end())
    return it->second;
  it = table.find("");
  return it != table.end() && it->second;
}

void AddTextFromFile(const std::string& mod_name, int priority, const std::string& guest_path,
                     const std::vector<std::string>& tail, const std::filesystem::path& file,
                     const std::map<std::string, bool>& toml) {
  // tail: [blob/]LANG/<id>.txt
  std::string selector;
  for (size_t i = 0; i < tail.size(); ++i) {
    if (i)
      selector += "/";
    selector += tail[i];
  }
  if (selector.size() > 4 && selector.compare(selector.size() - 4, 4, ".txt") == 0)
    selector.resize(selector.size() - 4);

  TextPatch patch;
  if (!ParseTextSelector(selector, &patch.blob, &patch.lang, &patch.id)) {
    REXLOG_WARN("assets: mod '{}' has an unreadable text path: {}", mod_name, file.string());
    return;
  }

  std::string error;
  if (!assets::TranscodeToGameEncoding(ReadFileText(file), patch.lang, patch.value, &error)) {
    REXLOG_ERROR("assets: mod '{}' text {}#text:{} rejected: {}", mod_name, guest_path, selector,
                 error);
    return;
  }
  patch.owner = mod_name;
  patch.priority = priority;
  patch.allow_resize = AllowResizeFor(toml, guest_path, "text:" + selector);
  RegisterText(guest_path, TextKey(patch.blob, patch.lang, patch.id), std::move(patch), false);
}

std::vector<std::string> ParseCsvRow(const std::string& line) {
  std::vector<std::string> fields;
  std::string field;
  bool quoted = false;
  for (size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (quoted) {
      if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') {
        field.push_back('"');
        ++i;
      } else if (c == '"') {
        quoted = false;
      } else {
        field.push_back(c);
      }
    } else if (c == '"') {
      quoted = true;
    } else if (c == ',') {
      fields.push_back(field);
      field.clear();
    } else if (c != '\r') {
      field.push_back(c);
    }
  }
  fields.push_back(field);
  return fields;
}

// assets/text/<LANG>.csv: file,blob,id,text. One row per string, which is what
// a whole translation wants instead of thousands of .txt files.
void AddTextTable(const std::string& mod_name, int priority, const std::string& lang_folder,
                  const std::filesystem::path& file, const std::map<std::string, bool>& toml) {
  std::string lang = lang_folder;
  for (char& c : lang)
    c = char(std::toupper(uint8_t(c)));
  if (lang != "ALL")
    lang.resize(4, ' ');
  else
    lang.clear();

  std::ifstream in(file);
  if (!in) {
    REXLOG_WARN("assets: mod '{}' could not read {}", mod_name, file.string());
    return;
  }
  std::string line;
  size_t row = 0, added = 0;
  while (std::getline(in, line)) {
    ++row;
    if (line.empty() || line[0] == '#')
      continue;
    auto fields = ParseCsvRow(line);
    if (fields.size() < 4)
      continue;
    if (row == 1 && fields[0] == "file")
      continue;  // header
    TextPatch patch;
    patch.blob = fields[1].empty() ? 0 : size_t(std::strtoul(fields[1].c_str(), nullptr, 10));
    patch.lang = lang;
    if (!IsAllDigits(fields[2])) {
      REXLOG_WARN("assets: mod '{}' {}:{} has a non-numeric string id", mod_name, file.string(),
                  row);
      continue;
    }
    patch.id = uint32_t(std::stoul(fields[2]));
    std::string error;
    if (!assets::TranscodeToGameEncoding(fields[3], patch.lang, patch.value, &error)) {
      REXLOG_ERROR("assets: mod '{}' {}:{} rejected: {}", mod_name, file.string(), row, error);
      continue;
    }
    const std::string guest_path = NormalizeGuestPath(fields[0]);
    patch.owner = mod_name;
    patch.priority = priority;
    const std::string key = TextKey(patch.blob, patch.lang, patch.id);
    patch.allow_resize = AllowResizeFor(toml, guest_path, key);
    if (RegisterText(guest_path, key, std::move(patch), false) == ETERNALSONATA_ASSET_OK)
      ++added;
  }
  REXLOG_INFO("assets: mod '{}' contributed {} strings from {}", mod_name, added,
              file.filename().string());
}

void ScanModAssets(const std::string& mod_name, int priority,
                   const std::filesystem::path& mod_root) {
  const std::filesystem::path assets_root = mod_root / "assets";
  std::error_code ec;
  if (!std::filesystem::is_directory(assets_root, ec))
    return;
  const auto toml = ReadAssetsToml(mod_root / "assets.toml");

  for (auto it = std::filesystem::recursive_directory_iterator(assets_root, ec);
       it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    if (ec)
      break;
    if (!it->is_regular_file(ec))
      continue;
    const auto relative = std::filesystem::relative(it->path(), assets_root, ec);
    if (ec)
      continue;

    std::vector<std::string> parts;
    for (const auto& component : relative)
      parts.push_back(component.string());
    if (parts.empty())
      continue;

    // The path under assets/ IS the reference: <container>/<kind>/<selector>,
    // or a bare guest path for a whole-file replacement.
    size_t split = parts.size();
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
      if (LooksLikeContainer(parts[i])) {
        split = i + 1;
        break;
      }
    }
    if (split == parts.size()) {
      if (parts.size() == 2 && parts[0] == "text") {
        AddTextTable(mod_name, priority, relative.stem().string(), it->path(), toml);
        continue;
      }
      std::string guest_path;
      for (size_t i = 0; i < parts.size(); ++i)
        guest_path += (i ? "/" : "") + parts[i];
      guest_path = NormalizeGuestPath(guest_path);
      auto& container = state().containers[guest_path];
      if (container.raw && container.raw->priority <= priority) {
        REXLOG_WARN("assets: '{}' already replaces {} whole; '{}' loses", container.raw->owner,
                    guest_path, mod_name);
        continue;
      }
      container.raw = RawPatch{it->path(), mod_name, priority};
      continue;
    }

    std::string guest_path;
    for (size_t i = 0; i < split; ++i)
      guest_path += (i ? "/" : "") + parts[i];
    guest_path = NormalizeGuestPath(guest_path);

    const std::string& kind = parts[split];
    std::vector<std::string> tail(parts.begin() + ptrdiff_t(split) + 1, parts.end());
    if (kind == "text" && !tail.empty()) {
      AddTextFromFile(mod_name, priority, guest_path, tail, it->path(), toml);
    } else if ((kind == "textures" || kind == "tex") && tail.size() == 1) {
      // assets/<container>/tex/<chunk name or index>.png
      TexturePatch patch;
      patch.selector = it->path().stem().string();
      patch.host_file = it->path();
      patch.owner = mod_name;
      patch.priority = priority;
      RegisterTexture(guest_path, std::move(patch), false);
    } else {
      REXLOG_WARN("assets: mod '{}' ships {} for {}, which this build cannot patch yet", mod_name,
                  kind, guest_path);
    }
  }
}

// ---------------------------------------------------------------------------
// Building the patched images
// ---------------------------------------------------------------------------
uint64_t HashUpdate(uint64_t h, std::string_view s) {
  for (char c : s) {
    h ^= uint8_t(c);
    h *= 0x100000001B3ull;
  }
  return h;
}

// Keyed on the mod list plus every patch's bytes, so a full translation pays
// the decode once per install rather than once per launch.
uint64_t CacheKey(rex::Runtime* runtime) {
  uint64_t h = 0xCBF29CE484222325ull;
  h = HashUpdate(h, "v1");
  for (const auto& mod : runtime->EnabledModsInfo()) {
    h = HashUpdate(h, mod.folder_name);
    h = HashUpdate(h, mod.version);
  }
  for (const auto& [path, container] : state().containers) {
    h = HashUpdate(h, path);
    for (const auto& [key, patch] : container.text) {
      h = HashUpdate(h, key);
      h = HashUpdate(h, patch.value);
      h = HashUpdate(h, patch.allow_resize ? "r" : "-");
    }
    auto hash_file = [&h](const std::filesystem::path& file) {
      std::error_code ec;
      h = HashUpdate(h, file.string());
      h = HashUpdate(h, std::to_string(std::filesystem::file_size(file, ec)));
      const auto when = std::filesystem::last_write_time(file, ec);
      h = HashUpdate(h, std::to_string(when.time_since_epoch().count()));
    };
    for (const auto& [key, patch] : container.textures) {
      h = HashUpdate(h, key);
      if (patch.host_file.empty())
        h = HashUpdate(h, std::string(reinterpret_cast<const char*>(patch.image.pixels.data()),
                                      patch.image.pixels.size()));
      else
        hash_file(patch.host_file);
    }
    if (container.raw)
      hash_file(container.raw->host_file);
  }
  return h;
}

bool ReadWholeFile(const std::filesystem::path& path, std::vector<uint8_t>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

// A mod's game/ folder is the base image every granular patch applies on top
// of, so look through the overlay roots before the shipped file.
std::filesystem::path ResolveBaseFile(rex::Runtime* runtime, const std::string& guest_path) {
  std::error_code ec;
  for (const auto& root : runtime->ModOverlayRoots("game")) {
    const auto candidate = root / std::filesystem::path(guest_path);
    if (std::filesystem::is_regular_file(candidate, ec))
      return candidate;
  }
  return runtime->game_data_root() / std::filesystem::path(guest_path);
}

// The one value the TOC writer consumes: patched bytes and the record they must
// be served with are produced together, and there is no other way to get either.
struct PatchedContainer {
  std::vector<uint8_t> bytes;
  std::string guest_path;
  size_t patches_applied = 0;
};

// Runs after the text pass, on the bytes that pass produced: a resizing text
// edit moves every chunk after it, and an NTX2's pixel data is found by 4 KB
// alignment from the chunk's own offset, so the chunks have to be located in
// the image that will actually be served.
void ApplyTexturePatches(const std::string& guest_path, const Container& container,
                         PatchedContainer& result) {
  if (container.textures.empty())
    return;

  const auto textures = assets::FindTextures(result.bytes);
  for (const auto& [key, patch] : container.textures) {
    assets::SourceImage image;
    std::string error;
    if (patch.host_file.empty()) {
      image = patch.image;
    } else if (!assets::LoadSourceImage(patch.host_file, image, &error)) {
      REXLOG_ERROR("assets: mod '{}' texture {} {}", patch.owner, patch.host_file.string(), error);
      continue;
    }

    bool matched = false;
    for (size_t i = 0; i < textures.size(); ++i) {
      if (!TextureMatches(textures[i], i, patch.selector))
        continue;
      matched = true;
      const EditStatus status =
          assets::ApplyTextureEdit(result.bytes, textures[i], image, &error);
      if (status == EditStatus::kOk) {
        ++result.patches_applied;
        REXLOG_INFO("assets: mod '{}' replaced {}#{} ({}x{} {})", patch.owner, guest_path, key,
                    textures[i].width, textures[i].height,
                    assets::TextureFormatName(textures[i].format));
      } else {
        REXLOG_WARN("assets: mod '{}' texture {}#{} {}", patch.owner, guest_path, key, error);
      }
      break;
    }
    if (!matched) {
      REXLOG_WARN("assets: mod '{}' patches {}#{}, which the container does not have", patch.owner,
                  guest_path, key);
    }
  }
}

std::optional<PatchedContainer> BuildContainer(rex::Runtime* runtime, const assets::Toc& toc,
                                               const std::string& guest_path,
                                               const Container& container) {
  PatchedContainer result;
  result.guest_path = guest_path;

  if (container.raw) {
    if (!ReadWholeFile(container.raw->host_file, result.bytes)) {
      REXLOG_ERROR("assets: could not read {}", container.raw->host_file.string());
      return std::nullopt;
    }
    result.patches_applied = 1;
    if (container.text.empty() && container.textures.empty())
      return result;
  } else {
    const auto base = ResolveBaseFile(runtime, guest_path);
    std::vector<uint8_t> encoded;
    if (!ReadWholeFile(base, encoded)) {
      REXLOG_ERROR("assets: {} names {}, which is not in the game data", guest_path, base.string());
      return std::nullopt;
    }
    const assets::TocEntry* entry = toc.Find(guest_path);
    if (entry && entry->flag != 0) {
      if (!assets::DecodeAsset(encoded.data(), encoded.size(), entry->size, entry->flag,
                               result.bytes)) {
        REXLOG_ERROR("assets: {} did not decode to its TOC size ({} bytes, flag {})", guest_path,
                     entry->size, entry->flag);
        return std::nullopt;
      }
    } else {
      result.bytes = std::move(encoded);
    }
  }

  std::vector<assets::TextEdit> edits;
  std::vector<const TextPatch*> owners;
  edits.reserve(container.text.size());
  for (const auto& [key, patch] : container.text) {
    assets::TextEdit edit;
    edit.blob = patch.blob;
    edit.lang = patch.lang;
    edit.id = patch.id;
    edit.value = patch.value;
    edit.allow_resize = patch.allow_resize;
    edits.push_back(std::move(edit));
    owners.push_back(&patch);
  }

  assets::ApplyTextEdits(result.bytes, edits);
  for (size_t i = 0; i < edits.size(); ++i) {
    switch (edits[i].status) {
      case EditStatus::kOk:
        ++result.patches_applied;
        break;
      case EditStatus::kNotFound:
        REXLOG_WARN("assets: mod '{}' patches {}#text:{}/{}, which the container does not have",
                    owners[i]->owner, guest_path, owners[i]->lang, owners[i]->id);
        break;
      case EditStatus::kTooLarge:
        REXLOG_WARN(
            "assets: mod '{}' text {}#text:{}/{} does not fit and did not ask for allow_resize",
            owners[i]->owner, guest_path, owners[i]->lang, owners[i]->id);
        break;
      case EditStatus::kBadData:
        REXLOG_WARN("assets: mod '{}' patch for {} was rejected as malformed", owners[i]->owner,
                    guest_path);
        break;
    }
  }

  ApplyTexturePatches(guest_path, container, result);
  return result;
}

bool WriteWholeFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out)
    return false;
  out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
  return out.good();
}

// Builds every patched container plus the index.vmtoc that describes them, into
// one cache generation. The TOC is written in the same pass as the containers:
// a half-updated cache directory is exactly the state the vmtoc invariant
// exists to prevent.
bool BuildCache(rex::Runtime* runtime, const std::filesystem::path& dir) {
  assets::Toc toc;
  bool have_toc = false;
  for (const auto& root : runtime->ModOverlayRoots("game")) {
    if ((have_toc = toc.Load(root / "index.vmtoc")))
      break;
  }
  if (!have_toc && !toc.Load(runtime->game_data_root() / "index.vmtoc")) {
    REXLOG_ERROR("assets: no index.vmtoc, so nothing can be patched");
    return false;
  }

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);

  size_t built = 0;
  for (const auto& [guest_path, container] : state().containers) {
    // Last chance for a lazy provider to register patches for this container.
    for (auto& entry : state().providers)
      entry.second.first(guest_path.c_str(), entry.second.second);
    if (auto* registry = runtime->mod_registry()) {
      rex::system::ModRegistry::EventPayload payload;
      payload.bytes = {reinterpret_cast<const uint8_t*>(guest_path.data()), guest_path.size()};
      registry->Publish(ETERNALSONATA_ASSET_EVENT_LOADING, payload);
    }

    auto patched = BuildContainer(runtime, toc, guest_path, container);
    if (!patched || patched->patches_applied == 0)
      continue;
    if (!WriteWholeFile(dir / std::filesystem::path(guest_path), patched->bytes)) {
      REXLOG_ERROR("assets: could not write the patched {}", guest_path);
      continue;
    }
    // The record and the bytes are one unit: this is the only writer of the
    // served TOC, and it only ever runs on what BuildContainer returned.
    if (!toc.SetStored(guest_path, uint32_t(patched->bytes.size()))) {
      REXLOG_WARN("assets: {} has no TOC record; it will load raw, sized by the file itself",
                  guest_path);
    }
    ++built;
    REXLOG_INFO("assets: patched {} ({} patches, {} bytes)", guest_path, patched->patches_applied,
                patched->bytes.size());
    if (auto* registry = runtime->mod_registry()) {
      rex::system::ModRegistry::EventPayload payload;
      payload.u64 = patched->patches_applied;
      payload.f64 = double(patched->bytes.size());
      payload.bytes = {reinterpret_cast<const uint8_t*>(guest_path.data()), guest_path.size()};
      registry->Publish(ETERNALSONATA_ASSET_EVENT_PATCHED, payload);
    }
  }

  if (!built) {
    std::filesystem::remove_all(dir, ec);
    return false;
  }
  if (!WriteWholeFile(dir / "index.vmtoc", toc.bytes())) {
    REXLOG_ERROR("assets: could not write the patched index.vmtoc, so nothing will be served");
    std::filesystem::remove_all(dir, ec);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Serving: remount the game partition with the cache ahead of the mods
// ---------------------------------------------------------------------------
constexpr const char* kPartitionMount = "\\Device\\Harddisk0\\Partition1";
constexpr const char* kNullMount = "\\Device\\Harddisk0";

bool Remount(rex::Runtime* runtime, const std::filesystem::path& cache_dir) {
  auto* vfs = runtime->file_system();
  if (!vfs)
    return false;

  std::vector<std::filesystem::path> roots{cache_dir};
  for (auto& root : runtime->ModOverlayRoots("game"))
    roots.push_back(std::move(root));

  // The null device's mount path is a prefix of the partition's, and the VFS
  // picks the first device whose mount path matches, so both have to be
  // re-registered in the original order.
  vfs->UnregisterDevice(kPartitionMount);
  vfs->UnregisterDevice(kNullMount);

  auto device = std::make_unique<rex::filesystem::HostPathDevice>(
      kPartitionMount, std::filesystem::absolute(runtime->game_data_root()),
      !REXCVAR_GET(allow_game_relative_writes));
  device->set_overlay_roots(std::move(roots));
  if (!device->Initialize() || !vfs->RegisterDevice(std::move(device))) {
    REXLOG_ERROR("assets: could not remount the game partition; patches will not be served");
    return false;
  }

  auto null_device = std::make_unique<rex::filesystem::NullDevice>(
      kNullMount, std::initializer_list<std::string>{std::string("\\Partition0"),
                                                     std::string("\\Cache0"),
                                                     std::string("\\Cache1")});
  if (null_device->Initialize())
    vfs->RegisterDevice(std::move(null_device));
  return true;
}

// Collect, build and serve. Also the body of EternalSonataInvalidateAsset.
void RebuildAndServe() {
  State& s = state();
  if (!s.runtime)
    return;
  const uint64_t key = CacheKey(s.runtime);
  char name[17];
  snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(key));

  const auto root = s.runtime->user_data_root() / "cache" / "patched_assets";
  const auto dir = root / name;
  std::error_code ec;
  const bool cached = std::filesystem::is_regular_file(dir / "index.vmtoc", ec);
  if (!cached && !BuildCache(s.runtime, dir))
    return;
  if (cached)
    REXLOG_INFO("assets: reusing the patched containers in {}", dir.string());

  // One generation is all that is ever served; the rest are last launch's.
  for (auto it = std::filesystem::directory_iterator(root, ec);
       it != std::filesystem::directory_iterator(); it.increment(ec)) {
    if (ec)
      break;
    if (it->path() != dir)
      std::filesystem::remove_all(it->path(), ec);
  }

  if (Remount(s.runtime, dir))
    s.cache_dir = dir;
}

// ---------------------------------------------------------------------------
// Reading the shipped asset back (GetText / enumerate)
// ---------------------------------------------------------------------------
bool LoadDecodedContainer(const std::string& guest_path, std::vector<uint8_t>& out) {
  State& s = state();
  if (!s.runtime)
    return false;
  assets::Toc toc;
  if (!toc.Load(s.runtime->game_data_root() / "index.vmtoc"))
    return false;
  std::vector<uint8_t> encoded;
  if (!ReadWholeFile(ResolveBaseFile(s.runtime, guest_path), encoded))
    return false;
  const assets::TocEntry* entry = toc.Find(guest_path);
  if (!entry || entry->flag == 0) {
    out = std::move(encoded);
    return true;
  }
  return assets::DecodeAsset(encoded.data(), encoded.size(), entry->size, entry->flag, out);
}

}  // namespace

void BindAssetSystem(rex::Runtime* runtime) {
  State& s = state();
  std::lock_guard<std::recursive_mutex> lock(s.mutex);
  s.runtime = runtime;

  int priority = 0;
  for (const auto& mod : runtime->EnabledModsInfo())
    ScanModAssets(mod.folder_name, priority++, mod.mod_root);

  s.bound = true;
  if (s.containers.empty())
    return;
  RebuildAndServe();
}

}  // namespace eternalsonata

// ---------------------------------------------------------------------------
// The mod-facing C ABI (src/eternalsonata_asset_api.h)
// ---------------------------------------------------------------------------
using namespace eternalsonata;

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t EternalSonataAssetAbiVersion(void) {
  return ETERNALSONATA_ASSET_ABI_VERSION;
}

extern "C" REX_MOD_PLUGIN_EXPORT EternalSonataAssetResult EternalSonataSetText(const char* ref,
                                                                              const char* text,
                                                                              uint32_t flags) {
  Reference parsed;
  if (!ParseReference(ref, &parsed) || parsed.kind != "text" || !text)
    return ETERNALSONATA_ASSET_BAD_REF;

  TextPatch patch;
  if (!ParseTextSelector(parsed.selector, &patch.blob, &patch.lang, &patch.id))
    return ETERNALSONATA_ASSET_BAD_REF;
  if (flags & ETERNALSONATA_ASSET_ALL_LANGUAGES)
    patch.lang.clear();
  // A mod hands us the game's own encoding, one glyph per byte, so this is
  // stored verbatim; the transcode is a tooling-boundary concern.
  patch.value = text;
  patch.allow_resize = (flags & ETERNALSONATA_ASSET_ALLOW_RESIZE) != 0;
  patch.owner = "runtime";
  patch.priority = kRuntimePriority;

  std::lock_guard<std::recursive_mutex> lock(state().mutex);
  return RegisterText(parsed.guest_path, TextKey(patch.blob, patch.lang, patch.id),
                      std::move(patch), (flags & ETERNALSONATA_ASSET_FORCE) != 0);
}

extern "C" REX_MOD_PLUGIN_EXPORT EternalSonataAssetResult EternalSonataGetText(
    const char* ref, char* buffer, uint32_t capacity, uint32_t* out_length) {
  Reference parsed;
  if (!ParseReference(ref, &parsed) || parsed.kind != "text")
    return ETERNALSONATA_ASSET_BAD_REF;
  size_t blob = 0;
  std::string lang;
  uint32_t id = 0;
  if (!ParseTextSelector(parsed.selector, &blob, &lang, &id) || lang.empty())
    return ETERNALSONATA_ASSET_BAD_REF;

  std::lock_guard<std::recursive_mutex> lock(state().mutex);
  std::vector<uint8_t> data;
  if (!LoadDecodedContainer(parsed.guest_path, data))
    return ETERNALSONATA_ASSET_IO_ERROR;
  const auto blobs = assets::FindBtxBlobs(data);
  if (blob >= blobs.size())
    return ETERNALSONATA_ASSET_NOT_FOUND;
  const assets::BtxLang* block = blobs[blob].Find(lang);
  if (!block)
    return ETERNALSONATA_ASSET_NOT_FOUND;
  auto it = block->entries.find(id);
  if (it == block->entries.end())
    return ETERNALSONATA_ASSET_NOT_FOUND;

  if (out_length)
    *out_length = uint32_t(it->second.size());
  if (buffer && capacity) {
    const size_t n = std::min<size_t>(it->second.size(), capacity - 1);
    std::memcpy(buffer, it->second.data(), n);
    buffer[n] = '\0';
  }
  return ETERNALSONATA_ASSET_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT EternalSonataAssetResult EternalSonataEnumerateAssets(
    const char* guest_path, EternalSonataAssetVisitorFn visitor, void* user) {
  if (!guest_path || !visitor)
    return ETERNALSONATA_ASSET_BAD_REF;
  std::string path = NormalizeGuestPath(guest_path);
  if (!path.empty() && path.back() == '*')
    return ETERNALSONATA_ASSET_UNSUPPORTED;  // subtree walks need the browser

  std::lock_guard<std::recursive_mutex> lock(state().mutex);
  std::vector<uint8_t> data;
  if (!LoadDecodedContainer(path, data))
    return ETERNALSONATA_ASSET_IO_ERROR;

  const auto blobs = assets::FindBtxBlobs(data);
  for (size_t bi = 0; bi < blobs.size(); ++bi) {
    for (const auto& lang : blobs[bi].langs) {
      for (const auto& [id, value] : lang.entries) {
        const std::string ref = path + "#text:" + std::to_string(bi) + "/" +
                                lang.fourcc.substr(0, 3) + "/" + std::to_string(id);
        if (!visitor(ref.c_str(), ETERNALSONATA_ASSET_KIND_TEXT, value.c_str(),
                     uint32_t(value.size()), user))
          return ETERNALSONATA_ASSET_OK;
      }
    }
  }

  const auto textures = assets::FindTextures(data);
  for (size_t i = 0; i < textures.size(); ++i) {
    // The ordinal, because it is the one selector every chunk has; the name is
    // reported alongside and addresses the same chunk when it has one.
    const std::string ref = path + "#tex:" + std::to_string(i);
    if (!visitor(ref.c_str(), ETERNALSONATA_ASSET_KIND_TEXTURE, textures[i].name.c_str(),
                 textures[i].chunk_size, user))
      return ETERNALSONATA_ASSET_OK;
  }
  return ETERNALSONATA_ASSET_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT EternalSonataAssetResult EternalSonataClearAssetPatch(
    const char* ref) {
  Reference parsed;
  if (!ParseReference(ref, &parsed) || (parsed.kind != "text" && parsed.kind != "tex"))
    return ETERNALSONATA_ASSET_BAD_REF;
  if (parsed.kind == "tex") {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    auto container = state().containers.find(parsed.guest_path);
    if (container == state().containers.end())
      return ETERNALSONATA_ASSET_NOT_FOUND;
    return container->second.textures.erase("tex:" + parsed.selector)
               ? ETERNALSONATA_ASSET_OK
               : ETERNALSONATA_ASSET_NOT_FOUND;
  }
  size_t blob = 0;
  std::string lang;
  uint32_t id = 0;
  if (!ParseTextSelector(parsed.selector, &blob, &lang, &id))
    return ETERNALSONATA_ASSET_BAD_REF;

  std::lock_guard<std::recursive_mutex> lock(state().mutex);
  auto it = state().containers.find(parsed.guest_path);
  if (it == state().containers.end())
    return ETERNALSONATA_ASSET_NOT_FOUND;
  return it->second.text.erase(TextKey(blob, lang, id)) ? ETERNALSONATA_ASSET_OK
                                                        : ETERNALSONATA_ASSET_NOT_FOUND;
}

extern "C" REX_MOD_PLUGIN_EXPORT void EternalSonataInvalidateAsset(const char* guest_path) {
  (void)guest_path;  // one cache generation, so any invalidation rebuilds it
  std::lock_guard<std::recursive_mutex> lock(state().mutex);
  if (state().bound)
    RebuildAndServe();
}

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t EternalSonataRegisterAssetProvider(
    EternalSonataAssetProviderFn provider, void* user) {
  if (!provider)
    return 0;
  std::lock_guard<std::recursive_mutex> lock(state().mutex);
  const uint32_t token = state().next_provider_token++;
  state().providers.push_back({token, {provider, user}});
  return token;
}

extern "C" REX_MOD_PLUGIN_EXPORT void EternalSonataUnregisterAssetProvider(uint32_t token) {
  std::lock_guard<std::recursive_mutex> lock(state().mutex);
  auto& providers = state().providers;
  providers.erase(std::remove_if(providers.begin(), providers.end(),
                                 [token](const auto& p) { return p.first == token; }),
                  providers.end());
}

extern "C" REX_MOD_PLUGIN_EXPORT EternalSonataAssetResult EternalSonataReplaceFile(
    const char* guest_path, const uint8_t* bytes, uint32_t size, uint32_t flags) {
  (void)guest_path;
  (void)bytes;
  (void)size;
  (void)flags;
  return ETERNALSONATA_ASSET_UNSUPPORTED;
}

extern "C" REX_MOD_PLUGIN_EXPORT EternalSonataAssetResult EternalSonataReplaceTexture(
    const char* ref, const EternalSonataImage* image, uint32_t flags) {
  Reference parsed;
  if (!ParseReference(ref, &parsed) || parsed.kind != "tex" || parsed.selector.empty() || !image ||
      !image->pixels || !image->width || !image->height) {
    return ETERNALSONATA_ASSET_BAD_REF;
  }
  // Mip levels are generated from level 0 to match the chain the chunk already
  // has, so a caller's own chain has nothing to be spliced into.
  if (image->mip_levels > 1)
    return ETERNALSONATA_ASSET_UNSUPPORTED;

  TexturePatch patch;
  patch.selector = parsed.selector;
  patch.image.width = image->width;
  patch.image.height = image->height;
  patch.image.pixels.assign(image->pixels,
                            image->pixels + size_t(image->width) * image->height * 4);
  patch.owner = "runtime";
  patch.priority = kRuntimePriority;

  std::lock_guard<std::recursive_mutex> lock(state().mutex);
  return RegisterTexture(parsed.guest_path, std::move(patch),
                         (flags & ETERNALSONATA_ASSET_FORCE) != 0);
}

extern "C" REX_MOD_PLUGIN_EXPORT EternalSonataAssetResult EternalSonataReplaceTextureFromFile(
    const char* ref, const char* host_path, uint32_t flags) {
  Reference parsed;
  if (!ParseReference(ref, &parsed) || parsed.kind != "tex" || parsed.selector.empty() ||
      !host_path) {
    return ETERNALSONATA_ASSET_BAD_REF;
  }
  std::error_code ec;
  if (!std::filesystem::is_regular_file(host_path, ec))
    return ETERNALSONATA_ASSET_IO_ERROR;

  TexturePatch patch;
  patch.selector = parsed.selector;
  patch.host_file = host_path;
  patch.owner = "runtime";
  patch.priority = kRuntimePriority;

  std::lock_guard<std::recursive_mutex> lock(state().mutex);
  return RegisterTexture(parsed.guest_path, std::move(patch),
                         (flags & ETERNALSONATA_ASSET_FORCE) != 0);
}

extern "C" REX_MOD_PLUGIN_EXPORT EternalSonataAssetResult EternalSonataReplaceMesh(
    const char* ref, const EternalSonataMesh* mesh, uint32_t flags) {
  (void)ref;
  (void)mesh;
  (void)flags;
  return ETERNALSONATA_ASSET_UNSUPPORTED;
}

extern "C" REX_MOD_PLUGIN_EXPORT EternalSonataAssetResult EternalSonataReplaceMeshFromFile(
    const char* ref, const char* host_path, uint32_t flags) {
  (void)ref;
  (void)host_path;
  (void)flags;
  return ETERNALSONATA_ASSET_UNSUPPORTED;
}

extern "C" REX_MOD_PLUGIN_EXPORT EternalSonataAssetResult EternalSonataReplaceAudio(
    const char* ref, const EternalSonataAudio* audio, uint32_t flags) {
  (void)ref;
  (void)audio;
  (void)flags;
  return ETERNALSONATA_ASSET_UNSUPPORTED;
}

extern "C" REX_MOD_PLUGIN_EXPORT EternalSonataAssetResult EternalSonataReplaceAudioFromFile(
    const char* ref, const char* host_path, uint32_t flags) {
  (void)ref;
  (void)host_path;
  (void)flags;
  return ETERNALSONATA_ASSET_UNSUPPORTED;
}
