// eternalsonata - Who each of the twelve character slots is.
//
// Every per character name and token lives here, so no other file spells the
// cast out. Slots 1..10 are the 360 cast; 11 and 12 are Crescendo and
// Serenade on PS3 data and are free for modded characters on the 360's. A
// modded character is defined by a mod (assets.toml [[character]] or the
// party API) and takes whatever it does not supply from a base character.
// See docs/modded-characters.md.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "save_record.h"

namespace rex {
class Runtime;
}  // namespace rex

namespace eternalsonata {

inline constexpr int kRosterSize = 12;
inline constexpr int kRetailCast = 10;
inline constexpr int kFirstAddedSlot = kRetailCast + 1;

enum class CharacterOrigin : uint8_t {
  kRetail,  // 1..10
  kPs3,     // 11, 12 on PS3 data
  kModded,  // 11, 12 on 360 data, defined by a mod
  kVacant,  // 11, 12 on 360 data, nobody yet
};

struct BuiltinCharacter {
  const char* name;   // English, as the game's own text blocks spell it
  const char* token;  // lowercase key for cvars, saves and assets.toml
  const char* scene;  // battle scene name sub_821A2B38 gives the model
};

// The twelve built-in characters, 1 based; a zeroed entry outside 1..12.
const BuiltinCharacter& Builtin(int character);

// Portrait kinds, ETERNALSONATA_COSTUME_PORTRAIT_*.
inline constexpr int kCharacterPortraitKinds = 4;

// What a mod supplies for a modded slot. Anything left empty or 0 falls back
// to `base`.
struct ModdedCharacter {
  std::string id;     // "<mod folder>/<key>", unique
  std::string token;  // lowercase key, unique among all characters
  std::string name;   // CP1252 display name
  int base = 1;       // retail character the slot falls back on
  // Field, battle and event body, an Xbox 360 NOBJ, worn as the default
  // costume. Empty wears the base's.
  std::filesystem::path model;
  // Battle scene name for the model; empty takes the base's.
  std::string scene;
  // pc%03d.bop / pc%03d.csf number to load; 0 loads the slot's own when the
  // game data or a mod has it, else the base's.
  int battle_file = 0;
  int voice_file = 0;
  std::array<std::filesystem::path, kCharacterPortraitKinds> portraits;
  // Starting own stats, applied when the character joins with no EXP yet.
  bool has_stats = false;
  int32_t level = 0, hp_max = 0, attack = 0, magic = 0, defense = 0, speed = 0;
};

CharacterOrigin OriginOf(int character);

// Retail, the PS3's, or defined by a mod.
bool CharacterExists(int character);
bool IsModdedCharacter(int character);

// The retail character whose cases a slot takes in the game's ten character
// switches: itself for 1..10, the base for a modded slot, 0 otherwise.
int CharacterBase(int character);

// Whose magic records a slot casts: the base's for a modded slot, so its
// ids match the base's battle file and text; the slot itself otherwise.
int MagicOwner(int character);

// English display name; "" for a vacant slot. Renames through the party API
// are layered on top in party_system.cpp.
std::string CharacterDisplayName(int character);

// Lowercase key; "" for a vacant slot.
std::string CharacterToken(int character);

// The character a token, display name (any case) or number names, if it
// exists; 0 otherwise.
int CharacterFromToken(std::string_view text);

// The PS3's spelling of 11 or 12 in dword_8243D370's language order (JPN,
// USA, GBR, FRA, ITA, DEU, ESP), or null.
const char* Ps3LocalizedName(int character, uint32_t language);

// The numbers sub_821A03D0 and sub_821BD0D0 format into pc%03d.bop / .csf.
int BattleFileNumber(int character);
int VoiceFileNumber(int character);

// A copy of the definition of a modded slot; false if it is not one.
bool ModdedDefinition(int character, ModdedCharacter& out);

// Defines slot `character` (0 picks the first free one). Returns the slot or
// a negative ETERNALSONATA_PARTY_ERR_*.
int DefineModdedCharacter(int character, ModdedCharacter definition);
int UndefineModdedCharacter(int character);

// Slots a mod may define on this data: 11 and 12 on 360 data, none on PS3.
int AddedSlotCount();
int AddedSlot(int index);

// Defines the [[character]] tables of every enabled mod's assets.toml. Call
// from OnPostSetup, before BindCostumeSystem.
void ScanModCharacters(rex::Runtime* runtime);

// Records which character held each modded slot, and warns on a load that
// finds another one there.
void SaveRosterRecord(SaveRecord& record);
void LoadRosterRecord(const SaveRecord& record);

}  // namespace eternalsonata
