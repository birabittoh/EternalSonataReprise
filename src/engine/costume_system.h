// eternalsonata - Character costumes: the registry behind
// eternalsonata_costume_api.h, which is the mod facing surface. This header is
// what the rest of the exe needs. See docs/costumes.md.
#pragma once

#include <cstdint>

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

// The PS3 variant number of the costume a 0 based PS3 character (ALG, PLK,
// BET) wears, as native 5028 answers it: 1 unless a PS3 costume is on.
int32_t Ps3WornCostume(int32_t character);

}  // namespace eternalsonata
