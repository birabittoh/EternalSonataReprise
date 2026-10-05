// eternalsonata - AppKeep.bmd from the PS3.

#pragma once

#include <cstdint>

#include "generated/eternalsonata_init.h"

namespace eternalsonata {

// Where CRS and SRN's camp portraits go: CRS and SRN of the first set, then of
// the second (0x8202CA28 and 0x8202CA3C name the ten of each).
inline constexpr uint32_t kPs3PortraitSlot = 426;

// CRS and SRN's portraits for the one to three member camp panel, PS3 entries
// 204 and 205: the first PS3 only slots.
inline constexpr uint32_t kPs3MenuPortraitSlot = 412;

// The PS3 costumes' status portraits: ALG v2, PLK v2, PLK v3, BET v2.
inline constexpr uint32_t kPs3CostumePortraitSlot = 430;

// dword_82420AFC, the loader's slot array, and the first slot nothing uses
// on either release; mods' costume portraits go there.
inline constexpr uint32_t kAppKeepSlotArray = 0x82420AFCu;
inline constexpr uint32_t kAppKeepSlotCount = 512;
inline constexpr uint32_t kFirstFreeAppKeepSlot = 434;

// Rebuilds the AppKeep slot array in the 360's numbering and loads the
// characters and camp entries the PS3 keeps in other files. Call from the
// AppKeep.bmd load, PS3 data only.
void BuildPs3AppKeep(PPCContext& ctx, uint8_t* base);

// The 360 image id (slot + 1) the rebuilt array holds a PS3 one at; 0 if
// that entry was not placed.
uint32_t Ps3AppKeepImageId(uint32_t ps3_id);

// appkeep2.bmd entry: CRS, SRN, CPN, VOL, SLS, JRB, FST, MCH, CLV; 0 if not
// loaded.
uint32_t Ps3AppKeep2Model(uint32_t index);

}  // namespace eternalsonata
