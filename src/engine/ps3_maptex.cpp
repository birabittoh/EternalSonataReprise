// eternalsonata - Map textures and map sounds of the PS3 only maps.
//
// sub_820FA680 pairs a map with its shared texture list (cfdata\maptex, kind
// 0) and its ambient bank (sound\mapSE, kind 1) through a sorted table of 332
// entries, off_820166A0. A map without an entry keeps the previous map's
// textures. The PS3's table (0x516E2C in the EBOOT) is the 360's plus the 19
// below; PS3 mode answers them when the 360 table has no entry.

#include "generated/eternalsonata_init.h"
#include "target.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

namespace {

struct MapEntry {
  const char* map;
  const char* maptex;
  const char* mapse;
};

constexpr std::array<MapEntry, 19> kPs3Maps = {{
    {"cbs60", "CBS_B", ""},
    {"lam01", "LAM_A", "LAM02"},
    {"lam02", "LAM_B", "LAM01"},
    {"lam03", "LAM_C", "LAM01"},
    {"lam04", "LAM_A", "LAM02"},
    {"lam05", "LAM_B", "LAM01"},
    {"lam06", "LAM_D", "LAM01"},
    {"lam07", "LAM_D", "LAM01"},
    {"lam08", "LAM_D", "LAM01"},
    {"lam09", "LAM_D", "LAM01"},
    {"lam10", "LAM_D", "LAM01"},
    {"lam14", "LAM_A", "LAM02"},
    {"sbi01", "SBI_A", "SBI03"},
    {"sbi02", "SBI_B", "SBI01"},
    {"sbi03", "SBI_C", "SBI02"},
    {"sbi04", "SBI_C", "SBI02"},
    {"sbi05", "SBI_D", "SBI02"},
    {"sbi06", "SBI_E", "SBI03"},
    {"sbi07", "SBI_E", "SBI03"},
}};

// The caller copies the answer into an 8 byte buffer.
constexpr uint32_t kNameSize = 8;
static_assert([] {
  for (const auto& entry : kPs3Maps)
    if (std::char_traits<char>::length(entry.maptex) >= kNameSize ||
        std::char_traits<char>::length(entry.mapse) >= kNameSize)
      return false;
  return true;
}());

// Guest copies of each entry's two names, kind 0 then kind 1.
uint32_t g_names = 0;

uint32_t GuestName(rex::memory::Memory* memory, size_t entry, uint32_t kind) {
  if (!g_names) {
    g_names = memory->SystemHeapAlloc(kNameSize * 2 * kPs3Maps.size(), 4);
    if (!g_names) {
      REXLOG_ERROR("ps3 maptex: could not allocate the map names");
      return 0;
    }
    auto* names = memory->TranslateVirtual<char*>(g_names);
    std::memset(names, 0, kNameSize * 2 * kPs3Maps.size());
    for (size_t i = 0; i < kPs3Maps.size(); ++i) {
      std::memcpy(names + kNameSize * 2 * i, kPs3Maps[i].maptex, std::strlen(kPs3Maps[i].maptex));
      std::memcpy(names + kNameSize * (2 * i + 1), kPs3Maps[i].mapse,
                  std::strlen(kPs3Maps[i].mapse));
    }
  }
  return g_names + kNameSize * static_cast<uint32_t>(2 * entry + kind);
}

}  // namespace

REX_EXTERN(__imp__sub_820FA680);

REX_HOOK_RAW(sub_820FA680) {
  const uint32_t name = ctx.r4.u32;
  const uint32_t kind = ctx.r5.u8;
  __imp__sub_820FA680(ctx, base);
  auto* runtime = rex::Runtime::instance();
  if (ctx.r3.u32 || kind > 1 || !name || !runtime || !runtime->memory() ||
      !eternalsonata::IsPs3Target())
    return;
  auto* memory = runtime->memory();

  // Like the guest, only the first five characters count, case folded.
  const auto* text = memory->TranslateVirtual<const char*>(name);
  char key[6] = {};
  for (int i = 0; i < 5 && text[i]; ++i)
    key[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
  for (size_t i = 0; i < kPs3Maps.size(); ++i) {
    if (std::strcmp(key, kPs3Maps[i].map) == 0) {
      ctx.r3.u64 = GuestName(memory, i, kind);
      return;
    }
  }
}
