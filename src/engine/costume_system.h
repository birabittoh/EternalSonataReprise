// eternalsonata - Character costumes: the registry behind
// eternalsonata_costume_api.h, which is the mod facing surface. This header is
// what the rest of the exe needs. See docs/costumes.md.
#pragma once

#include <cstdint>

#include "save_record.h"

namespace rex {
class Runtime;
}  // namespace rex

namespace eternalsonata {

// Registers the [[costume]] tables of every enabled mod's assets.toml and
// binds the change event to the runtime. Call once from OnPostSetup.
void BindCostumeSystem(rex::Runtime* runtime);

// The same calls the C ABI exports, with its results; characters are 1 based.
int CostumeCount(int character);
const char* CostumeLabel(int character, int costume);
int WornCostume(int character);
int WearCostume(int character, int costume);

// The unlocked costume after the one worn, wrapping; the worn one when no
// other is unlocked.
int NextUnlockedCostume(int character);

// The AppKeep image id of the worn costume's portrait of one
// ETERNALSONATA_COSTUME_PORTRAIT_* kind, loading a mod's on first use; 0 for
// the character's own.
uint32_t CostumePortrait(int character, int kind);

// The model the character wears now, for Crescendo and Serenade, which have
// no model table slot; 0 before the boot or when none is loaded.
uint32_t CostumeModel(int character);
int SetCostumePortrait(int character, int costume, int kind, const char* path);

// The PS3 variant number of the costume a 0 based PS3 character (ALG, PLK,
// BET) wears, as native 5028 answers it: 1 unless a PS3 costume is on.
int32_t Ps3WornCostume(int32_t character);

// Locks of the game in progress, with the C ABI's results: 1 unlocked, 0
// locked, or an error.
int CostumeUnlocked(int character, int costume);
int SetCostumeUnlocked(int character, int costume, bool unlocked);
int SetCostumeStartsLocked(int character, int costume, bool locked);

// The same for a 0 based PS3 character (ALG, PLK, BET) and PS3 variant, as
// natives 5027 and 5026 take them; variant 1 is the default, always unlocked.
bool Ps3CostumeUnlocked(int32_t character, int32_t variant);
void Ps3UnlockCostume(int32_t character, int32_t variant);

// Save record part: the costume each character wears and the locks. Reset is
// a new game: the costumes cvar's choice and every lock as it starts.
void ResetCostumeRecord();
void SaveCostumeRecord(SaveRecord& record);
void LoadCostumeRecord(const SaveRecord& record);

}  // namespace eternalsonata
