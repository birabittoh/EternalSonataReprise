// eternalsonata - Who each of the twelve character slots is. See
// character_roster.h and docs/modded-characters.md.

#include "character_roster.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <optional>
#include <vector>

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>

#include "costume_system.h"
#include "eternalsonata_party_api.h"
#include "party_arrays.h"
#include "target.h"

namespace eternalsonata {
namespace {

// Party numbering: Polka is 2, Beat 3, Frederic 4, Claves 9, March 10.
constexpr BuiltinCharacter kBuiltins[kRosterSize + 1] = {
    {"", "", ""},
    {"Allegretto", "allegretto", "bALG"},
    {"Polka", "polka", "bPLK"},
    {"Beat", "beat", "bBET"},
    {"Frederic", "frederic", "bCPN"},
    {"Viola", "viola", "bVOL"},
    {"Salsa", "salsa", "bSLS"},
    {"Jazz", "jazz", "bJRB"},
    {"Falsetto", "falsetto", "bFST"},
    {"Claves", "claves", "bCLV"},
    {"March", "march", "bMCH"},
    {"Crescendo", "crescendo", "bCRS"},
    {"Serenade", "serenade", "bSRN"},
};

// The PS3's own spellings (its EBOOT's menu block, ids 11, 12, 23, 24).
constexpr const char* kPs3Names[2][7] = {
    {"\x83N\x83\x8c\x83" "b\x83V\x83" "F\x83\x93\x83h", "Crescendo", "Crescendo", "Crescendo",
     "Crescendo", "Crescendo", "Crescendo"},
    {"\x83Z\x83\x8c\x83i\x81[\x83" "f", "Serenade", "Serenade", "S\xe9r\xe9nade", "Serenata",
     "Serenade", "Serenata"},
};

constexpr int kAddedSlots = kRosterSize - kRetailCast;

std::mutex g_mutex;
std::array<std::optional<ModdedCharacter>, kAddedSlots> g_slots;

bool AddedSlotNumber(int character) {
  return character >= kFirstAddedSlot && character <= kRosterSize;
}

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& c : out)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// A token from a display name: lowercase, spaces as underscores.
std::string TokenFrom(std::string_view text) {
  std::string out = Lower(text);
  std::replace(out.begin(), out.end(), ' ', '_');
  return out;
}

const ModdedCharacter* SlotLocked(int character) {
  if (!AddedSlotNumber(character) || IsPs3Target())
    return nullptr;
  const auto& slot = g_slots[character - kFirstAddedSlot];
  return slot ? &*slot : nullptr;
}

// Whether the game data, a mod's game/ overlay or the asset cache has `path`.
bool GameFileExists(const char* format, int number) {
  auto* kernel = rex::system::kernel_state();
  auto* vfs = kernel ? kernel->file_system() : nullptr;
  if (!vfs)
    return false;
  char path[96];
  std::snprintf(path, sizeof(path), format, number);
  return vfs->ResolvePath(path) != nullptr;
}

}  // namespace

const BuiltinCharacter& Builtin(int character) {
  return kBuiltins[character >= 1 && character <= kRosterSize ? character : 0];
}

CharacterOrigin OriginOf(int character) {
  if (character >= 1 && character <= kRetailCast)
    return CharacterOrigin::kRetail;
  if (!AddedSlotNumber(character))
    return CharacterOrigin::kVacant;
  if (IsPs3Target())
    return CharacterOrigin::kPs3;
  std::lock_guard lock(g_mutex);
  return SlotLocked(character) ? CharacterOrigin::kModded : CharacterOrigin::kVacant;
}

bool CharacterExists(int character) {
  return OriginOf(character) != CharacterOrigin::kVacant;
}

bool IsModdedCharacter(int character) {
  return OriginOf(character) == CharacterOrigin::kModded;
}

int CharacterBase(int character) {
  if (character >= 1 && character <= kRetailCast)
    return character;
  std::lock_guard lock(g_mutex);
  const ModdedCharacter* modded = SlotLocked(character);
  return modded ? modded->base : 0;
}

int MagicOwner(int character) {
  return IsModdedCharacter(character) ? CharacterBase(character) : character;
}

std::string CharacterDisplayName(int character) {
  switch (OriginOf(character)) {
    case CharacterOrigin::kRetail:
    case CharacterOrigin::kPs3:
      return Builtin(character).name;
    case CharacterOrigin::kModded: {
      std::lock_guard lock(g_mutex);
      const ModdedCharacter* modded = SlotLocked(character);
      return modded ? modded->name : std::string();
    }
    default:
      return {};
  }
}

std::string CharacterToken(int character) {
  switch (OriginOf(character)) {
    case CharacterOrigin::kRetail:
    case CharacterOrigin::kPs3:
      return Builtin(character).token;
    case CharacterOrigin::kModded: {
      std::lock_guard lock(g_mutex);
      const ModdedCharacter* modded = SlotLocked(character);
      return modded ? modded->token : std::string();
    }
    default:
      return {};
  }
}

int CharacterFromToken(std::string_view text) {
  const std::string token = TokenFrom(text);
  if (token.empty())
    return 0;
  for (int c = 1; c <= kRosterSize; ++c) {
    if (CharacterExists(c) && (CharacterToken(c) == token || TokenFrom(CharacterDisplayName(c)) == token))
      return c;
  }
  char* end = nullptr;
  const long number = std::strtol(token.c_str(), &end, 10);
  return end && *end == 0 && CharacterExists(static_cast<int>(number)) ? static_cast<int>(number)
                                                                         : 0;
}

const char* Ps3LocalizedName(int character, uint32_t language) {
  if (!AddedSlotNumber(character))
    return nullptr;
  return kPs3Names[character - kFirstAddedSlot][std::min(language, 6u)];
}

int BattleFileNumber(int character) {
  int chosen = 0, base = 0;
  {
    std::lock_guard lock(g_mutex);
    if (const ModdedCharacter* modded = SlotLocked(character)) {
      chosen = modded->battle_file;
      base = modded->base;
    }
  }
  if (!base)
    return character;
  if (chosen)
    return chosen;
  return GameFileExists("game:\\btldata\\player\\pc%03d.bop", character) ? character : base;
}

int VoiceFileNumber(int character) {
  int chosen = 0, base = 0;
  {
    std::lock_guard lock(g_mutex);
    if (const ModdedCharacter* modded = SlotLocked(character)) {
      chosen = modded->voice_file;
      base = modded->base;
    }
  }
  if (!base)
    return character;
  if (chosen)
    return chosen;
  // The loader tries the _usa bank first, then the bare one.
  return GameFileExists("game:\\btldata\\voice\\pc%03d_usa.csf", character) ||
                 GameFileExists("game:\\btldata\\voice\\pc%03d.csf", character)
             ? character
             : base;
}

bool ModdedDefinition(int character, ModdedCharacter& out) {
  std::lock_guard lock(g_mutex);
  const ModdedCharacter* modded = SlotLocked(character);
  if (!modded)
    return false;
  out = *modded;
  return true;
}

int DefineModdedCharacter(int character, ModdedCharacter definition) {
  if (IsPs3Target())
    return ETERNALSONATA_PARTY_ERR_NO_SLOTS;
  if (definition.name.empty() || definition.id.empty())
    return ETERNALSONATA_PARTY_ERR_INVALID_ARGUMENT;
  if (definition.base < 1 || definition.base > kRetailCast)
    return ETERNALSONATA_PARTY_ERR_INVALID_ARGUMENT;
  if (definition.token.empty())
    definition.token = TokenFrom(definition.name);
  for (int c = 1; c <= kRetailCast; ++c) {
    if (definition.token == Builtin(c).token)
      return ETERNALSONATA_PARTY_ERR_INVALID_ARGUMENT;
  }
  {
    std::lock_guard lock(g_mutex);
    // The same id redefines its own slot wherever it is.
    int own = 0;
    for (int i = 0; i < kAddedSlots; ++i) {
      if (g_slots[i] && g_slots[i]->id == definition.id)
        own = kFirstAddedSlot + i;
      else if (g_slots[i] && g_slots[i]->token == definition.token)
        return ETERNALSONATA_PARTY_ERR_INVALID_ARGUMENT;
    }
    if (character == 0) {
      character = own;
      for (int i = 0; i < kAddedSlots && !character; ++i) {
        if (!g_slots[i])
          character = kFirstAddedSlot + i;
      }
      if (!character)
        return ETERNALSONATA_PARTY_ERR_NO_SLOTS;
    }
    if (!AddedSlotNumber(character))
      return ETERNALSONATA_PARTY_ERR_INVALID_CHARACTER;
    auto& slot = g_slots[character - kFirstAddedSlot];
    if (slot && slot->id != definition.id)
      return ETERNALSONATA_PARTY_ERR_SLOT_TAKEN;
    if (own && own != character)
      return ETERNALSONATA_PARTY_ERR_SLOT_TAKEN;
    slot = std::move(definition);
    REXLOG_INFO("roster: character {} is '{}' ({}), based on {}", character, slot->name, slot->id,
                Builtin(slot->base).name);
  }
  SeedModdedSlot(character);
  CostumeCharacterDefined(character);
  return character;
}

int UndefineModdedCharacter(int character) {
  {
    std::lock_guard lock(g_mutex);
    if (!SlotLocked(character))
      return ETERNALSONATA_PARTY_ERR_INVALID_CHARACTER;
  }
  // Leaving the party runs guest code; a mod removes the member first.
  if (auto* runtime = rex::Runtime::instance(); runtime && runtime->memory()) {
    const uint32_t position = rex::memory::load_and_swap<uint32_t>(
        runtime->memory()->TranslateVirtual<uint8_t*>(PartyArrayAddress(
            PartyArray::kPosition, static_cast<uint32_t>(character - 1))));
    if (position)
      return ETERNALSONATA_PARTY_ERR_ALREADY_IN_PARTY;
  }
  {
    std::lock_guard lock(g_mutex);
    REXLOG_INFO("roster: character {} ('{}') undefined", character,
                g_slots[character - kFirstAddedSlot]->id);
    g_slots[character - kFirstAddedSlot].reset();
  }
  CostumeCharacterDefined(character);
  return ETERNALSONATA_PARTY_OK;
}

int AddedSlotCount() {
  return IsPs3Target() ? 0 : kAddedSlots;
}

int AddedSlot(int index) {
  return index >= 0 && index < AddedSlotCount() ? kFirstAddedSlot + index
                                                : ETERNALSONATA_PARTY_ERR_INVALID_ARGUMENT;
}

// ---------------------------------------------------------------------------
// assets.toml
// ---------------------------------------------------------------------------
//
//   [[character]]
//   name = "Cadenza"                 # required, CP1252
//   key = "cadenza"                  # optional; id "<mod folder>/<key>"
//   slot = 11                        # optional; the first free one otherwise
//   base = "jazz"                    # whose data fills in what is missing
//   model = "chars/cadenza.nobj"     # optional NOBJ
//   scene = "bCDZ"                   # optional battle scene name
//   battle_file = 7                  # optional pc%03d.bop to borrow
//   voice_file = 7                   # optional pc%03d.csf to borrow
//   portrait, panel_portrait, face_portrait, small_face_portrait
//   level, hp, attack, magic, defense, speed   # optional starting stats

namespace {

struct DeclaredCharacter {
  std::vector<std::pair<std::string, std::string>> keys;
};

std::vector<DeclaredCharacter> ReadDeclaredCharacters(const std::filesystem::path& path) {
  std::vector<DeclaredCharacter> out;
  std::ifstream in(path);
  if (!in)
    return out;
  std::string line;
  bool inside = false;
  while (std::getline(in, line)) {
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
      if (line[i] == '"')
        quoted = !quoted;
      else if (line[i] == '#' && !quoted) {
        line.resize(i);
        break;
      }
    }
    const size_t first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos)
      continue;
    line = line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
    if (line.front() == '[') {
      inside = line == "[[character]]";
      if (inside)
        out.emplace_back();
      continue;
    }
    const size_t eq = line.find('=');
    if (!inside || eq == std::string::npos)
      continue;
    std::string key = line.substr(0, eq);
    std::string value = line.substr(eq + 1);
    key.erase(key.find_last_not_of(" \t") + 1);
    const size_t vstart = value.find_first_not_of(" \t");
    value = vstart == std::string::npos ? std::string() : value.substr(vstart);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
      value = value.substr(1, value.size() - 2);
    out.back().keys.emplace_back(std::move(key), std::move(value));
  }
  return out;
}

int RetailFromText(std::string_view text) {
  const std::string token = TokenFrom(text);
  for (int c = 1; c <= kRetailCast; ++c) {
    if (token == Builtin(c).token)
      return c;
  }
  const int number = std::atoi(token.c_str());
  return number >= 1 && number <= kRetailCast ? number : 0;
}

}  // namespace

void ScanModCharacters(rex::Runtime* runtime) {
  if (IsPs3Target())
    return;
  static constexpr const char* kPortraitKeys[kCharacterPortraitKinds] = {
      "portrait", "panel_portrait", "face_portrait", "small_face_portrait"};
  for (const auto& mod : runtime->EnabledModsInfo()) {
    for (const auto& declared : ReadDeclaredCharacters(mod.mod_root / "assets.toml")) {
      ModdedCharacter definition;
      std::string key;
      int slot = 0;
      bool bad = false;
      const auto file = [&](const std::string& value) {
        std::filesystem::path path = mod.mod_root / std::filesystem::u8path(value);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) {
          REXLOG_WARN("roster: mod '{}' names {}, which it does not ship", mod.folder_name, value);
          bad = true;
        }
        return path;
      };
      for (const auto& [name, value] : declared.keys) {
        const int number = std::atoi(value.c_str());
        if (name == "name")
          definition.name = value;
        else if (name == "key")
          key = value;
        else if (name == "slot")
          slot = number;
        else if (name == "base")
          definition.base = RetailFromText(value);
        else if (name == "model")
          definition.model = file(value);
        else if (name == "scene")
          definition.scene = value;
        else if (name == "battle_file")
          definition.battle_file = number;
        else if (name == "voice_file")
          definition.voice_file = number;
        else if (name == "level")
          definition.level = number, definition.has_stats = true;
        else if (name == "hp")
          definition.hp_max = number, definition.has_stats = true;
        else if (name == "attack")
          definition.attack = number, definition.has_stats = true;
        else if (name == "magic")
          definition.magic = number, definition.has_stats = true;
        else if (name == "defense")
          definition.defense = number, definition.has_stats = true;
        else if (name == "speed")
          definition.speed = number, definition.has_stats = true;
        else {
          for (int kind = 0; kind < kCharacterPortraitKinds; ++kind) {
            if (name == kPortraitKeys[kind])
              definition.portraits[kind] = file(value);
          }
        }
      }
      definition.token = TokenFrom(key.empty() ? definition.name : key);
      definition.id = mod.folder_name + "/" + definition.token;
      const int result = bad ? ETERNALSONATA_PARTY_ERR_INVALID_ARGUMENT
                             : DefineModdedCharacter(slot, std::move(definition));
      if (result < 0)
        REXLOG_WARN("roster: mod '{}' declares a [[character]] that cannot be defined ({})",
                    mod.folder_name, result);
    }
  }
}

// ---------------------------------------------------------------------------
// Save record
// ---------------------------------------------------------------------------

void SaveRosterRecord(SaveRecord& record) {
  std::lock_guard lock(g_mutex);
  for (int i = 0; i < kAddedSlots; ++i) {
    if (const ModdedCharacter* modded = SlotLocked(kFirstAddedSlot + i))
      record["roster." + std::to_string(kFirstAddedSlot + i)] = modded->id;
  }
}

void LoadRosterRecord(const SaveRecord& record) {
  std::lock_guard lock(g_mutex);
  for (int i = 0; i < kAddedSlots; ++i) {
    const int character = kFirstAddedSlot + i;
    const auto it = record.find("roster." + std::to_string(character));
    const ModdedCharacter* modded = SlotLocked(character);
    const std::string now = modded ? modded->id : std::string();
    if (it != record.end() && it->second != now)
      REXLOG_WARN("roster: the save had '{}' in slot {}, which now holds '{}'", it->second,
                  character, now.empty() ? "nobody" : now);
  }
}

}  // namespace eternalsonata
