// eternalsonata - Field character models only the PS3 release ships.
//
// Native 1141 kind 0 returns a field character model as a raw NOBJ pointer,
// which 1062 then builds. The 360 serves ids 1..10 from the model table
// (dword_82420AF8, AppKeep.bmd entries 0..9). The PS3 (sub_80610 in the
// EBOOT) adds negative ids, which its events use for the party:
//
//   -10..-12  worn costume of ALG, PLK, BET: pc%s_v%d.p3obj, one buffer per
//             character that sub_801F0 reloads when the selection changes
//   -20..-28  appkeep2.bmd entries 0..8: CRS, SRN, CPN, VOL, SLS, JRB, FST,
//             MCH, CLV, loaded whole at boot by sub_80C40
//
// ps3_appkeep.cpp loads all of these. Every id but CRS and SRN is the model
// table entry of the same character, which for -10..-12 is the costume worn;
// CRS and SRN wear theirs through CostumeModel (costume_system.cpp).

#include "costume_system.h"

#include "generated/eternalsonata_init.h"
#include "target.h"

#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>

namespace {

constexpr uint32_t kModelTableAddr = 0x82420AF8u;

// The dword_82420AF8 slot of the character an id names.
int32_t FallbackSlot(int32_t id) {
  switch (id) {
    case -10: return 1;   // ALG
    case -11: return 2;   // PLK
    case -12: return 3;   // BET
    case -22: return 4;   // CPN
    case -23: return 5;   // VOL
    case -24: return 6;   // SLS
    case -25: return 7;   // JRB
    case -26: return 8;   // FST
    case -27: return 9;   // MCH
    case -28: return 10;  // CLV
    default: return -1;
  }
}

uint32_t Ps3Model(uint8_t* base, int32_t id) {
  if (id >= -21 && id <= -20)
    return eternalsonata::CostumeModel(11 + (-20 - id));
  const int32_t slot = FallbackSlot(id);
  return slot < 0 ? 0 : REX_LOAD_U32(kModelTableAddr + 4 * slot);
}

}  // namespace

REX_EXTERN(__imp__sub_820E8B10);

// Native 1141 (kind, id).
REX_HOOK_RAW(sub_820E8B10) {
  const uint32_t args = ctx.r3.u32;
  const int32_t kind = static_cast<int32_t>(REX_LOAD_U32(args));
  const int32_t id = static_cast<int32_t>(REX_LOAD_U32(args + 4));
  if (!eternalsonata::IsPs3Target() || kind != 0 || id >= 0)
    return __imp__sub_820E8B10(ctx, base);
  const uint32_t model = Ps3Model(base, id);
  if (!model) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      REXLOG_WARN("ps3 models: no model for character id {} (1141)", id);
    }
  }
  ctx.r3.s64 = static_cast<int32_t>(model);
}
