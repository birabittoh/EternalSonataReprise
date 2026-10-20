// eternalsonata - Which release's stat growth the ten original characters use.
//
// Each 136 byte template record holds five curves of five floats (HP, ATK, MAG,
// DEF, SPD) at +0x24..+0x88, read by sub_821E7138 (new game), sub_821E8A50
// (joining) and sub_821E80E0 (level up). The PS3 retuned seven of the ten. The
// choice is written into the relocated template (party_arrays.cpp), so it
// reaches the next level up; stats already earned stay.

#include "generated/eternalsonata_init.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/runtime.h>

#include "party_arrays.h"
#include "progression_curves.h"
#include "target.h"

REXCVAR_DEFINE_STRING(progression_curves, "auto", "Eternal Sonata",
                      "Which release's stat growth the original ten characters use: 360, ps3 "
                      "or auto (follows the game data)")
    .allowed({"auto", "360", "ps3"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace eternalsonata {
namespace {

constexpr uint32_t kRecordSize = 136;
constexpr uint32_t kCurvesOffset = 0x24;
constexpr uint32_t kCurvesSize = 0x88 - kCurvesOffset;

// Extracted from the PS3 EBOOT by scripts/ps3_party_template.py.
constexpr uint8_t kPs3Curves[kRetailCharacterCount][kCurvesSize] = {
#include "ps3_progression_curves.inc"
};

enum Applied : int { kNone, k360, kPs3 };

std::mutex g_mutex;
uint8_t g_curves_360[kRetailCharacterCount][kCurvesSize];
std::atomic<int> g_applied{kNone};

bool WantsPs3() {
  const std::string& value = REXCVAR_GET(progression_curves);
  return value == "ps3" || (value == "auto" && IsPs3Target());
}

}  // namespace

void ApplyProgressionCurves() {
  const int wanted = WantsPs3() ? kPs3 : k360;
  if (g_applied.load(std::memory_order_acquire) == wanted)
    return;
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory)
    return;
  std::lock_guard lock(g_mutex);
  auto* table = memory->TranslateVirtual<uint8_t*>(PartyArrayAddress(PartyArray::kTemplate));
  if (g_applied.load(std::memory_order_relaxed) == kNone) {
    for (uint32_t c = 0; c < kRetailCharacterCount; ++c)
      std::memcpy(g_curves_360[c], table + c * kRecordSize + kCurvesOffset, kCurvesSize);
  }
  for (uint32_t c = 0; c < kRetailCharacterCount; ++c)
    std::memcpy(table + c * kRecordSize + kCurvesOffset,
                wanted == kPs3 ? kPs3Curves[c] : g_curves_360[c], kCurvesSize);
  g_applied.store(wanted, std::memory_order_release);
}

}  // namespace eternalsonata

REX_EXTERN(__imp__sub_821E80E0);
REX_EXTERN(__imp__sub_821E8A50);

REX_HOOK_RAW(sub_821E80E0) {
  eternalsonata::ApplyProgressionCurves();
  __imp__sub_821E80E0(ctx, base);
}

REX_HOOK_RAW(sub_821E8A50) {
  eternalsonata::ApplyProgressionCurves();
  __imp__sub_821E8A50(ctx, base);
}
