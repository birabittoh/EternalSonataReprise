// eternalsonata - AppKeep.bmd from the PS3.

#pragma once

#include <cstdint>

#include "generated/eternalsonata_init.h"

namespace eternalsonata {

// Rebuilds the AppKeep slot array in the 360's numbering and loads the
// characters and camp entries the PS3 keeps in other files. Call from the
// AppKeep.bmd load, PS3 data only.
void BuildPs3AppKeep(PPCContext& ctx, uint8_t* base);

// appkeep2.bmd entry: CRS, SRN, CPN, VOL, SLS, JRB, FST, MCH, CLV; 0 if not
// loaded.
uint32_t Ps3AppKeep2Model(uint32_t index);

}  // namespace eternalsonata
