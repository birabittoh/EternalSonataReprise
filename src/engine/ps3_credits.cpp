// eternalsonata - The PS3's ending credits.
//
// sub_82132100 spawns the next ED credits line: it scrolls up at a fixed
// 0.32 pixels per 1/300 s, and the next spawn waits for the line's height
// plus 8 at that speed. The PS3 (EBOOT 0x3237B0, 0x323D38) reads the speed
// from the task instead, picked once by the scenario counter, because its
// longer ending scenes need slower credits to fill them.
//
// A record with a negative height is an image on the PS3: its text points at
// an NTX3 chunk, drawn as a sprite that scrolls and fades like a line
// (0x323D38, 0x324600). The 360 has no such case and would print the chunk
// as text, so PS3 mode builds an NTEX sprite from it and runs the line itself.

#include "generated/eternalsonata_init.h"
#include "ps3_convert.h"
#include "target.h"

#include <bit>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

namespace {

constexpr uint32_t kRecord = 1396;  // next 12 byte record: x, height, text
constexpr uint32_t kTimer = 1400;   // time left until the next spawn
constexpr uint32_t kData = 52;      // the loaded .bmd
constexpr uint32_t kSlots = 372;    // 32 lines of 32 bytes; free when negative
constexpr uint32_t kSlotStride = 32;
constexpr uint32_t kSlotCount = 32;
constexpr uint32_t kSlotLife = 4;
constexpr uint32_t kSlotVelocityY = 28;
constexpr uint32_t kEdTask = 0x8244099Cu;
constexpr uint32_t kScreenWidth = 0x82465F4Cu;
constexpr uint32_t kScreenHeight = 0x82465F4Eu;
constexpr uint32_t kScenario = 0x8243CB69u;  // big endian, unaligned
constexpr uint32_t kDeltaScale = 0x824D0130u;
constexpr uint32_t kSpriteVtable = 0x82082810u;
constexpr uint32_t kSpriteBytes = 28;

// The x sentinels both releases place a line by.
constexpr int32_t kCentre = -10000;
constexpr int32_t kLeft = -10001;
constexpr int32_t kRight = -10002;

float Ps3Speed(uint32_t scenario) {
  switch (scenario) {
    case 8021: return 0.20333333f;
    case 8022: return 0.211f;
    case 8023: return 0.15f;
    default: return 0.37333333f;
  }
}

struct ImageLine {
  uint32_t sprite;
  float life;
  float x;
  float y;
  float velocity_y;
};

uint32_t g_task = 0;
std::vector<ImageLine> g_images;
// Sprites by a hash of the NTX3 they were built from, which each run of the
// credits loads anew.
std::unordered_map<uint64_t, uint32_t> g_sprites;

uint64_t Hash(const uint8_t* bytes, uint32_t size) {
  uint64_t hash = 0xCBF29CE484222325ull;
  for (uint32_t i = 0; i < size; ++i)
    hash = (hash ^ bytes[i]) * 0x100000001B3ull;
  return hash;
}

uint32_t Scenario(uint8_t* base) {
  uint32_t scenario = 0;
  for (uint32_t i = 0; i < 4; ++i)
    scenario = (scenario << 8) | REX_LOAD_U8(kScenario + i);
  return scenario;
}

}  // namespace

extern "C" REX_FUNC(sub_82181728);
extern "C" REX_FUNC(TEXTURE__Load);
extern "C" REX_FUNC(sub_8212EBC8);

namespace {

// The texture is created asynchronously from the chunk, so both stay alive.
uint32_t SpriteFor(PPCContext& ctx, uint8_t* base, uint32_t ntx3) {
  auto* memory = rex::Runtime::instance()->memory();
  const uint32_t size = REX_LOAD_U32(ntx3 + 4);
  const auto* bytes = memory->TranslateVirtual<const uint8_t*>(ntx3);
  const uint64_t key = Hash(bytes, size);
  if (const auto it = g_sprites.find(key); it != g_sprites.end())
    return it->second;
  g_sprites[key] = 0;
  const auto ntex = eternalsonata::ps3::ConvertNtx3Chunk({bytes, bytes + size});
  if (!ntex) {
    REXLOG_ERROR("ps3 credits: cannot convert the image at {:08X}", ntx3);
    return 0;
  }
  const uint32_t chunk = memory->SystemHeapAlloc(static_cast<uint32_t>(ntex->size()), 0x1000,
                                                 rex::memory::kSystemHeapPhysical);
  const uint32_t sprite = memory->SystemHeapAlloc(kSpriteBytes, 0x20);
  if (!chunk || !sprite) {
    REXLOG_ERROR("ps3 credits: no guest memory for the image at {:08X}", ntx3);
    return 0;
  }
  std::memcpy(memory->TranslateVirtual(chunk), ntex->data(), ntex->size());
  std::memset(memory->TranslateVirtual(sprite), 0, kSpriteBytes);
  REX_STORE_U32(sprite, kSpriteVtable);

  PPCContext saved = ctx;
  ctx.r3.u32 = sprite;
  ctx.r4.u32 = chunk;
  ctx.r5.u32 = 0;
  ctx.r6.u32 = 2;
  TEXTURE__Load(ctx, base);
  const bool loaded = ctx.r3.u32 != 0;
  ctx = saved;
  if (!loaded) {
    REXLOG_ERROR("ps3 credits: the 360 rejected the image at {:08X}", ntx3);
    return 0;
  }
  g_sprites[key] = sprite;
  return sprite;
}

void SpawnImage(PPCContext& ctx, uint8_t* base, uint32_t task, int32_t x, uint32_t ntx3,
                float speed) {
  const uint32_t sprite = SpriteFor(ctx, base, ntx3);
  // The size is the NTX3's own, so spacing holds while the texture loads.
  const int32_t width = REX_LOAD_U16(ntx3 + 32);
  const int32_t height = REX_LOAD_U16(ntx3 + 34);
  const int32_t screen_width = REX_LOAD_U16(kScreenWidth);
  const int32_t screen_height = REX_LOAD_U16(kScreenHeight);
  float left = float(x);
  if (x == kCentre)
    left = float(screen_width / 2 - width / 2);
  else if (x == kLeft)
    left = float(screen_width) * 0.1f;
  else if (x == kRight)
    left = float(screen_width) * 0.9f - float(width);
  if (sprite) {
    const auto life = static_cast<int32_t>(float(screen_height + height) / speed);
    g_images.push_back({sprite, float(life), left, float(screen_height), -speed});
  }
  REX_STORE_U32(task + kTimer, std::bit_cast<uint32_t>(float(height + 8) / speed));
}

// sub_324600 with a fade length of 0, which every spawned line has: full
// alpha while it lives, gone when it ends.
void TickImages(PPCContext& ctx, uint8_t* base) {
  PPCContext saved = ctx;
  ctx.r3.u32 = kDeltaScale;
  ctx.r4.u32 = 0;
  sub_82181728(ctx, base);
  const float delta = static_cast<float>(ctx.f1.f64);
  for (size_t i = 0; i < g_images.size();) {
    ImageLine& line = g_images[i];
    if (line.life <= 0.0f) {
      g_images.erase(g_images.begin() + i);
      continue;
    }
    line.life -= delta;
    line.y += line.velocity_y * delta;
    ctx.r3.u32 = line.sprite;
    ctx.f1.f64 = line.x;
    ctx.f2.f64 = line.y;
    ctx.f3.f64 = 0.0;
    ctx.r7.u32 = 0xFFFFFFFFu;
    ctx.r8.u32 = 0;
    sub_8212EBC8(ctx, base);
    ++i;
  }
  ctx = saved;
}

}  // namespace

REX_EXTERN(__imp__sub_82132100);

REX_HOOK_RAW(sub_82132100) {
  const uint32_t task = ctx.r3.u32;
  const uint32_t record = REX_LOAD_U32(task + kRecord);
  if (!eternalsonata::IsPs3Target() || !record || !REX_LOAD_U32(record)) {
    __imp__sub_82132100(ctx, base);
    return;
  }
  const auto x = static_cast<int32_t>(REX_LOAD_U32(record));
  const auto height = static_cast<int32_t>(REX_LOAD_U32(record + 4));
  const uint32_t text = REX_LOAD_U32(record + 8);
  const float speed = Ps3Speed(Scenario(base));
  if (height < 0 && text) {
    REX_STORE_U32(task + kRecord, record + 12);
    SpawnImage(ctx, base, task, x, REX_LOAD_U32(task + kData) + text, speed);
    return;
  }

  uint32_t slot = 0;
  for (uint32_t i = 0; i < kSlotCount && !slot; ++i) {
    const uint32_t candidate = task + kSlots + i * kSlotStride;
    if (static_cast<int32_t>(REX_LOAD_U32(candidate)) < 0)
      slot = candidate;
  }

  __imp__sub_82132100(ctx, base);

  REX_STORE_U32(task + kTimer, std::bit_cast<uint32_t>(float(height + 8) / speed));
  if (!text || !slot || static_cast<int32_t>(REX_LOAD_U32(slot)) < 0)
    return;
  const int32_t screen = REX_LOAD_U16(kScreenHeight);
  const auto life = static_cast<int32_t>(float(screen + height) / speed);
  REX_STORE_U32(slot + kSlotLife, std::bit_cast<uint32_t>(float(life)));
  REX_STORE_U32(slot + kSlotVelocityY, std::bit_cast<uint32_t>(-speed));
}

REX_EXTERN(__imp__sub_821322A8);

// The ED task's update, which runs with the 2D tasks: SwapEffect draws its
// sprite from the same place.
REX_HOOK_RAW(sub_821322A8) {
  const uint32_t task = ctx.r3.u32;
  if (task != g_task) {
    g_task = task;
    g_images.clear();
  }
  __imp__sub_821322A8(ctx, base);
  // The update deletes the task once the last line has gone.
  if (!eternalsonata::IsPs3Target() || REX_LOAD_U32(kEdTask) != task) {
    g_images.clear();
    return;
  }
  TickImages(ctx, base);
}
