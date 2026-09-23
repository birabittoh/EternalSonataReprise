#include "field_player_model_override.h"

#include "force_load_area.h"
#include "generated/eternalsonata_init.h"

#include "settings.h"
#include "cutscene_system.h"
#include "overworld_system.h"

#include <atomic>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

REXCVAR_DECLARE(bool, field_action_default_model);

namespace {

// Character number -> cached model handle slot, from sub_821A2B38's dispatch.
// Numbering matches the party code (name table at 0x8203304B). Claves and
// March are out of order.
constexpr uint32_t kCharacterSlotAddr[10] = {
    0x82420AFCu,  // 1  bALG  Allegretto
    0x82420B00u,  // 2  bPLK  Polka
    0x82420B04u,  // 3  bBET  Beat
    0x82420B08u,  // 4  bCPN  Frederic
    0x82420B0Cu,  // 5  bVOL  Viola
    0x82420B10u,  // 6  bSLS  Salsa
    0x82420B14u,  // 7  bJRB  Jazz
    0x82420B18u,  // 8  bFST  Falsetto
    0x82420B20u,  // 9  bCLV  Claves
    0x82420B1Cu,  // 10 bMCH  March
};

constexpr const char* kCharacterNames[11] = {
    "(none)", "Allegretto", "Polka",    "Beat",   "Frederic", "Viola",
    "Salsa",  "Jazz",       "Falsetto", "Claves", "March",
};

// dword_8243FC08[c - 1]: character c's 1-based status screen position, 0 when
// not in the party. The party leader holds position 1.
constexpr uint32_t kStatusMemberList = 0x8243FC08u;
constexpr uint32_t kStatusMemberCount = 10u;
constexpr uint32_t kPartyLeaderPosition = 1u;

// Object kind at object+8; 1 is the field leader.
constexpr uint32_t kObjectKindPC = 1u;

// Map manager singleton, its field leader object pointer, and the party slot
// sub_820FCF80 caches.
constexpr uint32_t kMapManager = 0x8244B4B0u;
constexpr uint32_t kFieldObjectPtrOffset = 1520u;
constexpr uint32_t kCurrentPartySlot = 0x8243C270u;
// When set, sub_820F9EC8 resets the map and respawns the leader itself.
constexpr uint32_t kMapResetFlag = 0x8243C368u;

// Second field object, paired with the leader by sub_820FCF80's forced path.
constexpr uint32_t kSecondObjectPtrOffset = 1528u;
// Field objects: scene handle id at +4, valid only while the byte at +20 is 1.
constexpr uint32_t kObjectHandleIdOffset = 4u;
constexpr uint32_t kObjectHandleLiveOffset = 20u;
constexpr uint8_t kObjectHandleLive = 1u;
// Scene object registry.
constexpr uint32_t kSceneHandleTable = 0x824CF500u;

// sub_8217BED0: scene handle id -> object pointer, 0 when unregistered.
REX_IMPORT(__imp__sub_8217BED0, ResolveSceneHandle, u32(u32, u32));

// sub_820EE238: scene object + mesh name -> mesh index, 0xFFFF when absent.
REX_IMPORT(__imp__sub_820EE238, MeshIndexByName, u32(u32, u32));

// Weapon mesh hiding. sub_820FCF80 hides the leader's weapon by party slot, so
// a substituted model keeps its own weapon visible. Mesh visibility lives at
// scene_object+1380 in 64 byte entries; bit 0 of entry+20 hides the mesh.
constexpr uint32_t kSceneMeshFlagsOffset = 1380u;
constexpr uint32_t kMeshFlagsStrideShift = 6u;
constexpr uint32_t kMeshFlagsMask = 0x3FFFC0u;
constexpr uint32_t kMeshFlagsWordOffset = 20u;
constexpr uint32_t kMeshFlagHidden = 1u;
constexpr uint32_t kMeshIndexNone = 0xFFFFu;

// Every weapon mesh name in the cast (AppKeep.bmd). Kept in guest memory
// because sub_820EE238 compares guest strings, and most are not in the image.
enum MeshName {
  kMeshWeapon,
  kMeshWeaponSw,
  kMeshWeapon01,
  kMeshWeapon02,
  kMeshWeapon03,
  kMeshNameCount,
};

constexpr const char* kMeshNameText[kMeshNameCount] = {
    "weapon", "weapon_sw", "weapon_01", "weapon_02", "weapon_03",
};

// 16 bytes each, sub_820EE238's compare width, on a free 64 KB page at the
// top of the XEX image heap.
constexpr uint32_t kMeshNameBase = 0x8B010000u;
constexpr uint32_t kMeshNameSize = 0x10000u;
constexpr uint32_t kMeshNameStride = 16u;

bool g_mesh_names_ready = false;

// Guest address of a mesh name, or 0 before the page is up.
uint32_t MeshNameAddress(MeshName name) {
  return g_mesh_names_ready ? kMeshNameBase + name * kMeshNameStride : 0;
}

// The scene object behind a field object, or 0.
uint32_t SceneObjectFor(uint8_t* base, uint32_t object) {
  if (object == 0 || object == 0xFFFFFFFFu ||
      REX_LOAD_U8(object + kObjectHandleLiveOffset) != kObjectHandleLive) {
    return 0;
  }
  return ResolveSceneHandle(kSceneHandleTable,
                            REX_LOAD_U32(object + kObjectHandleIdOffset));
}

// Falsetto's arms are rigged to "weapon_sw", so that mesh stays visible.
constexpr int kCharacterFalsetto = 8;

// Runs on sub_820FCF80's tail; hiding from inside the sub_820EE7D8 spawn
// softlocks the next leader change.
void HideWeaponMeshes(uint32_t object, int character) {
  if (!g_mesh_names_ready) {
    return;
  }
  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  const uint32_t scene = SceneObjectFor(base, object);
  if (scene == 0) {
    return;
  }
  const uint32_t flags = REX_LOAD_U32(scene + kSceneMeshFlagsOffset);
  if (flags == 0) {
    return;
  }
  for (int name = 0; name < kMeshNameCount; ++name) {
    if (name == kMeshWeaponSw && character == kCharacterFalsetto) {
      continue;
    }
    const uint32_t index =
        MeshIndexByName(scene, MeshNameAddress(static_cast<MeshName>(name)));
    if (index == kMeshIndexNone) {
      continue;
    }
    const uint32_t entry =
        flags + ((index << kMeshFlagsStrideShift) & kMeshFlagsMask);
    REX_STORE_U32(entry + kMeshFlagsWordOffset,
                  REX_LOAD_U32(entry + kMeshFlagsWordOffset) | kMeshFlagHidden);
  }
}

// Runs from Bind, before the guest. Failure only skips the weapon hiding.
void ReserveMeshNames(rex::Runtime* runtime) {
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory) {
    REXLOG_ERROR("field leader model: no memory subsystem for the mesh names");
    return;
  }
  auto* heap = memory->LookupHeap(kMeshNameBase);
  if (!heap) {
    REXLOG_ERROR("field leader model: no heap covers {:#010x}", kMeshNameBase);
    return;
  }
  if (!heap->AllocFixed(kMeshNameBase, kMeshNameSize, heap->page_size(),
                        rex::memory::kMemoryAllocationReserve |
                            rex::memory::kMemoryAllocationCommit,
                        rex::memory::kMemoryProtectRead |
                            rex::memory::kMemoryProtectWrite)) {
    REXLOG_ERROR("field leader model: could not commit {:#x} bytes at {:#010x}",
                 kMeshNameSize, kMeshNameBase);
    return;
  }
  auto* host = memory->TranslateVirtual<uint8_t*>(kMeshNameBase);
  if (!host) {
    REXLOG_ERROR("field leader model: {:#010x} did not translate", kMeshNameBase);
    return;
  }
  std::memset(host, 0, kMeshNameCount * kMeshNameStride);
  for (int i = 0; i < kMeshNameCount; ++i) {
    std::memcpy(host + i * kMeshNameStride, kMeshNameText[i],
                std::strlen(kMeshNameText[i]));
  }
  g_mesh_names_ready = true;
}

// A forced sub_820FCF80 looks up the second object's scene handle through
// sub_820F6420 without checking it, and faults when it is unregistered.
bool ForcedRespawnIsSafe() {
  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  const uint32_t second = REX_LOAD_U32(kMapManager + kSecondObjectPtrOffset);
  if (second == 0 || second == 0xFFFFFFFFu) {
    return true;
  }
  if (REX_LOAD_U8(second + kObjectHandleLiveOffset) != kObjectHandleLive) {
    return false;
  }
  return ResolveSceneHandle(kSceneHandleTable,
                            REX_LOAD_U32(second + kObjectHandleIdOffset)) != 0;
}

// Field leader respawn; a3 != 0 forces it. Goes through the sub_820EE7D8 hook.
REX_EXTERN(sub_820FCF80);

// sub_820F6068(object, kind, attach): attach to or detach from a map object.
REX_IMPORT(__imp__sub_820F6068, SetObjectRide, uint32_t(uint32_t, uint32_t, uint32_t));

// sub_820F2858(object, on): pad control, bit 0 of object+296. The respawn
// clears it and only a menu close sets it again.
REX_EXTERN(sub_820F2858);

constexpr uint32_t kObjectFlagsOffset = 12u;
constexpr uint32_t kObjectControlOffset = 296u;
constexpr uint32_t kObjectControlled = 1u;
constexpr uint32_t kObjectRideOffset = 84u;
constexpr uint32_t kRideKindMap = 3u;
// Ground bits script command 29 changes to lift the leader, e.g. on a ladder.
constexpr uint32_t kGroundFlagsMask = 0xC0000u;
// Scene object shade colour: current RGB at +0x5E0, target at +0x5F0.
constexpr uint32_t kSceneLightOffset = 0x5E0u;
constexpr uint32_t kSceneLightWords = 8u;

// Forced sub_820FCF80 on the hook's own context: an isolated import leaves
// nested hooks calling from a stale stack pointer. The respawn resets ground
// state, shade and pad control, so those are carried over.
void RespawnFieldLeaderLive(PPCContext& ctx, uint8_t* base) {
  const uint32_t leader = REX_LOAD_U32(kMapManager + kFieldObjectPtrOffset);
  const bool live = leader != 0 && leader != 0xFFFFFFFFu;
  const uint32_t flags = live ? REX_LOAD_U32(leader + kObjectFlagsOffset) : 0;
  const uint32_t ride = live ? REX_LOAD_U32(leader + kObjectRideOffset) : 0;
  const bool controlled =
      live && (REX_LOAD_U32(leader + kObjectControlOffset) & kObjectControlled);
  const uint32_t scene = live ? SceneObjectFor(base, leader) : 0;
  uint32_t light[kSceneLightWords] = {};
  for (uint32_t i = 0; scene && i < kSceneLightWords; ++i) {
    light[i] = REX_LOAD_U32(scene + kSceneLightOffset + i * 4);
  }
  PPCContext saved = ctx;
  ctx.r3.u32 = kMapManager;
  ctx.r4.u32 = REX_LOAD_U32(kCurrentPartySlot);
  ctx.r5.u32 = 1;
  sub_820FCF80(ctx, base);
  ctx = saved;
  const uint32_t respawned = REX_LOAD_U32(kMapManager + kFieldObjectPtrOffset);
  if (live && respawned == leader) {
    REX_STORE_U32(leader + kObjectFlagsOffset,
                  (REX_LOAD_U32(leader + kObjectFlagsOffset) & ~kGroundFlagsMask) |
                      (flags & kGroundFlagsMask));
    if (ride == 0 && REX_LOAD_U32(leader + kObjectRideOffset) != 0) {
      SetObjectRide(leader, kRideKindMap, 0);
    }
    if (controlled) {
      ctx.r3.u32 = leader;
      ctx.r4.u32 = 1;
      sub_820F2858(ctx, base);
      ctx = saved;
    }
    const uint32_t respawned_scene = scene ? SceneObjectFor(base, leader) : 0;
    for (uint32_t i = 0; respawned_scene && i < kSceneLightWords; ++i) {
      REX_STORE_U32(respawned_scene + kSceneLightOffset + i * 4, light[i]);
    }
  }
}

// Persisted cvar tokens, in combo order (see settings.cpp).
constexpr const char* kSelectionTokens[] = {
    "default", "party", "allegretto", "polka",    "beat",   "frederic",
    "viola",   "salsa", "jazz",       "falsetto", "claves", "march",
};

constexpr const char* kSelectionNames[] = {
    "Default", "Party Leader", "Allegretto", "Polka",    "Beat",   "Frederic",
    "Viola",   "Salsa",        "Jazz",       "Falsetto", "Claves", "March",
};
static_assert(sizeof(kSelectionNames) / sizeof(kSelectionNames[0]) ==
                  eternalsonata::FieldPlayerModelOverride::kSelectionCount,
              "combo labels must match kSelectionCount");

// Mirror of the field_leader_model cvar, read on the guest thread.
std::atomic<int> g_selection{eternalsonata::FieldPlayerModelOverride::kSelectionDefault};

// Character the leader is wearing, 0 for the game's own; guest thread only.
int g_applied_character = -1;

// Field actions use the retail model: their motions only fit the retail rigs.
bool g_default_model_for_action = false;
bool g_action_model_respawn = false;

// A character whose model was never cached, so the tick does not retry it
// every frame.
int g_failed_character = -1;

// Respawns the leader when it wears the wrong model. Skipped during a field
// action and a map reset, which respawns on its own.
void ApplySelectedModel(PPCContext& ctx, uint8_t* base) {
  const int character = eternalsonata::FieldPlayerModelOverride::DesiredCharacter();
  const uint32_t object = REX_LOAD_U32(kMapManager + kFieldObjectPtrOffset);
  if (character == g_applied_character || character == g_failed_character ||
      g_default_model_for_action || object == 0 || object == 0xFFFFFFFFu ||
      REX_LOAD_U8(kMapResetFlag) != 0 || !ForcedRespawnIsSafe()) {
    return;
  }
  RespawnFieldLeaderLive(ctx, base);
  g_failed_character = g_applied_character == character ? -1 : character;
}

}  // namespace

namespace eternalsonata {

void FieldPlayerModelOverride::SetSelection(int selection) {
  if (selection < 0 || selection >= kSelectionCount) {
    return;
  }
  g_selection.store(selection, std::memory_order_relaxed);
  rex::cvar::SetFlagByName("field_leader_model", kSelectionTokens[selection]);
  SaveUserSettings();
}

int FieldPlayerModelOverride::Selection() {
  return g_selection.load(std::memory_order_relaxed);
}

const char* const* FieldPlayerModelOverride::SelectionNames() {
  return kSelectionNames;
}

int FieldPlayerModelOverride::DesiredCharacter() {
  const int selection = g_selection.load(std::memory_order_relaxed);
  if (selection == kSelectionDefault) {
    return 0;
  }
  if (selection == kSelectionFollowParty) {
    return PartyLeaderCharacter();
  }
  return selection - kSelectionFirstCharacter + 1;
}

const char* FieldPlayerModelOverride::CharacterName(int character) {
  if (character < 0 || character > 10) {
    return "?";
  }
  return kCharacterNames[character];
}

int FieldPlayerModelOverride::PartyLeaderCharacter() {
  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  for (uint32_t i = 0; i < kStatusMemberCount; ++i) {
    if (REX_LOAD_U32(kStatusMemberList + i * 4u) == kPartyLeaderPosition) {
      return static_cast<int>(i) + 1;
    }
  }
  return 0;
}

void FieldPlayerModelOverride::Bind(rex::Runtime* runtime) {
  ReserveMeshNames(runtime);

  // Unknown tokens fall back to default.
  const std::string value = rex::cvar::GetFlagByName("field_leader_model");
  int selection = kSelectionDefault;
  for (int i = 0; i < kSelectionCount; ++i) {
    if (value == kSelectionTokens[i]) {
      selection = i;
      break;
    }
  }
  g_selection.store(selection, std::memory_order_relaxed);
}

}  // namespace eternalsonata

// sub_820F9EC8 resumes the field after a menu or transition. The leader object
// survives area loads, so a new selection has to be forced here or in the tick.
REX_EXTERN(__imp__sub_820F9EC8);

REX_HOOK_RAW(sub_820F9EC8) {
  // Pending area warp.
  {
    std::lock_guard<std::mutex> lock(eternalsonata::GetWarpMutex());
    if (eternalsonata::GetWarpPending()) {
      eternalsonata::GetWarpPending() = false;
      const uint32_t encoded_dword = eternalsonata::GetWarpEncodedDword();
      REX_STORE_U32(0x8243C230u, encoded_dword);
      // Spawn position; the game takes it from dword_82081BC8[area].
      REX_STORE_U8(0x8243CB69u, 0x00);
      REX_STORE_U8(0x8243CB6Au, 0x00);
      REX_STORE_U8(0x8243CB6Bu, 0x00);
      REX_STORE_U8(0x8243CB6Cu, 0x00);
      REX_STORE_U8(0x8243C368u, 1u);
    }
  }

  // The model may have been cached since the last attempt.
  g_failed_character = -1;
  ApplySelectedModel(ctx, base);
  __imp__sub_820F9EC8(ctx, base);
}

// sub_820FA210 suspends the field and clears mm+36; sub_820F9EC8 sets it again.
// A pending warp is applied through that pair, the way the pause menu does it.
REX_EXTERN(sub_820FA210);
REX_EXTERN(sub_820F9EC8);

constexpr uint32_t kFieldActiveOffset = 36u;

bool WarpPending() {
  std::lock_guard<std::mutex> lock(eternalsonata::GetWarpMutex());
  return eternalsonata::GetWarpPending();
}

// sub_820FE7F8: the map manager's per frame tick. Cutscenes script the leader,
// so the swap waits for them to end.
REX_EXTERN(__imp__sub_820FE7F8);

REX_HOOK_RAW(sub_820FE7F8) {
  if (WarpPending() && REX_LOAD_U8(kMapManager + kFieldActiveOffset) == 1) {
    // The reset tears the field down, so this frame's tick is skipped.
    PPCContext saved = ctx;
    ctx.r3.u32 = kMapManager;
    sub_820FA210(ctx, base);
    ctx = saved;
    ctx.r3.u32 = kMapManager;
    sub_820F9EC8(ctx, base);
    ctx = saved;
    return;
  }
  if (!eternalsonata::IsCutsceneActive()) {
    ApplySelectedModel(ctx, base);
  }
  __imp__sub_820FE7F8(ctx, base);
}

// sub_820EFE38(object, name, visible) writes out of bounds for a mesh the model
// lacks, which the retail hide list hits on a substituted model.
REX_EXTERN(__imp__sub_820EFE38);

REX_HOOK_RAW(sub_820EFE38) {
  const uint32_t name = ctx.r4.u32;
  if (name != 0) {
    const uint32_t scene = SceneObjectFor(base, ctx.r3.u32);
    if (scene != 0 && MeshIndexByName(scene, name) == kMeshIndexNone) {
      // The real function's result for an unknown name.
      ctx.r3.u32 = kMeshIndexNone;
      return;
    }
  }
  __imp__sub_820EFE38(ctx, base);
}

// sub_820EE7D8 instantiates a field object's model from the handle in r4;
// swapping it for a leader (kind 1) spawn changes the model.
REX_EXTERN(__imp__sub_820EE7D8);

REX_HOOK_RAW(sub_820EE7D8) {
  if (REX_LOAD_U16(ctx.r3.u32 + 8) == kObjectKindPC) {
    const int character = eternalsonata::FieldPlayerModelOverride::DesiredCharacter();
    if (character == 0 || g_default_model_for_action) {
      g_applied_character = 0;
    } else if (character >= 1 && character <= 10) {
      const u32 handle = REX_LOAD_U32(kCharacterSlotAddr[character - 1]);
      // An uncached slot would instantiate a null resource.
      if (handle != 0 && handle != 0xFFFFFFFFu) {
        ctx.r4.u32 = handle;
        g_applied_character = character;
      }
    }
  }
  __imp__sub_820EE7D8(ctx, base);
}

// Field action motions (16..35) only fit the retail rigs: swap to the retail
// model for them and back when locomotion resumes.
REX_EXTERN(__imp__sub_820F1490);

REX_HOOK_RAW(sub_820F1490) {
  const uint32_t object = ctx.r3.u32;
  const int32_t animation = ctx.r4.s32;
  eternalsonata::NotifyOverworldFieldAction(object, animation);
  const uint32_t leader = REX_LOAD_U32(kMapManager + kFieldObjectPtrOffset);
  if (!g_action_model_respawn && object == leader &&
      eternalsonata::FieldPlayerModelOverride::DesiredCharacter() >= 1) {
    const bool enabled = REXCVAR_GET(field_action_default_model);
    const bool starts_action = enabled && animation >= 16 && animation <= 35;
    const bool resumes_normal = g_default_model_for_action &&
                                (!enabled || (animation >= 0 && animation < 16));
    if ((starts_action && !g_default_model_for_action) ||
        (resumes_normal && g_default_model_for_action)) {
      g_default_model_for_action = starts_action;
      g_action_model_respawn = true;
      RespawnFieldLeaderLive(ctx, base);
      g_action_model_respawn = false;
      ctx.r3.u32 = REX_LOAD_U32(kMapManager + kFieldObjectPtrOffset);
    }
  }
  __imp__sub_820F1490(ctx, base);
}

// Adds the substituted model's weapon meshes to sub_820FCF80's hide list.
REX_EXTERN(__imp__sub_820FCF80);

REX_HOOK_RAW(sub_820FCF80) {
  const uint32_t map_manager = ctx.r3.u32;
  if (eternalsonata::FieldPlayerModelOverride::Selection() ==
          eternalsonata::FieldPlayerModelOverride::kSelectionFollowParty &&
      eternalsonata::FieldPlayerModelOverride::DesiredCharacter() !=
          g_applied_character &&
      ctx.r5.u32 == 0) {
    // Scripted party changes can keep the slot while the character changes.
    ctx.r5.u32 = 1;
  }
  __imp__sub_820FCF80(ctx, base);

  const int character = eternalsonata::FieldPlayerModelOverride::DesiredCharacter();
  if (character < 1) {
    return;
  }
  HideWeaponMeshes(REX_LOAD_U32(map_manager + kFieldObjectPtrOffset), character);
}
