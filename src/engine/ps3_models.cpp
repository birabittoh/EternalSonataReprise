// eternalsonata - Field character models only the PS3 release ships.
//
// Native 1141 kind 0 returns a field character model as a raw NOBJ pointer,
// which 1062 then builds. The 360 serves ids 1..10 from AppKeep.bmd entries
// 0..9 (dword_82420AF8). The PS3 (sub_80610 in the EBOOT) adds negative ids,
// which its events use for the party:
//
//   -10..-12  worn costume of ALG, PLK, BET: pc%s_v%d.p3obj, one buffer per
//             character that sub_801F0 reloads when the selection changes
//   -20..-28  AppKeep2.bmd entries 0..8: CRS, SRN, CPN, VOL, SLS, JRB, FST,
//             MCH, CLV, loaded whole at boot by sub_80C40
//
// The kept 360 AppKeep.bmd already holds every character but CRS and SRN,
// and guest physical memory runs out late in a session with ~48 MB more
// resident, so only AppKeep2 entries 0 and 1 load (at boot, after
// AppKeep.bmd) and a costume buffer is made the first time a non default
// costume is worn. Everything else is the 360 model of the same character.

#include "generated/eternalsonata_init.h"
#include "target.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>

namespace {

constexpr uint32_t kModelTableAddr = 0x82420AF8u;
constexpr uint32_t kAppKeep2Entries = 9;
// CRS and SRN, the only characters the 360 AppKeep.bmd lacks.
constexpr uint32_t kAppKeep2Loaded = 2;

constexpr std::array<const char*, 3> kWearNames = {"alg", "plk", "bet"};
// Variants each character ships, v1 included.
constexpr std::array<int32_t, 3> kWearVariants = {2, 3, 2};
// The costume selection is not kept yet (5028 answers 1).
constexpr int32_t kWornVariant = 1;

struct Wear {
  uint32_t buffer = 0;
  uint32_t capacity = 0;
  int32_t variant = 0;
};

std::array<uint32_t, kAppKeep2Loaded> g_appkeep2{};
std::array<Wear, 3> g_wear{};

std::filesystem::path Ps3File(const std::string& name) {
  return eternalsonata::GameDataRoot() / name;
}

std::filesystem::path WearPath(size_t character, int32_t variant) {
  return Ps3File("pc" + std::string(kWearNames[character]) + "_v" + std::to_string(variant) +
                 ".p3obj");
}

bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

uint32_t AllocPhysical(rex::memory::Memory* memory, uint32_t size) {
  return memory->SystemHeapAlloc(size, 0x1000, rex::memory::kSystemHeapPhysical);
}

uint32_t ReadBe32(const std::vector<uint8_t>& data, size_t at) {
  return (uint32_t(data[at]) << 24) | (uint32_t(data[at + 1]) << 16) |
         (uint32_t(data[at + 2]) << 8) | uint32_t(data[at + 3]);
}

void LoadAppKeep2(rex::memory::Memory* memory) {
  std::vector<uint8_t> data;
  if (!ReadFile(Ps3File("AppKeep2.bmd"), data) || data.size() < 12 ||
      std::memcmp(data.data(), "BMD ", 4) != 0) {
    REXLOG_ERROR("ps3 models: AppKeep2.bmd missing or not a BMD");
    return;
  }
  const uint32_t count = ReadBe32(data, 8);
  if (count != kAppKeep2Entries || data.size() < 12 + 4 * count) {
    REXLOG_ERROR("ps3 models: AppKeep2.bmd has {} entries, not {}", count, kAppKeep2Entries);
    return;
  }
  const uint32_t begin = ReadBe32(data, 12);
  const uint32_t end = ReadBe32(data, 12 + 4 * kAppKeep2Loaded);
  if (begin >= end || end > data.size()) {
    REXLOG_ERROR("ps3 models: AppKeep2.bmd entry table out of range");
    return;
  }
  const uint32_t at = AllocPhysical(memory, end - begin);
  if (!at) {
    REXLOG_ERROR("ps3 models: no guest memory for AppKeep2.bmd ({} bytes)", end - begin);
    return;
  }
  std::memcpy(memory->TranslateVirtual(at), data.data() + begin, end - begin);
  for (uint32_t i = 0; i < kAppKeep2Loaded; ++i)
    g_appkeep2[i] = at + ReadBe32(data, 12 + 4 * i) - begin;
  REXLOG_INFO("ps3 models: AppKeep2.bmd CRS, SRN at {:08X}, {} bytes", at, end - begin);
}

// Sized for the largest variant, as the PS3's WEAR slots are, so a costume
// change reuses the buffer.
bool AllocWear(rex::memory::Memory* memory, size_t character) {
  uint32_t capacity = 0;
  for (int32_t v = 1; v <= kWearVariants[character]; ++v) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(WearPath(character, v), ec);
    if (!ec)
      capacity = std::max(capacity, static_cast<uint32_t>(size));
  }
  Wear& wear = g_wear[character];
  if (!capacity || !(wear.buffer = AllocPhysical(memory, capacity))) {
    REXLOG_ERROR("ps3 models: no buffer for pc{} costumes", kWearNames[character]);
    return false;
  }
  wear.capacity = capacity;
  return true;
}

// sub_801F0: loads a costume into the character's buffer unless it is the
// one already there.
bool LoadWear(rex::memory::Memory* memory, size_t character, int32_t variant) {
  Wear& wear = g_wear[character];
  if (!wear.buffer && !AllocWear(memory, character))
    return false;
  if (wear.variant == variant)
    return true;
  std::vector<uint8_t> data;
  if (!ReadFile(WearPath(character, variant), data) || data.size() > wear.capacity) {
    REXLOG_ERROR("ps3 models: cannot load pc{}_v{}", kWearNames[character], variant);
    return false;
  }
  std::memcpy(memory->TranslateVirtual(wear.buffer), data.data(), data.size());
  wear.variant = variant;
  return true;
}

// The 360 model of the same character, for when a PS3 one did not load.
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
  auto* memory = rex::Runtime::instance()->memory();
  if (id >= -21 && id <= -20 && g_appkeep2[-20 - id])
    return g_appkeep2[-20 - id];
  if (id >= -12 && id <= -10 && kWornVariant != 1) {
    const size_t character = static_cast<size_t>(-10 - id);
    if (LoadWear(memory, character, kWornVariant))
      return g_wear[character].buffer;
  }
  const int32_t slot = FallbackSlot(id);
  return slot < 0 ? 0 : REX_LOAD_U32(kModelTableAddr + 4 * slot);
}

}  // namespace

// sub_82162058 loads AppKeep.bmd into the APPKEEP heap at boot.
REX_EXTERN(__imp__sub_82162058);

REX_HOOK_RAW(sub_82162058) {
  __imp__sub_82162058(ctx, base);
  if (!eternalsonata::IsPs3Target())
    return;
  LoadAppKeep2(rex::Runtime::instance()->memory());
}

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
