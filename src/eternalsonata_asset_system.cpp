// eternalsonata - Granular asset replacement.
//
// See eternalsonata_asset_system.h for how patches reach the guest, and
// eternalsonata_asset_api.h for the contract this implements. Text, textures,
// model chunks, audio, and lip sync are wired up.

#include "eternalsonata_asset_system.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/devices/null_device.h>
#include <rex/audio/audio_system.h>
#include <rex/audio/xma/decoder.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/system/mod_plugin.h>

#include "eternalsonata_asset_api.h"
#include "eternalsonata_asset_container.h"
#include "eternalsonata_asset_mesh.h"
#include "eternalsonata_asset_texture.h"
#include "settings.h"

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
  std::vector<uint8_t> bytes;
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

struct MeshPatch {
  std::string selector;
  std::filesystem::path host_file;
  std::vector<EternalSonataVertex> vertices;
  std::vector<uint32_t> indices;
  std::vector<EternalSonataFaceSection> sections;
  bool allow_resize = false;
  std::string owner;
  int priority = 0;
};

struct ModelChunkPatch {
  std::string selector;
  std::filesystem::path host_file;
  std::vector<uint8_t> bytes;
  bool allow_resize = false;
  std::string owner;
  int priority = 0;
};

struct AudioPatch {
  std::string kind;
  std::string selector;
  std::filesystem::path host_file;
  std::vector<int16_t> samples;
  uint32_t frame_count = 0;
  uint32_t sample_rate = 0;
  uint16_t channels = 0;
  uint32_t loop_start = 0;
  uint32_t loop_end = 0;
  bool inherit_loop_points = true;
  std::string owner;
  int priority = 0;
  std::array<uint8_t, 16> tag{};
};

struct LipSyncPatch {
  std::string selector;
  std::filesystem::path host_file;
  std::vector<EternalSonataLipEvent> events;
  std::string owner;
  int priority = 0;
};

// What a mod's [[voice_language]] means for its voice patches: which suffix its
// synthesized banks are written under, and which shipped bank each one is
// cloned from. Keyed on the mod's folder name, filled by ScanModLanguages and
// read by ScanModAssets, which runs later (OnPostSetup, then BindAssetSystem).
struct ModVoiceBanks {
  std::string suffix;        // "_ptbr", already normalised by settings.cpp
  std::string donor_suffix;  // "" (Japanese) or "_usa" (English)
  std::string label;
};

struct Container {
  // Set only on a bank this host synthesizes for a mod voice language: the
  // guest path of the shipped container whose framing it clones. The game has
  // no file at this container's own path, so that is where BuildContainer reads
  // its base bytes from, and it is why BuildCache has to *append* an
  // index.vmtoc record rather than rewrite one.
  std::string donor_path;
  std::map<std::string, TextPatch> text;  // key: canonical reference suffix
  std::map<std::string, TexturePatch> textures;
  std::map<std::string, MeshPatch> meshes;
  std::map<std::string, ModelChunkPatch> skeletons;
  std::map<std::string, ModelChunkPatch> animations;
  std::map<std::string, AudioPatch> audio;
  std::map<std::string, LipSyncPatch> lipsync;
  std::optional<RawPatch> raw;
};

struct State {
  std::recursive_mutex mutex;
  rex::Runtime* runtime = nullptr;
  std::map<std::string, Container> containers;  // guest path -> patches
  std::vector<std::pair<uint32_t, std::pair<EternalSonataAssetProviderFn, void*>>> providers;
  uint32_t next_provider_token = 1;
  std::filesystem::path cache_dir;
  std::map<std::string, ModVoiceBanks> mod_voice;  // mod folder name -> its banks
  bool bound = false;
  std::map<std::array<uint8_t, 16>, AudioPatch*> tagged_audio;
};

State& state() {
  static State s;
  return s;
}

EternalSonataAssetResult RegisterAudio(const std::string& guest_path, AudioPatch patch,
                                       bool force);
EternalSonataAssetResult RegisterLipSync(const std::string& guest_path, LipSyncPatch patch,
                                         bool force);

// ---------------------------------------------------------------------------
// Reference parsing
// ---------------------------------------------------------------------------
struct Reference {
  std::string guest_path;
  std::string kind;      // asset kind, or empty for a whole file
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
  const std::string code = want;  // "PT", "ESP", ... before the fourcc padding
  want.resize(4, ' ');
  for (const char* known : assets::kBtxLanguages) {
    if (want == known) {
      *lang = want;
      return true;
    }
  }

  // Not one of the seven shipped fourccs, so try the language registry: a mod
  // that added Portuguese writes text/PT/1234.txt, and its text belongs in
  // whichever BTX block that language claimed as its donor. Matching on the
  // registry's two-letter code is what makes the mod's own folder name work
  // without it having to know which block it landed on.
  for (const auto& option : GetLanguageOptions()) {
    if (!option.btx_slot || code != option.code)
      continue;
    *lang = option.btx_slot;
    return true;
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

EternalSonataAssetResult RegisterMesh(const std::string& guest_path, MeshPatch patch, bool force) {
  auto& container = state().containers[guest_path];
  const std::string key = "mesh:" + patch.selector;
  auto it = container.meshes.find(key);
  if (it != container.meshes.end()) {
    const bool wins = force || patch.priority < it->second.priority;
    REXLOG_WARN("assets: '{}' and '{}' both patch {}#{}; '{}' wins", it->second.owner, patch.owner,
                guest_path, key, wins ? patch.owner : it->second.owner);
    if (!wins)
      return ETERNALSONATA_ASSET_CONFLICT;
  }
  container.meshes[key] = std::move(patch);
  return ETERNALSONATA_ASSET_OK;
}

EternalSonataAssetResult RegisterModelChunk(const std::string& guest_path, const char* kind,
                                            ModelChunkPatch patch, bool force) {
  auto& patches = std::string_view(kind) == "skeleton" ? state().containers[guest_path].skeletons
                                                       : state().containers[guest_path].animations;
  const std::string key = std::string(kind) + ":" + patch.selector;
  auto it = patches.find(key);
  if (it != patches.end()) {
    const bool wins = force || patch.priority < it->second.priority;
    REXLOG_WARN("assets: '{}' and '{}' both patch {}#{}; '{}' wins", it->second.owner, patch.owner,
                guest_path, key, wins ? patch.owner : it->second.owner);
    if (!wins)
      return ETERNALSONATA_ASSET_CONFLICT;
  }
  patches[key] = std::move(patch);
  return ETERNALSONATA_ASSET_OK;
}

EternalSonataAssetResult RegisterLipSync(const std::string& guest_path, LipSyncPatch patch,
                                         bool force) {
  auto& patches = state().containers[guest_path].lipsync;
  const std::string key = "lipsync:" + patch.selector;
  auto it = patches.find(key);
  if (it != patches.end()) {
    const bool wins = force || patch.priority < it->second.priority;
    REXLOG_WARN("assets: '{}' and '{}' both patch {}#{}; '{}' wins", it->second.owner,
                patch.owner, guest_path, key, wins ? patch.owner : it->second.owner);
    if (!wins)
      return ETERNALSONATA_ASSET_CONFLICT;
  }
  patches[key] = std::move(patch);
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

bool MeshMatches(const assets::MeshRef& ref, size_t index, const std::string& selector) {
  if (IsAllDigits(selector))
    return size_t(std::stoul(selector)) == index;
  if (ref.name.empty())
    return false;
  auto lower = [](std::string value) {
    for (char& c : value)
      c = char(std::tolower(uint8_t(c)));
    return value;
  };
  return lower(ref.name) == lower(selector);
}

// ---------------------------------------------------------------------------
// Declarative discovery: mods/<name>/assets/
// ---------------------------------------------------------------------------
// The executable is not a guest file the VFS serves, but its image holds 23
// ordinary BTX blobs (the whole UI chrome), so it is addressed as a container
// like any other: assets/default.xex/text/<blob>/<LANG>/<id>.txt. Everything
// downstream of discovery has to keep it out of the cache directory, since
// there is nothing to serve and no vmtoc record to update -- it is written
// straight into guest memory instead. See ApplyXexTextPatches.
const char kXexContainer[] = "default.xex";

bool IsXexContainer(const std::string& guest_path) { return guest_path == kXexContainer; }

bool LooksLikeContainer(const std::string& component) {
  static const char* kExtensions[] = {".e", ".bmd", ".bop", ".csf", ".cxs", ".xex"};
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

// mods/<name>/assets.toml, the `[[language]]` tables only:
//
//   [[language]]
//   id = 9          # XLanguage id, any value the five built-ins do not use
//   label = "Portugues"
//   code = "PT"     # what the native Options screen's Text row draws
//   slot = "ESP"    # the BTX block this language's text lives in
//
//   [language.strings]
//   resolution_label = "Spyglass Size"
//   achv_name_1 = "First Blood"
//
// A `[language.strings]` table translates the strings this host authors itself
// (the option rows it adds to the game's own screens, and the achievements
// overlay) for the `[[language]]` most recently declared above it. It is what
// lets a translation ship without any C++: the same strings a code mod would
// publish through "settings.native_string".
//
// Hand-parsed like ReadAssetsToml above, and for the same reason. A mod may
// declare more than one language. Everything is validated by
// RegisterModLanguage and RegisterNativeString, which own the first-wins rules
// shared with the mod-registry event route, so this only has to get the fields
// out.
struct DeclaredLanguage {
  std::string id, label, code, slot;
  std::vector<std::pair<std::string, std::string>> strings;
};

// The voice half, from the same file and the same parser:
//
//   [[voice_language]]
//   id = "ptbr"         # what the voice_language cvar stores
//   label = "Portugues"
//   code = "PT"         # what the Options screen's Voice row draws
//   suffix = "_ptbr"    # the bank filename suffix; defaults to the id
//   donor = "usa"       # which shipped bank the clips are timed against
//
// Deliberately not the same list as [[language]]: text and voice are selected
// independently, so a mod may declare either or both. `donor` is per mod rather
// than global because 27 of the 45 shipped banks have no English twin to
// inherit from. See the inventory in HANDOFF_voice_languages.md.
struct DeclaredVoiceLanguage {
  std::string id, label, code, suffix, donor;
};

std::vector<DeclaredLanguage> ReadDeclaredLanguages(
    const std::filesystem::path& path, std::vector<DeclaredVoiceLanguage>* voices = nullptr) {
  std::vector<DeclaredLanguage> languages;
  std::ifstream in(path);
  if (!in)
    return languages;
  std::string line;
  bool in_language = false;  // inside a [[language]] table
  bool in_strings = false;   // inside its [language.strings] table
  bool in_voice = false;     // inside a [[voice_language]] table
  std::vector<DeclaredVoiceLanguage> local_voices;
  while (std::getline(in, line)) {
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
      in_language = (line == "[[language]]");
      // Attaches to the language above it, so a mod declaring two languages
      // gets one strings table each.
      in_strings = (line == "[language.strings]") && !languages.empty();
      in_voice = (line == "[[voice_language]]");
      if (in_language)
        languages.emplace_back();
      if (in_voice)
        local_voices.emplace_back();
      continue;
    }
    if (!in_language && !in_strings && !in_voice)
      continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    std::string key = line.substr(0, eq);
    std::string value = line.substr(eq + 1);
    key.erase(key.find_last_not_of(" \t") + 1);
    const size_t vstart = value.find_first_not_of(" \t");
    value = vstart == std::string::npos ? std::string() : value.substr(vstart);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
      value = value.substr(1, value.size() - 2);
    if (in_voice) {
      DeclaredVoiceLanguage& voice = local_voices.back();
      if (key == "id")
        voice.id = value;
      else if (key == "label")
        voice.label = value;
      else if (key == "code")
        voice.code = value;
      else if (key == "suffix")
        voice.suffix = value;
      else if (key == "donor")
        voice.donor = value;
      continue;
    }
    DeclaredLanguage& current = languages.back();
    if (in_strings)
      current.strings.emplace_back(std::move(key), std::move(value));
    else if (key == "id")
      current.id = value;
    else if (key == "label")
      current.label = value;
    else if (key == "code")
      current.code = value;
    else if (key == "slot")
      current.slot = value;
  }
  if (voices)
    *voices = std::move(local_voices);
  return languages;
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

// True for the guest paths the two voice path builders resolve:
// `btldata\voice\<bank>.csf`, and nothing else. The hook in
// eternalsonata_hooks.cpp filters on exactly the same two ends, since
// sub_8210D380 is the file-existence probe for every file in the game.
constexpr const char* kVoiceDir = "btldata\\voice\\";

bool IsVoiceBankPath(const std::string& guest_path) {
  return guest_path.size() > std::strlen(kVoiceDir) + 4 &&
         guest_path.compare(0, std::strlen(kVoiceDir), kVoiceDir) == 0 &&
         guest_path.compare(guest_path.size() - 4, 4, ".csf") == 0;
}

// `btldata\voice\pc001.csf` -> `btldata\voice\pc001_ptbr.csf`, stripping the
// donor's own suffix first if the mod addressed the English bank directly. The
// same rewrite the path hook performs at runtime, so the two cannot drift.
std::string VoiceBankWithSuffix(const std::string& guest_path, const std::string& suffix) {
  std::string stem = guest_path.substr(0, guest_path.size() - 4);
  if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, "_usa") == 0)
    stem.resize(stem.size() - 4);
  return stem + suffix + ".csf";
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
      RawPatch patch;
      patch.host_file = it->path();
      patch.owner = mod_name;
      patch.priority = priority;
      container.raw = std::move(patch);
      continue;
    }

    std::string guest_path;
    for (size_t i = 0; i < split; ++i)
      guest_path += (i ? "/" : "") + parts[i];
    guest_path = NormalizeGuestPath(guest_path);

    // A mod that declared a [[voice_language]] addresses the *shipped* bank -
    // `assets/btldata/voice/pc001.csf/sfx/3.wav`, because that is the container
    // whose clip ordinals its selectors mean. Its patches go to a bank of the
    // mod's own instead, so both shipped voice languages stay selectable and
    // two voice mods cannot collide. Everything downstream sees an ordinary
    // container that happens to name a donor.
    if (IsVoiceBankPath(guest_path)) {
      const auto voice = state().mod_voice.find(mod_name);
      if (voice != state().mod_voice.end()) {
        const std::string donor =
            VoiceBankWithSuffix(guest_path, voice->second.donor_suffix);
        guest_path = VoiceBankWithSuffix(guest_path, voice->second.suffix);
        state().containers[guest_path].donor_path = donor;
      }
    }

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
    } else if ((kind == "meshes" || kind == "mesh") && tail.size() == 1) {
      MeshPatch patch;
      patch.selector = it->path().stem().string();
      patch.host_file = it->path();
      patch.allow_resize = AllowResizeFor(toml, guest_path, "mesh:" + patch.selector);
      patch.owner = mod_name;
      patch.priority = priority;
      RegisterMesh(guest_path, std::move(patch), false);
    } else if ((kind == "skeletons" || kind == "skeleton") && tail.size() == 1) {
      ModelChunkPatch patch;
      patch.selector = it->path().stem().string();
      patch.host_file = it->path();
      patch.allow_resize = AllowResizeFor(toml, guest_path, "skeleton:" + patch.selector);
      patch.owner = mod_name;
      patch.priority = priority;
      RegisterModelChunk(guest_path, "skeleton", std::move(patch), false);
    } else if ((kind == "animations" || kind == "animation") && tail.size() == 1) {
      ModelChunkPatch patch;
      patch.selector = it->path().stem().string();
      patch.host_file = it->path();
      patch.allow_resize = AllowResizeFor(toml, guest_path, "animation:" + patch.selector);
      patch.owner = mod_name;
      patch.priority = priority;
      RegisterModelChunk(guest_path, "animation", std::move(patch), false);
    } else if (std::filesystem::path(kind).stem() == "music" && tail.empty()) {
      AudioPatch patch;
      patch.kind = "music";
      patch.host_file = it->path();
      patch.owner = mod_name;
      patch.priority = priority;
      RegisterAudio(guest_path, std::move(patch), false);
    } else if (kind == "sfx" && tail.size() == 1) {
      AudioPatch patch;
      patch.kind = "sfx";
      patch.selector = it->path().stem().string();
      patch.host_file = it->path();
      patch.owner = mod_name;
      patch.priority = priority;
      RegisterAudio(guest_path, std::move(patch), false);
    } else if (kind == "lipsync" && tail.size() == 1) {
      LipSyncPatch patch;
      patch.selector = it->path().stem().string();
      patch.host_file = it->path();
      patch.owner = mod_name;
      patch.priority = priority;
      RegisterLipSync(guest_path, std::move(patch), false);
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
  h = HashUpdate(h, "v2");
  for (const auto& mod : runtime->EnabledModsInfo()) {
    h = HashUpdate(h, mod.folder_name);
    h = HashUpdate(h, mod.version);
  }
  for (const auto& [path, container] : state().containers) {
    h = HashUpdate(h, path);
    // A synthesized bank's bytes are mostly its donor's, so changing which
    // donor a mod clones is as much a content change as swapping a clip.
    h = HashUpdate(h, container.donor_path);
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
      h = HashUpdate(h, std::to_string(static_cast<int64_t>(when.time_since_epoch().count())));
    };
    for (const auto& [key, patch] : container.textures) {
      h = HashUpdate(h, key);
      if (patch.host_file.empty())
        h = HashUpdate(h, std::string(reinterpret_cast<const char*>(patch.image.pixels.data()),
                                      patch.image.pixels.size()));
      else
        hash_file(patch.host_file);
    }
    for (const auto& [key, patch] : container.meshes) {
      h = HashUpdate(h, key);
      if (!patch.host_file.empty()) {
        hash_file(patch.host_file);
        continue;
      }
      h = HashUpdate(h, std::string_view(reinterpret_cast<const char*>(patch.vertices.data()),
                                         patch.vertices.size() * sizeof(EternalSonataVertex)));
      h = HashUpdate(h, std::string_view(reinterpret_cast<const char*>(patch.indices.data()),
                                         patch.indices.size() * sizeof(uint32_t)));
      h = HashUpdate(h, std::string_view(reinterpret_cast<const char*>(patch.sections.data()),
                                         patch.sections.size() * sizeof(EternalSonataFaceSection)));
      h = HashUpdate(h, patch.allow_resize ? "r" : "p");
    }
    auto hash_chunks = [&](const auto& patches) {
      for (const auto& [key, patch] : patches) {
        h = HashUpdate(h, key);
        if (patch.host_file.empty())
          h = HashUpdate(h, std::string_view(reinterpret_cast<const char*>(patch.bytes.data()),
                                             patch.bytes.size()));
        else
          hash_file(patch.host_file);
        h = HashUpdate(h, patch.allow_resize ? "r" : "p");
      }
    };
    hash_chunks(container.skeletons);
    hash_chunks(container.animations);
    for (const auto& [key, patch] : container.audio) {
      h = HashUpdate(h, key);
      if (!patch.host_file.empty())
        hash_file(patch.host_file);
      else
        h = HashUpdate(h, std::string_view(reinterpret_cast<const char*>(patch.samples.data()),
                                           patch.samples.size() * sizeof(int16_t)));
      h = HashUpdate(h, std::string_view(reinterpret_cast<const char*>(&patch.loop_start),
                                         sizeof(patch.loop_start)));
      h = HashUpdate(h, std::string_view(reinterpret_cast<const char*>(&patch.loop_end),
                                         sizeof(patch.loop_end)));
    }
    for (const auto& [key, patch] : container.lipsync) {
      h = HashUpdate(h, key);
      if (!patch.host_file.empty())
        hash_file(patch.host_file);
      else
        h = HashUpdate(h, std::string_view(reinterpret_cast<const char*>(patch.events.data()),
                                           patch.events.size() * sizeof(EternalSonataLipEvent)));
    }
    if (container.raw) {
      if (container.raw->host_file.empty())
        h = HashUpdate(h, std::string_view(reinterpret_cast<const char*>(container.raw->bytes.data()),
                                           container.raw->bytes.size()));
      else
        hash_file(container.raw->host_file);
    }
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
// be served with are produced together, and there is no other way to get
// either.
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
      const EditStatus status = assets::ApplyTextureEdit(result.bytes, textures[i], image, &error);
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

void ApplyMeshPatches(const std::string& guest_path, const Container& container,
                      PatchedContainer& result) {
  for (const auto& [key, patch] : container.meshes) {
    const auto meshes = assets::FindMeshes(result.bytes);
    bool matched = false;
    for (size_t i = 0; i < meshes.size(); ++i) {
      if (!MeshMatches(meshes[i], i, patch.selector))
        continue;
      matched = true;
      if (!patch.host_file.empty()) {
        std::vector<uint8_t> bytes;
        if (!ReadWholeFile(patch.host_file, bytes) || bytes.size() < 8 ||
            std::memcmp(bytes.data(), "NSHP", 4) != 0) {
          REXLOG_WARN("assets: mod '{}' mesh {}#{} is not an NSHP chunk", patch.owner, guest_path,
                      key);
          break;
        }
        const uint32_t declared = uint32_t(bytes[4]) << 24 | uint32_t(bytes[5]) << 16 |
                                  uint32_t(bytes[6]) << 8 | bytes[7];
        if (declared != bytes.size() ||
            (!patch.allow_resize && bytes.size() > meshes[i].chunk_size)) {
          REXLOG_WARN("assets: mod '{}' mesh {}#{} has an invalid size", patch.owner, guest_path,
                      key);
          break;
        }
        if (!patch.allow_resize) {
          bytes.resize(meshes[i].chunk_size, 0);
          bytes[4] = uint8_t(meshes[i].chunk_size >> 24);
          bytes[5] = uint8_t(meshes[i].chunk_size >> 16);
          bytes[6] = uint8_t(meshes[i].chunk_size >> 8);
          bytes[7] = uint8_t(meshes[i].chunk_size);
        }
        if (assets::ReplaceContainerRange(result.bytes, meshes[i].offset, meshes[i].chunk_size,
                                          bytes))
          ++result.patches_applied;
        break;
      }
      EternalSonataMesh mesh{};
      mesh.vertices = patch.vertices.data();
      mesh.vertex_count = uint32_t(patch.vertices.size());
      mesh.indices = patch.indices.data();
      mesh.index_count = uint32_t(patch.indices.size());
      mesh.sections = patch.sections.data();
      mesh.section_count = uint32_t(patch.sections.size());
      std::string error;
      const auto status =
          assets::ReplaceMeshChunk(result.bytes, meshes[i], mesh, patch.allow_resize, &error);
      if (status == assets::ModelEditStatus::kOk) {
        ++result.patches_applied;
        REXLOG_INFO("assets: mod '{}' replaced {}#{}", patch.owner, guest_path, key);
      } else {
        REXLOG_WARN("assets: mod '{}' mesh {}#{} {}", patch.owner, guest_path, key, error);
      }
      break;
    }
    if (!matched) {
      REXLOG_WARN("assets: mod '{}' patches {}#{}, which the container does not have", patch.owner,
                  guest_path, key);
    }
  }
}

bool LoadModelChunk(const ModelChunkPatch& patch, const char magic[4], std::vector<uint8_t>& bytes,
                    std::string* error) {
  if (patch.host_file.empty()) {
    bytes = patch.bytes;
  } else if (!ReadWholeFile(patch.host_file, bytes)) {
    *error = "could not be read";
    return false;
  }
  if (bytes.size() < 8 || std::memcmp(bytes.data(), magic, 4) != 0) {
    *error = "does not contain the expected native chunk";
    return false;
  }
  const uint32_t declared =
      uint32_t(bytes[4]) << 24 | uint32_t(bytes[5]) << 16 | uint32_t(bytes[6]) << 8 | bytes[7];
  if (declared != bytes.size()) {
    *error = "chunk size field does not match the file length";
    return false;
  }
  bool valid = false;
  if (std::memcmp(magic, "NBN2", 4) == 0) {
    const auto found = assets::FindSkeletons(bytes);
    valid = found.size() == 1 && found[0].offset == 0;
  } else {
    const auto found = assets::FindAnimations(bytes);
    valid = found.size() == 1 && found[0].offset == 0;
  }
  if (!valid) {
    *error = "native chunk structure is malformed";
    return false;
  }
  return true;
}

uint16_t ReadLe16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
uint32_t ReadLe32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint32_t ReadBe32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

struct LipSyncRef {
  size_t events_offset = 0;
  size_t capacity = 0;
  size_t event_count = 0;
};

std::vector<LipSyncRef> FindLipSync(const std::vector<uint8_t>& bytes) {
  std::vector<LipSyncRef> found;
  for (size_t csf = 0; csf + 16 <= bytes.size(); ++csf) {
    if (std::memcmp(bytes.data() + csf, "CSF ", 4) != 0)
      continue;
    const uint32_t header_size = ReadBe32(bytes.data() + csf + 8);
    if (header_size < 16 || header_size > bytes.size() - csf)
      continue;
    const size_t header_end = csf + header_size;
    for (size_t at = csf + 16; at + 16 <= header_end; ++at) {
      if (std::memcmp(bytes.data() + at, "LIP ", 4) != 0)
        continue;
      const uint32_t chunk_size = ReadBe32(bytes.data() + at + 4);
      if (chunk_size < 18 || chunk_size > header_end - at)
        continue;
      LipSyncRef ref;
      ref.events_offset = at + 16;
      ref.capacity = (chunk_size - 16) / 2;
      while (ref.event_count < ref.capacity) {
        const size_t event = ref.events_offset + ref.event_count * 2;
        if (bytes[event] == 0 && bytes[event + 1] == 0)
          break;
        ++ref.event_count;
      }
      found.push_back(ref);
      at += chunk_size - 1;
    }
    csf = header_end - 1;
  }
  return found;
}

bool LoadLipSync(const std::filesystem::path& path, LipSyncPatch& patch, std::string* error) {
  std::ifstream in(path);
  if (!in) {
    *error = "is not readable";
    return false;
  }
  patch.events.clear();
  std::string line;
  size_t line_number = 0;
  while (std::getline(in, line)) {
    ++line_number;
    const size_t first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos || line[first] == '#')
      continue;
    const size_t comma = line.find(',', first);
    if (comma == std::string::npos) {
      *error = "has a line without a comma";
      return false;
    }
    std::string left = line.substr(first, comma - first);
    std::string right = line.substr(comma + 1);
    if (left == "phoneme" && right.find("duration") != std::string::npos)
      continue;
    try {
      size_t left_used = 0;
      size_t right_used = 0;
      const unsigned long phoneme = std::stoul(left, &left_used);
      const unsigned long duration = std::stoul(right, &right_used);
      if (left.find_first_not_of(" \t", left_used) != std::string::npos ||
          right.find_first_not_of(" \t\r", right_used) != std::string::npos || phoneme > 5 ||
          duration == 0 || duration > 255) {
        *error = "has an invalid event on line " + std::to_string(line_number);
        return false;
      }
      patch.events.push_back({uint8_t(phoneme), uint8_t(duration)});
    } catch (...) {
      *error = "has an invalid event on line " + std::to_string(line_number);
      return false;
    }
  }
  return true;
}

bool LoadPcmWav(const std::filesystem::path& path, AudioPatch& patch, std::string* error) {
  std::vector<uint8_t> bytes;
  if (!ReadWholeFile(path, bytes) || bytes.size() < 12 ||
      std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
    *error = "is not a readable little endian WAV";
    return false;
  }
  const uint8_t* pcm = nullptr;
  size_t pcm_size = 0;
  uint16_t bits = 0;
  for (size_t at = 12; at + 8 <= bytes.size();) {
    const uint32_t size = ReadLe32(bytes.data() + at + 4);
    if (size > bytes.size() - at - 8)
      break;
    const uint8_t* body = bytes.data() + at + 8;
    if (std::memcmp(bytes.data() + at, "fmt ", 4) == 0 && size >= 16) {
      if (ReadLe16(body) != 1) {
        *error = "must contain uncompressed PCM";
        return false;
      }
      patch.channels = ReadLe16(body + 2);
      patch.sample_rate = ReadLe32(body + 4);
      bits = ReadLe16(body + 14);
    } else if (std::memcmp(bytes.data() + at, "data", 4) == 0) {
      pcm = body;
      pcm_size = size;
    } else if (std::memcmp(bytes.data() + at, "smpl", 4) == 0 && size >= 60 &&
               ReadLe32(body + 28) > 0) {
      patch.loop_start = ReadLe32(body + 44);
      patch.loop_end = ReadLe32(body + 48) + 1;
      patch.inherit_loop_points = false;
    }
    at += 8 + size + (size & 1);
  }
  if (!pcm || !patch.channels || !patch.sample_rate || bits != 16 ||
      pcm_size % (patch.channels * sizeof(int16_t)) != 0) {
    *error = "must contain 16 bit PCM with a valid data chunk";
    return false;
  }
  patch.samples.resize(pcm_size / 2);
  for (size_t i = 0; i < patch.samples.size(); ++i)
    patch.samples[i] = int16_t(ReadLe16(pcm + i * 2));
  patch.frame_count = uint32_t(patch.samples.size() / patch.channels);
  return true;
}

bool EnsureAudioLoaded(AudioPatch& patch, std::string* error) {
  if (!patch.samples.empty())
    return true;
  std::string ext = patch.host_file.extension().string();
  for (char& c : ext)
    c = char(std::tolower(uint8_t(c)));
  if (ext == ".wav")
    return LoadPcmWav(patch.host_file, patch, error);
  *error = "uses an encoded format this build cannot decode; convert it to 16 bit PCM WAV";
  return false;
}

bool ConvertWavToGuestEndian(std::vector<uint8_t>& bytes) {
  if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
      std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
    return false;
  auto put_be16 = [&bytes](size_t at, uint16_t value) {
    bytes[at] = uint8_t(value >> 8);
    bytes[at + 1] = uint8_t(value);
  };
  auto put_be32 = [&bytes](size_t at, uint32_t value) {
    bytes[at] = uint8_t(value >> 24);
    bytes[at + 1] = uint8_t(value >> 16);
    bytes[at + 2] = uint8_t(value >> 8);
    bytes[at + 3] = uint8_t(value);
  };
  put_be32(4, uint32_t(bytes.size()));
  for (size_t at = 12; at + 8 <= bytes.size();) {
    const uint32_t size = ReadLe32(bytes.data() + at + 4);
    if (size > bytes.size() - at - 8)
      return false;
    put_be32(at + 4, size);
    if (std::memcmp(bytes.data() + at, "fmt ", 4) == 0 && size >= 16) {
      put_be16(at + 8, ReadLe16(bytes.data() + at + 8));
      put_be16(at + 10, ReadLe16(bytes.data() + at + 10));
      put_be32(at + 12, ReadLe32(bytes.data() + at + 12));
      put_be32(at + 16, ReadLe32(bytes.data() + at + 16));
      put_be16(at + 20, ReadLe16(bytes.data() + at + 20));
      put_be16(at + 22, ReadLe16(bytes.data() + at + 22));
    } else if (std::memcmp(bytes.data() + at, "data", 4) == 0) {
      for (size_t i = at + 8; i + 1 < at + 8 + size; i += 2)
        std::swap(bytes[i], bytes[i + 1]);
    }
    at += 8 + size + (size & 1);
  }
  return true;
}

EternalSonataAssetResult RegisterAudio(const std::string& guest_path, AudioPatch patch,
                                       bool force) {
  auto& patches = state().containers[guest_path].audio;
  const std::string key = patch.kind + ":" + patch.selector;
  auto it = patches.find(key);
  if (it != patches.end()) {
    const bool wins = force || patch.priority < it->second.priority;
    REXLOG_WARN("assets: '{}' and '{}' both patch {}#{}; '{}' wins", it->second.owner,
                patch.owner, guest_path, key, wins ? patch.owner : it->second.owner);
    if (!wins)
      return ETERNALSONATA_ASSET_CONFLICT;
  }
  uint64_t token = 0xcbf29ce484222325ull;
  for (char c : guest_path + "#" + key) {
    token ^= uint8_t(c);
    token *= 0x100000001b3ull;
  }
  std::memcpy(patch.tag.data(), "RXPcmSub", 8);
  for (size_t i = 0; i < 8; ++i)
    patch.tag[8 + i] = uint8_t(token >> (i * 8));
  patches[key] = std::move(patch);
  state().tagged_audio[patches[key].tag] = &patches[key];
  return ETERNALSONATA_ASSET_OK;
}

void ApplyModelChunkPatches(const std::string& guest_path, const Container& container,
                            PatchedContainer& result) {
  for (const auto& [key, patch] : container.skeletons) {
    const auto refs = assets::FindSkeletons(result.bytes);
    const size_t index =
        IsAllDigits(patch.selector) ? size_t(std::stoul(patch.selector)) : size_t(-1);
    if (index >= refs.size()) {
      REXLOG_WARN("assets: mod '{}' patches {}#{}, which the container does not have", patch.owner,
                  guest_path, key);
      continue;
    }
    std::vector<uint8_t> bytes;
    std::string error;
    if (!LoadModelChunk(patch, "NBN2", bytes, &error)) {
      REXLOG_WARN("assets: mod '{}' skeleton {}#{} {}", patch.owner, guest_path, key, error);
      continue;
    }
    if (!patch.allow_resize && bytes.size() > refs[index].chunk_size) {
      REXLOG_WARN("assets: mod '{}' skeleton {}#{} does not fit", patch.owner, guest_path, key);
      continue;
    }
    if (!patch.allow_resize) {
      bytes.resize(refs[index].chunk_size, 0);
      bytes[4] = uint8_t(refs[index].chunk_size >> 24);
      bytes[5] = uint8_t(refs[index].chunk_size >> 16);
      bytes[6] = uint8_t(refs[index].chunk_size >> 8);
      bytes[7] = uint8_t(refs[index].chunk_size);
    }
    if (assets::ReplaceContainerRange(result.bytes, refs[index].offset, refs[index].chunk_size,
                                      bytes))
      ++result.patches_applied;
  }
  for (const auto& [key, patch] : container.animations) {
    const auto refs = assets::FindAnimations(result.bytes);
    size_t index = size_t(-1);
    if (IsAllDigits(patch.selector)) {
      index = size_t(std::stoul(patch.selector));
    } else {
      for (size_t i = 0; i < refs.size(); ++i)
        if (std::equal(refs[i].name.begin(), refs[i].name.end(), patch.selector.begin(),
                       patch.selector.end(), [](char a, char b) {
                         return std::tolower(uint8_t(a)) == std::tolower(uint8_t(b));
                       })) {
          index = i;
          break;
        }
    }
    if (index >= refs.size()) {
      REXLOG_WARN("assets: mod '{}' patches {}#{}, which the container does not have", patch.owner,
                  guest_path, key);
      continue;
    }
    std::vector<uint8_t> bytes;
    std::string error;
    if (!LoadModelChunk(patch, "NMTN", bytes, &error)) {
      REXLOG_WARN("assets: mod '{}' animation {}#{} {}", patch.owner, guest_path, key, error);
      continue;
    }
    if (!patch.allow_resize && bytes.size() > refs[index].chunk_size) {
      REXLOG_WARN("assets: mod '{}' animation {}#{} does not fit", patch.owner, guest_path, key);
      continue;
    }
    if (!patch.allow_resize) {
      bytes.resize(refs[index].chunk_size, 0);
      bytes[4] = uint8_t(refs[index].chunk_size >> 24);
      bytes[5] = uint8_t(refs[index].chunk_size >> 16);
      bytes[6] = uint8_t(refs[index].chunk_size >> 8);
      bytes[7] = uint8_t(refs[index].chunk_size);
    }
    if (assets::ReplaceContainerRange(result.bytes, refs[index].offset, refs[index].chunk_size,
                                      bytes))
      ++result.patches_applied;
  }
}

void ApplyAudioPatches(const std::string& guest_path, Container& container,
                       PatchedContainer& result) {
  for (auto& [key, patch] : container.audio) {
    std::string error;
    if (!EnsureAudioLoaded(patch, &error)) {
      REXLOG_WARN("assets: mod '{}' audio {}#{} {}", patch.owner, guest_path, key, error);
      continue;
    }

    size_t payload = size_t(-1);
    if (patch.kind == "music" && result.bytes.size() >= 0x30 &&
        std::memcmp(result.bytes.data(), "CXS ", 4) == 0) {
      payload = ReadBe32(result.bytes.data() + 0x20);
      if (patch.inherit_loop_points) {
        patch.loop_start = ReadBe32(result.bytes.data() + 0x14);
        patch.loop_end = ReadBe32(result.bytes.data() + 0x18);
      }
    } else if (patch.kind == "sfx" && result.bytes.size() >= 0x20 &&
               std::memcmp(result.bytes.data(), "CSF ", 4) == 0 &&
               IsAllDigits(patch.selector)) {
      const uint32_t audio_start = ReadBe32(result.bytes.data() + 8);
      const size_t wanted = size_t(std::stoul(patch.selector));
      size_t ordinal = 0;
      for (size_t at = 0x10; at + 24 <= std::min<size_t>(audio_start, result.bytes.size()); ++at) {
        if (std::memcmp(result.bytes.data() + at, "TIM ", 4) != 0)
          continue;
        if (ordinal++ == wanted) {
          payload = size_t(audio_start) + ReadBe32(result.bytes.data() + at + 16);
          break;
        }
        const uint32_t length = ReadBe32(result.bytes.data() + at + 4);
        if (length >= 16)
          at += 7 + length;
      }
    }
    if (payload == size_t(-1) || payload + patch.tag.size() > result.bytes.size()) {
      REXLOG_WARN("assets: mod '{}' patches {}#{}, which the audio container does not have",
                  patch.owner, guest_path, key);
      continue;
    }
    std::memcpy(result.bytes.data() + payload, patch.tag.data(), patch.tag.size());
    ++result.patches_applied;
    REXLOG_INFO("assets: mod '{}' replaced {}#{} with {} PCM frames", patch.owner, guest_path,
                key, patch.frame_count);
  }
}

void ApplyLipSyncPatches(const std::string& guest_path, Container& container,
                         PatchedContainer& result) {
  for (auto& [key, patch] : container.lipsync) {
    if (!patch.host_file.empty()) {
      std::string error;
      if (!LoadLipSync(patch.host_file, patch, &error)) {
        REXLOG_WARN("assets: mod '{}' lip sync {}#{} {}", patch.owner, guest_path, key, error);
        continue;
      }
    }
    const auto refs = FindLipSync(result.bytes);
    const size_t index =
        IsAllDigits(patch.selector) ? size_t(std::stoul(patch.selector)) : size_t(-1);
    if (index >= refs.size()) {
      REXLOG_WARN("assets: mod '{}' patches {}#{}, which the container does not have",
                  patch.owner, guest_path, key);
      continue;
    }
    const LipSyncRef& ref = refs[index];
    if (patch.events.size() + 1 > ref.capacity) {
      REXLOG_WARN("assets: mod '{}' lip sync {}#{} has {} events but only {} fit", patch.owner,
                  guest_path, key, patch.events.size(), ref.capacity - 1);
      continue;
    }
    uint8_t* output = result.bytes.data() + ref.events_offset;
    std::memset(output, 0, ref.capacity * 2);
    for (size_t i = 0; i < patch.events.size(); ++i) {
      output[i * 2] = patch.events[i].phoneme;
      output[i * 2 + 1] = patch.events[i].duration;
    }
    ++result.patches_applied;
  }
}

std::optional<PatchedContainer> BuildContainer(rex::Runtime* runtime, const assets::Toc& toc,
                                               const std::string& guest_path,
                                               Container& container) {
  PatchedContainer result;
  result.guest_path = guest_path;

  if (container.raw) {
    if (container.raw->host_file.empty()) {
      result.bytes = container.raw->bytes;
    } else if (!ReadWholeFile(container.raw->host_file, result.bytes)) {
      REXLOG_ERROR("assets: could not read {}", container.raw->host_file.string());
      return std::nullopt;
    }
    if (std::filesystem::path(guest_path).extension() == ".wav" &&
        !ConvertWavToGuestEndian(result.bytes)) {
      REXLOG_ERROR("assets: {} is not a supported PCM WAV", container.raw->host_file.string());
      return std::nullopt;
    }
    result.patches_applied = 1;
    if (container.text.empty() && container.textures.empty() && container.meshes.empty() &&
        container.skeletons.empty() && container.animations.empty() && container.audio.empty() &&
        container.lipsync.empty())
      return result;
  } else {
    // A synthesized bank has no file of its own; its bytes start as the donor's,
    // framing and all, and the clips the mod did not replace stay the donor's
    // XMA2, so a partial voice mod is a mix rather than silence.
    const std::string& source =
        container.donor_path.empty() ? guest_path : container.donor_path;
    const auto base = ResolveBaseFile(runtime, source);
    std::vector<uint8_t> encoded;
    if (!ReadWholeFile(base, encoded)) {
      // A voice bank with no `_usa` twin (27 of the 45) falls back to the
      // bare Japanese name, which is exactly what the guest's own probe does.
      const size_t usa = source.size() > 8 ? source.size() - 8 : std::string::npos;
      if (!container.donor_path.empty() && usa != std::string::npos &&
          source.compare(usa, 8, "_usa.csf") == 0 &&
          ReadWholeFile(ResolveBaseFile(runtime, source.substr(0, usa) + ".csf"), encoded)) {
        container.donor_path = source.substr(0, usa) + ".csf";
        REXLOG_INFO("assets: {} has no English twin; {} is cloned from {} instead", source,
                    guest_path, container.donor_path);
      } else {
        REXLOG_ERROR("assets: {} names {}, which is not in the game data", guest_path,
                     base.string());
        return std::nullopt;
      }
    }
    const assets::TocEntry* entry = toc.Find(container.donor_path.empty() ? guest_path
                                                                         : container.donor_path);
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
        REXLOG_WARN(
            "assets: mod '{}' patches {}#text:{}/{}, which the container "
            "does not have",
            owners[i]->owner, guest_path, owners[i]->lang, owners[i]->id);
        break;
      case EditStatus::kTooLarge:
        REXLOG_WARN(
            "assets: mod '{}' text {}#text:{}/{} does not fit and did "
            "not ask for allow_resize",
            owners[i]->owner, guest_path, owners[i]->lang, owners[i]->id);
        break;
      case EditStatus::kBadData:
        REXLOG_WARN("assets: mod '{}' patch for {} was rejected as malformed", owners[i]->owner,
                    guest_path);
        break;
    }
  }

  ApplyTexturePatches(guest_path, container, result);
  ApplyMeshPatches(guest_path, container, result);
  ApplyModelChunkPatches(guest_path, container, result);
  ApplyAudioPatches(guest_path, container, result);
  ApplyLipSyncPatches(guest_path, container, result);
  if (!container.meshes.empty() || !container.skeletons.empty() || !container.animations.empty()) {
    std::string error;
    if (!assets::ValidateModelGraph(result.bytes, &error)) {
      REXLOG_ERROR("assets: model graph in {} is invalid: {}", guest_path, error);
      return std::nullopt;
    }
  }
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
  for (auto& [guest_path, container] : state().containers) {
    // The executable's blobs are patched in guest memory at launch, not served
    // from the cache directory: there is no file here to write and no TOC
    // record to keep in sync with one.
    if (IsXexContainer(guest_path))
      continue;
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
    // served TOC, and it only ever runs on what BuildContainer returned. A
    // synthesized bank has no record to rewrite, so it gets one appended -
    // AddStored is the only way a new served path comes into existence, which
    // is what keeps the vmtoc invariant true by construction.
    if (!container.donor_path.empty()) {
      if (!toc.AddStored(guest_path, uint32_t(patched->bytes.size()))) {
        // Only reachable on a path too long for the 32-byte field, which the
        // suffix check at registration is supposed to have made impossible.
        REXLOG_ERROR("assets: {} does not fit an index.vmtoc record, so it cannot be served",
                     guest_path);
        continue;
      }
    } else if (!toc.SetStored(guest_path, uint32_t(patched->bytes.size()))) {
      REXLOG_WARN(
          "assets: {} has no TOC record; it will load raw, sized by "
          "the file itself",
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
    REXLOG_ERROR(
        "assets: could not write the patched index.vmtoc, so nothing "
        "will be served");
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
    REXLOG_ERROR(
        "assets: could not remount the game partition; patches will "
        "not be served");
    return false;
  }

  auto null_device = std::make_unique<rex::filesystem::NullDevice>(
      kNullMount,
      std::initializer_list<std::string>{std::string("\\Partition0"), std::string("\\Cache0"),
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

bool SupplyReplacementPcm(void*, const uint8_t tag[16], uint64_t* cursor, int sample_rate,
                          uint32_t channels, int16_t* output, uint32_t frames, bool* finished) {
  State& s = state();
  std::lock_guard<std::recursive_mutex> lock(s.mutex);
  std::array<uint8_t, 16> key;
  std::memcpy(key.data(), tag, key.size());
  auto found = s.tagged_audio.find(key);
  if (found == s.tagged_audio.end())
    return false;
  const AudioPatch& patch = *found->second;
  if (!patch.frame_count || !patch.channels || !patch.sample_rate || sample_rate <= 0)
    return false;

  const uint64_t step = (uint64_t(patch.sample_rate) << 32) / uint32_t(sample_rate);
  const uint64_t loop_begin = uint64_t(patch.loop_start) << 32;
  const uint64_t loop_end = uint64_t(std::min(patch.loop_end, patch.frame_count)) << 32;
  const uint64_t end = uint64_t(patch.frame_count) << 32;
  bool ended = false;
  for (uint32_t frame = 0; frame < frames; ++frame) {
    if (*cursor >= end) {
      if (loop_end > loop_begin)
        *cursor = loop_begin + (*cursor - loop_begin) % (loop_end - loop_begin);
      else
        ended = true;
    } else if (loop_end > loop_begin && *cursor >= loop_end) {
      *cursor = loop_begin + (*cursor - loop_begin) % (loop_end - loop_begin);
    }
    for (uint32_t channel = 0; channel < channels; ++channel) {
      int32_t value = 0;
      if (!ended) {
        const size_t source_frame = size_t(*cursor >> 32);
        if (patch.channels == 1) {
          value = patch.samples[source_frame];
        } else if (channels == 1) {
          value = (int32_t(patch.samples[source_frame * patch.channels]) +
                   patch.samples[source_frame * patch.channels + 1]) /
                  2;
        } else {
          value = patch.samples[source_frame * patch.channels +
                                std::min<uint32_t>(channel, patch.channels - 1)];
        }
      }
      output[size_t(frame) * channels + channel] = int16_t(value);
    }
    if (!ended)
      *cursor += step;
  }
  *finished = ended;
  return true;
}

}  // namespace

void ScanModLanguages(rex::Runtime* runtime) {
  for (const auto& mod : runtime->EnabledModsInfo()) {
    std::vector<DeclaredVoiceLanguage> voices;
    for (const auto& declared : ReadDeclaredLanguages(mod.mod_root / "assets.toml", &voices)) {
      if (declared.id.empty() || declared.label.empty()) {
        REXLOG_WARN("assets: mod '{}' declares a [[language]] with no id or label",
                    mod.folder_name);
        continue;
      }
      if (!RegisterModLanguage(declared.id, declared.label, declared.code, declared.slot)) {
        REXLOG_WARN("assets: mod '{}' could not register language '{}'; its text patches for it "
                    "will not resolve, but its other patches still apply",
                    mod.folder_name, declared.label);
        continue;
      }
      // The [language.strings] table, if it had one. Registered against the id
      // rather than the list position, so it survives another mod being
      // enabled ahead of this one.
      const auto id = uint32_t(std::strtoul(declared.id.c_str(), nullptr, 10));
      for (const auto& [key, value] : declared.strings)
        RegisterNativeString(id, key, value);
    }

    // Voice languages, from the same file but a separate list and a separate
    // registry. Only the first one a mod declares gets its voice patches
    // redirected: the redirect is keyed on the mod, and a mod shipping clips
    // for two voice languages at once has no way to say which set is which.
    for (const auto& voice : voices) {
      if (voice.id.empty() || voice.label.empty()) {
        REXLOG_WARN("assets: mod '{}' declares a [[voice_language]] with no id or label",
                    mod.folder_name);
        continue;
      }
      if (!RegisterModVoiceLanguage(voice.id, voice.label, voice.code, voice.suffix)) {
        REXLOG_WARN(
            "assets: mod '{}' could not register voice language '{}'; its voice clips will not "
            "be served, but its other patches still apply",
            mod.folder_name, voice.label);
        continue;
      }
      if (state().mod_voice.count(mod.folder_name)) {
        REXLOG_WARN(
            "assets: mod '{}' declares more than one [[voice_language]]; '{}' is registered but "
            "its clips under assets/btldata/voice/ still go to '{}'",
            mod.folder_name, voice.label, state().mod_voice[mod.folder_name].label);
        continue;
      }
      ModVoiceBanks banks;
      banks.label = voice.label;
      // Defaults to English: it is what a mod is most likely timing against,
      // and the fall-back below covers the 27 banks that have no English twin.
      banks.donor_suffix = voice.donor == "jpn" || voice.donor == "jp" ? "" : "_usa";
      // The registry lowercases ids and normalises suffixes, so the entry it
      // just created is the authority for both, not the raw toml fields.
      std::string id = voice.id;
      for (char& c : id)
        c = char(std::tolower(uint8_t(c)));
      for (const auto& opt : GetVoiceLanguageOptions()) {
        if (id == opt.id)
          banks.suffix = opt.suffix;
      }
      state().mod_voice[mod.folder_name] = std::move(banks);
    }
  }
}

void BindAssetSystem(rex::Runtime* runtime) {
  State& s = state();
  std::lock_guard<std::recursive_mutex> lock(s.mutex);
  s.runtime = runtime;

  int priority = 0;
  for (const auto& mod : runtime->EnabledModsInfo())
    ScanModAssets(mod.folder_name, priority++, mod.mod_root);

  for (auto& [guest_path, container] : s.containers) {
    for (auto& [key, patch] : container.audio) {
      std::string error;
      if (!EnsureAudioLoaded(patch, &error))
        REXLOG_WARN("assets: mod '{}' audio {}#{} {}", patch.owner, guest_path, key, error);
    }
  }
  if (auto* audio = static_cast<rex::audio::AudioSystem*>(runtime->audio_system())) {
    if (audio->xma_decoder())
      audio->xma_decoder()->SetPcmReplacementProvider(SupplyReplacementPcm, nullptr);
  }

  s.bound = true;
  if (s.containers.empty())
    return;
  RebuildAndServe();
}

// ---------------------------------------------------------------------------
// The executable's own BTX blobs
// ---------------------------------------------------------------------------
namespace {

// The image always loads here for this title. The *size* is deliberately not
// hardcoded: a title update extends the image, so the committed run is walked
// instead. Reading one contiguous run is what keeps `guest = kImageBase +
// offset` true, which the blob offsets depend on.
constexpr uint32_t kImageBase = 0x82000000u;
constexpr uint32_t kImageScanLimit = 0x83000000u;

// The contiguous committed run starting at the image base, copied out so the
// container code can scan it as an ordinary buffer.
bool ReadGuestImage(rex::Runtime* runtime, std::vector<uint8_t>* out, uint32_t* base) {
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory)
    return false;
  auto* heap = memory->LookupHeap(kImageBase);
  if (!heap)
    return false;
  const uint32_t page = heap->page_size();
  uint32_t end = kImageBase;
  for (uint32_t p = kImageBase; p < kImageScanLimit; p += page) {
    auto* h = memory->LookupHeap(p);
    uint32_t protect = 0;
    if (!h || !h->QueryProtect(p, &protect) || protect == 0)
      break;
    end = p + page;
  }
  if (end == kImageBase)
    return false;
  auto* host = memory->TranslateVirtual<uint8_t*>(kImageBase);
  if (!host)
    return false;
  out->assign(host, host + (end - kImageBase));
  *base = kImageBase;
  return true;
}

}  // namespace

void ApplyXexTextPatches(rex::Runtime* runtime) {
  auto& s = state();
  auto it = s.containers.find(kXexContainer);
  if (it == s.containers.end() || it->second.text.empty())
    return;
  const Container& container = it->second;

  std::vector<uint8_t> image;
  uint32_t base = 0;
  if (!ReadGuestImage(runtime, &image, &base)) {
    REXLOG_ERROR("assets: could not read the executable image, {} text patches dropped",
                 container.text.size());
    return;
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
    // The blob cannot grow: it is pinned in the image with unrelated data
    // either side. Refused rather than honoured, however the mod asked.
    edit.allow_resize = false;
    edits.push_back(std::move(edit));
    owners.push_back(&patch);
  }

  std::vector<assets::InPlaceWrite> writes;
  assets::ApplyTextEditsInPlace(image, edits, &writes);

  size_t applied = 0;
  for (size_t i = 0; i < edits.size(); ++i) {
    switch (edits[i].status) {
      case EditStatus::kOk:
        ++applied;
        break;
      case EditStatus::kNotFound:
        REXLOG_WARN("assets: mod '{}' patches {}#text:{}/{}/{}, which the image does not have",
                    owners[i]->owner, kXexContainer, owners[i]->blob, owners[i]->lang,
                    owners[i]->id);
        break;
      case EditStatus::kTooLarge:
        REXLOG_WARN(
            "assets: mod '{}' text {}#text:{}/{}/{} does not fit; the executable's blobs are "
            "preserve-size only, so the whole language block must come in under the original",
            owners[i]->owner, kXexContainer, owners[i]->blob, owners[i]->lang, owners[i]->id);
        break;
      case EditStatus::kBadData:
        REXLOG_WARN("assets: mod '{}' patch for {} was rejected as malformed", owners[i]->owner,
                    kXexContainer);
        break;
    }
  }
  if (writes.empty())
    return;

  // The pool is read-only, so a naive write access-violates. Unprotect only the
  // sub-blocks actually rebuilt, and put the original protection back.
  auto* memory = runtime->memory();
  size_t written = 0;
  for (const auto& w : writes) {
    const uint32_t addr = base + uint32_t(w.offset);
    auto* heap = memory->LookupHeap(addr);
    if (!heap) {
      REXLOG_ERROR("assets: no heap covers {:#010x}, sub-block skipped", addr);
      continue;
    }
    uint32_t old_protect = 0;
    if (!heap->Protect(addr, uint32_t(w.bytes.size()),
                       rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite,
                       &old_protect)) {
      REXLOG_ERROR("assets: could not unprotect {:#010x}, sub-block skipped", addr);
      continue;
    }
    auto* host = memory->TranslateVirtual<uint8_t*>(addr);
    if (host) {
      std::memcpy(host, w.bytes.data(), w.bytes.size());
      ++written;
    }
    heap->Protect(addr, uint32_t(w.bytes.size()), old_protect, nullptr);
  }
  REXLOG_INFO("assets: patched {} strings across {} language blocks in {}", applied, written,
              kXexContainer);
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

extern "C" REX_MOD_PLUGIN_EXPORT EternalSonataAssetResult
EternalSonataGetText(const char* ref, char* buffer, uint32_t capacity, uint32_t* out_length) {
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
  if (IsXexContainer(path)) {
    // Not a file the VFS can open: the executable's blobs are read back out of
    // the loaded image. Text is all it has, so it returns before the texture
    // and mesh scans, which would otherwise walk several megabytes for nothing.
    uint32_t base = 0;
    if (!ReadGuestImage(state().runtime, &data, &base))
      return ETERNALSONATA_ASSET_IO_ERROR;
    const auto image_blobs = assets::FindBtxBlobs(data);
    for (size_t bi = 0; bi < image_blobs.size(); ++bi) {
      for (const auto& lang : image_blobs[bi].langs) {
        for (const auto& [id, value] : lang.entries) {
          const std::string ref = path + "#text:" + std::to_string(bi) + "/" +
                                  lang.fourcc.substr(0, 3) + "/" + std::to_string(id);
          if (!visitor(ref.c_str(), ETERNALSONATA_ASSET_KIND_TEXT, value.c_str(),
                       uint32_t(value.size()), user))
            return ETERNALSONATA_ASSET_OK;
        }
      }
    }
    return ETERNALSONATA_ASSET_OK;
  }
  if (!LoadDecodedContainer(path, data))
    return ETERNALSONATA_ASSET_IO_ERROR;

  if (data.size() >= 0x30 && std::memcmp(data.data(), "CXS ", 4) == 0) {
    const std::string ref = path + "#music";
    visitor(ref.c_str(), ETERNALSONATA_ASSET_KIND_MUSIC, "", ReadBe32(data.data() + 0x10), user);
    return ETERNALSONATA_ASSET_OK;
  }
  if (data.size() >= 0x20 && std::memcmp(data.data(), "CSF ", 4) == 0) {
    const uint32_t audio_start = ReadBe32(data.data() + 8);
    size_t ordinal = 0;
    for (size_t at = 0x10; at + 24 <= std::min<size_t>(audio_start, data.size()); ++at) {
      if (std::memcmp(data.data() + at, "TIM ", 4) != 0)
        continue;
      const std::string ref = path + "#sfx:" + std::to_string(ordinal++);
      if (!visitor(ref.c_str(), ETERNALSONATA_ASSET_KIND_SFX, "", ReadBe32(data.data() + at + 20),
                   user))
        break;
      const uint32_t length = ReadBe32(data.data() + at + 4);
      if (length >= 16)
        at += 7 + length;
    }
    const auto lips = FindLipSync(data);
    for (size_t i = 0; i < lips.size(); ++i) {
      const std::string ref = path + "#lipsync:" + std::to_string(i);
      if (!visitor(ref.c_str(), ETERNALSONATA_ASSET_KIND_LIPSYNC, "",
                   uint32_t(lips[i].event_count), user))
        break;
    }
    return ETERNALSONATA_ASSET_OK;
  }

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

  const auto meshes = assets::FindMeshes(data);
  for (size_t i = 0; i < meshes.size(); ++i) {
    const std::string ref = path + "#mesh:" + std::to_string(i);
    if (!visitor(ref.c_str(), ETERNALSONATA_ASSET_KIND_MESH, meshes[i].name.c_str(),
                 meshes[i].chunk_size, user))
      return ETERNALSONATA_ASSET_OK;
  }
  const auto skeletons = assets::FindSkeletons(data);
  for (size_t i = 0; i < skeletons.size(); ++i) {
    const std::string ref = path + "#skeleton:" + std::to_string(i);
    if (!visitor(ref.c_str(), ETERNALSONATA_ASSET_KIND_SKELETON, "", skeletons[i].chunk_size, user))
      return ETERNALSONATA_ASSET_OK;
  }
  const auto animations = assets::FindAnimations(data);
  for (size_t i = 0; i < animations.size(); ++i) {
    const std::string ref = path + "#animation:" + std::to_string(i);
    if (!visitor(ref.c_str(), ETERNALSONATA_ASSET_KIND_ANIMATION, animations[i].name.c_str(),
                 animations[i].chunk_size, user))
      return ETERNALSONATA_ASSET_OK;
  }
  const auto lips = FindLipSync(data);
  for (size_t i = 0; i < lips.size(); ++i) {
    const std::string ref = path + "#lipsync:" + std::to_string(i);
    if (!visitor(ref.c_str(), ETERNALSONATA_ASSET_KIND_LIPSYNC, "",
                 uint32_t(lips[i].event_count), user))
      return ETERNALSONATA_ASSET_OK;
  }
  return ETERNALSONATA_ASSET_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT EternalSonataAssetResult
EternalSonataClearAssetPatch(const char* ref) {
  Reference parsed;
  if (!ParseReference(ref, &parsed) ||
      (parsed.kind != "text" && parsed.kind != "tex" && parsed.kind != "mesh" &&
       parsed.kind != "skeleton" && parsed.kind != "animation" && parsed.kind != "music" &&
       parsed.kind != "sfx" && parsed.kind != "lipsync" && !parsed.kind.empty()))
    return ETERNALSONATA_ASSET_BAD_REF;
  if (parsed.kind.empty()) {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    auto container = state().containers.find(parsed.guest_path);
    if (container == state().containers.end() || !container->second.raw)
      return ETERNALSONATA_ASSET_NOT_FOUND;
    container->second.raw.reset();
    return ETERNALSONATA_ASSET_OK;
  }
  if (parsed.kind == "skeleton" || parsed.kind == "animation") {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    auto container = state().containers.find(parsed.guest_path);
    if (container == state().containers.end())
      return ETERNALSONATA_ASSET_NOT_FOUND;
    auto& patches =
        parsed.kind == "skeleton" ? container->second.skeletons : container->second.animations;
    return patches.erase(parsed.kind + ":" + parsed.selector) ? ETERNALSONATA_ASSET_OK
                                                              : ETERNALSONATA_ASSET_NOT_FOUND;
  }
  if (parsed.kind == "mesh") {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    auto container = state().containers.find(parsed.guest_path);
    if (container == state().containers.end())
      return ETERNALSONATA_ASSET_NOT_FOUND;
    return container->second.meshes.erase("mesh:" + parsed.selector)
               ? ETERNALSONATA_ASSET_OK
               : ETERNALSONATA_ASSET_NOT_FOUND;
  }
  if (parsed.kind == "tex") {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    auto container = state().containers.find(parsed.guest_path);
    if (container == state().containers.end())
      return ETERNALSONATA_ASSET_NOT_FOUND;
    return container->second.textures.erase("tex:" + parsed.selector)
               ? ETERNALSONATA_ASSET_OK
               : ETERNALSONATA_ASSET_NOT_FOUND;
  }
  if (parsed.kind == "music" || parsed.kind == "sfx") {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    auto container = state().containers.find(parsed.guest_path);
    if (container == state().containers.end())
      return ETERNALSONATA_ASSET_NOT_FOUND;
    const std::string key = parsed.kind + ":" + parsed.selector;
    auto patch = container->second.audio.find(key);
    if (patch == container->second.audio.end())
      return ETERNALSONATA_ASSET_NOT_FOUND;
    state().tagged_audio.erase(patch->second.tag);
    container->second.audio.erase(patch);
    return ETERNALSONATA_ASSET_OK;
  }
  if (parsed.kind == "lipsync") {
    std::lock_guard<std::recursive_mutex> lock(state().mutex);
    auto container = state().containers.find(parsed.guest_path);
    if (container == state().containers.end())
      return ETERNALSONATA_ASSET_NOT_FOUND;
    return container->second.lipsync.erase("lipsync:" + parsed.selector)
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

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t
EternalSonataRegisterAssetProvider(EternalSonataAssetProviderFn provider, void* user) {
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
  if (!guest_path || !*guest_path)
    return ETERNALSONATA_ASSET_BAD_REF;
  if (!bytes || !size)
    return ETERNALSONATA_ASSET_BAD_DATA;
  const std::string path = NormalizeGuestPath(guest_path);
  if (path.empty())
    return ETERNALSONATA_ASSET_BAD_REF;
  RawPatch patch;
  patch.bytes.assign(bytes, bytes + size);
  patch.owner = "runtime";
  patch.priority = kRuntimePriority;
  std::lock_guard<std::recursive_mutex> lock(state().mutex);
  auto& current = state().containers[path].raw;
  if (current) {
    const bool wins = (flags & ETERNALSONATA_ASSET_FORCE) != 0 ||
                      patch.priority < current->priority;
    REXLOG_WARN("assets: '{}' and '{}' both replace {}; '{}' wins", current->owner, patch.owner,
                path, wins ? patch.owner : current->owner);
    if (!wins)
      return ETERNALSONATA_ASSET_CONFLICT;
  }
  current = std::move(patch);
  return ETERNALSONATA_ASSET_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT EternalSonataAssetResult
EternalSonataReplaceTexture(const char* ref, const EternalSonataImage* image, uint32_t flags) {
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

extern "C" REX_MOD_PLUGIN_EXPORT EternalSonataAssetResult
EternalSonataReplaceTextureFromFile(const char* ref, const char* host_path, uint32_t flags) {
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

extern "C" REX_MOD_PLUGIN_EXPORT EternalSonataAssetResult
EternalSonataReplaceMesh(const char* ref, const EternalSonataMesh* mesh, uint32_t flags) {
  Reference parsed;
  if (!ParseReference(ref, &parsed) || parsed.kind != "mesh" || parsed.selector.empty())
    return ETERNALSONATA_ASSET_BAD_REF;
  if (!mesh || !mesh->vertices || !mesh->vertex_count || !mesh->indices || !mesh->index_count ||
      !mesh->sections || !mesh->section_count)
    return ETERNALSONATA_ASSET_BAD_DATA;

  MeshPatch patch;
  patch.selector = parsed.selector;
  patch.vertices.assign(mesh->vertices, mesh->vertices + mesh->vertex_count);
  patch.indices.assign(mesh->indices, mesh->indices + mesh->index_count);
  patch.sections.assign(mesh->sections, mesh->sections + mesh->section_count);
  patch.allow_resize = (flags & ETERNALSONATA_ASSET_ALLOW_RESIZE) != 0;
  patch.owner = "runtime";
  patch.priority = kRuntimePriority;
  std::lock_guard<std::recursive_mutex> lock(state().mutex);
  return RegisterMesh(parsed.guest_path, std::move(patch),
                      (flags & ETERNALSONATA_ASSET_FORCE) != 0);
}

extern "C" REX_MOD_PLUGIN_EXPORT EternalSonataAssetResult
EternalSonataReplaceMeshFromFile(const char* ref, const char* host_path, uint32_t flags) {
  Reference parsed;
  if (!ParseReference(ref, &parsed) || parsed.kind != "mesh" || parsed.selector.empty() ||
      !host_path)
    return ETERNALSONATA_ASSET_BAD_REF;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(host_path, ec))
    return ETERNALSONATA_ASSET_IO_ERROR;
  const std::filesystem::path path(host_path);
  std::string extension = path.extension().string();
  for (char& c : extension)
    c = char(std::tolower(uint8_t(c)));
  if (extension != ".nshp")
    return ETERNALSONATA_ASSET_UNSUPPORTED;
  MeshPatch patch;
  patch.selector = parsed.selector;
  patch.host_file = path;
  patch.allow_resize = (flags & ETERNALSONATA_ASSET_ALLOW_RESIZE) != 0;
  patch.owner = "runtime";
  patch.priority = kRuntimePriority;
  std::lock_guard<std::recursive_mutex> lock(state().mutex);
  return RegisterMesh(parsed.guest_path, std::move(patch),
                      (flags & ETERNALSONATA_ASSET_FORCE) != 0);
}

EternalSonataAssetResult RegisterNativeModelChunk(const char* ref, const uint8_t* bytes,
                                                  uint32_t size, uint32_t flags, const char* kind,
                                                  const char magic[4]) {
  Reference parsed;
  if (!ParseReference(ref, &parsed) || parsed.kind != kind || parsed.selector.empty())
    return ETERNALSONATA_ASSET_BAD_REF;
  if (!bytes || size < 8 || std::memcmp(bytes, magic, 4) != 0)
    return ETERNALSONATA_ASSET_BAD_DATA;
  const uint32_t declared =
      uint32_t(bytes[4]) << 24 | uint32_t(bytes[5]) << 16 | uint32_t(bytes[6]) << 8 | bytes[7];
  if (declared != size)
    return ETERNALSONATA_ASSET_BAD_DATA;
  ModelChunkPatch patch;
  patch.selector = parsed.selector;
  patch.bytes.assign(bytes, bytes + size);
  patch.allow_resize = (flags & ETERNALSONATA_ASSET_ALLOW_RESIZE) != 0;
  patch.owner = "runtime";
  patch.priority = kRuntimePriority;
  std::lock_guard<std::recursive_mutex> lock(state().mutex);
  return RegisterModelChunk(parsed.guest_path, kind, std::move(patch),
                            (flags & ETERNALSONATA_ASSET_FORCE) != 0);
}

EternalSonataAssetResult RegisterNativeModelChunkFile(const char* ref, const char* host_path,
                                                      uint32_t flags, const char* kind) {
  Reference parsed;
  if (!ParseReference(ref, &parsed) || parsed.kind != kind || parsed.selector.empty() || !host_path)
    return ETERNALSONATA_ASSET_BAD_REF;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(host_path, ec))
    return ETERNALSONATA_ASSET_IO_ERROR;
  ModelChunkPatch patch;
  patch.selector = parsed.selector;
  patch.host_file = host_path;
  patch.allow_resize = (flags & ETERNALSONATA_ASSET_ALLOW_RESIZE) != 0;
  patch.owner = "runtime";
  patch.priority = kRuntimePriority;
  std::lock_guard<std::recursive_mutex> lock(state().mutex);
  return RegisterModelChunk(parsed.guest_path, kind, std::move(patch),
                            (flags & ETERNALSONATA_ASSET_FORCE) != 0);
}

extern "C" REX_MOD_PLUGIN_EXPORT EternalSonataAssetResult
EternalSonataReplaceSkeleton(const char* ref, const uint8_t* bytes, uint32_t size, uint32_t flags) {
  return RegisterNativeModelChunk(ref, bytes, size, flags, "skeleton", "NBN2");
}

extern "C" REX_MOD_PLUGIN_EXPORT EternalSonataAssetResult
EternalSonataReplaceSkeletonFromFile(const char* ref, const char* host_path, uint32_t flags) {
  return RegisterNativeModelChunkFile(ref, host_path, flags, "skeleton");
}

extern "C" REX_MOD_PLUGIN_EXPORT EternalSonataAssetResult EternalSonataReplaceAnimation(
    const char* ref, const uint8_t* bytes, uint32_t size, uint32_t flags) {
  return RegisterNativeModelChunk(ref, bytes, size, flags, "animation", "NMTN");
}

extern "C" REX_MOD_PLUGIN_EXPORT EternalSonataAssetResult
EternalSonataReplaceAnimationFromFile(const char* ref, const char* host_path, uint32_t flags) {
  return RegisterNativeModelChunkFile(ref, host_path, flags, "animation");
}

extern "C" REX_MOD_PLUGIN_EXPORT EternalSonataAssetResult
EternalSonataReplaceAudio(const char* ref, const EternalSonataAudio* audio, uint32_t flags) {
  Reference parsed;
  if (!ParseReference(ref, &parsed) ||
      (parsed.kind != "music" && parsed.kind != "sfx") ||
      (parsed.kind == "sfx" && !IsAllDigits(parsed.selector)))
    return ETERNALSONATA_ASSET_BAD_REF;
  if (!audio || !audio->samples || !audio->frame_count || !audio->sample_rate ||
      !audio->channels)
    return ETERNALSONATA_ASSET_BAD_DATA;
  const size_t sample_count = size_t(audio->frame_count) * audio->channels;
  if (sample_count > std::numeric_limits<size_t>::max() / sizeof(int16_t))
    return ETERNALSONATA_ASSET_BAD_DATA;
  AudioPatch patch;
  patch.kind = parsed.kind;
  patch.selector = parsed.selector;
  patch.samples.assign(audio->samples, audio->samples + sample_count);
  patch.frame_count = audio->frame_count;
  patch.sample_rate = audio->sample_rate;
  patch.channels = audio->channels;
  patch.loop_start = audio->loop_start;
  patch.loop_end = audio->loop_end;
  patch.inherit_loop_points = audio->inherit_loop_points != 0;
  patch.owner = "runtime";
  patch.priority = kRuntimePriority;
  std::lock_guard<std::recursive_mutex> lock(state().mutex);
  return RegisterAudio(parsed.guest_path, std::move(patch),
                       (flags & ETERNALSONATA_ASSET_FORCE) != 0);
}

extern "C" REX_MOD_PLUGIN_EXPORT EternalSonataAssetResult
EternalSonataReplaceAudioFromFile(const char* ref, const char* host_path, uint32_t flags) {
  Reference parsed;
  if (!ParseReference(ref, &parsed) ||
      (parsed.kind != "music" && parsed.kind != "sfx") ||
      (parsed.kind == "sfx" && !IsAllDigits(parsed.selector)) || !host_path)
    return ETERNALSONATA_ASSET_BAD_REF;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(host_path, ec))
    return ETERNALSONATA_ASSET_IO_ERROR;
  AudioPatch patch;
  patch.kind = parsed.kind;
  patch.selector = parsed.selector;
  patch.host_file = host_path;
  patch.owner = "runtime";
  patch.priority = kRuntimePriority;
  std::string error;
  if (!EnsureAudioLoaded(patch, &error))
    return ETERNALSONATA_ASSET_UNSUPPORTED;
  std::lock_guard<std::recursive_mutex> lock(state().mutex);
  return RegisterAudio(parsed.guest_path, std::move(patch),
                       (flags & ETERNALSONATA_ASSET_FORCE) != 0);
}

extern "C" REX_MOD_PLUGIN_EXPORT EternalSonataAssetResult EternalSonataReplaceLipSync(
    const char* ref, const EternalSonataLipEvent* events, uint32_t event_count, uint32_t flags) {
  Reference parsed;
  if (!ParseReference(ref, &parsed) || parsed.kind != "lipsync" ||
      !IsAllDigits(parsed.selector))
    return ETERNALSONATA_ASSET_BAD_REF;
  if ((flags & ETERNALSONATA_ASSET_ALLOW_RESIZE) != 0)
    return ETERNALSONATA_ASSET_UNSUPPORTED;
  if (!events && event_count)
    return ETERNALSONATA_ASSET_BAD_DATA;
  LipSyncPatch patch;
  patch.selector = parsed.selector;
  patch.owner = "runtime";
  patch.priority = kRuntimePriority;
  if (event_count)
    patch.events.assign(events, events + event_count);
  for (const auto& event : patch.events) {
    if (event.phoneme > 5 || event.duration == 0)
      return ETERNALSONATA_ASSET_BAD_DATA;
  }
  std::lock_guard<std::recursive_mutex> lock(state().mutex);
  return RegisterLipSync(parsed.guest_path, std::move(patch),
                         (flags & ETERNALSONATA_ASSET_FORCE) != 0);
}

extern "C" REX_MOD_PLUGIN_EXPORT EternalSonataAssetResult EternalSonataReplaceLipSyncFromFile(
    const char* ref, const char* host_path, uint32_t flags) {
  Reference parsed;
  if (!ParseReference(ref, &parsed) || parsed.kind != "lipsync" ||
      !IsAllDigits(parsed.selector) || !host_path)
    return ETERNALSONATA_ASSET_BAD_REF;
  if ((flags & ETERNALSONATA_ASSET_ALLOW_RESIZE) != 0)
    return ETERNALSONATA_ASSET_UNSUPPORTED;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(host_path, ec))
    return ETERNALSONATA_ASSET_IO_ERROR;
  LipSyncPatch patch;
  patch.selector = parsed.selector;
  patch.host_file = host_path;
  patch.owner = "runtime";
  patch.priority = kRuntimePriority;
  std::string error;
  if (!LoadLipSync(patch.host_file, patch, &error))
    return ETERNALSONATA_ASSET_BAD_DATA;
  std::lock_guard<std::recursive_mutex> lock(state().mutex);
  return RegisterLipSync(parsed.guest_path, std::move(patch),
                         (flags & ETERNALSONATA_ASSET_FORCE) != 0);
}
