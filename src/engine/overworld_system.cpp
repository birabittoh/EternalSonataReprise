#include "overworld_system.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include <rex/runtime.h>
#include <rex/hook.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>
#include <rex/system/mod_plugin.h>
#include <rex/system/mod_registry.h>

#include "area_names.generated.h"
#include "eternalsonata_overworld_api.h"
#include "field_player_model_override.h"
#include "force_load_area.h"
#include "guest_main_thread.h"
#include "room_presence.h"
#include "generated/eternalsonata_init.h"

namespace eternalsonata {
namespace {

rex::Runtime* g_runtime = nullptr;
constexpr uint32_t kMapManager = 0x8244B4B0u;
constexpr uint32_t kFieldLeaderOffset = 1520u;
constexpr uint32_t kFieldCameraOffset = 1524u;
constexpr uint32_t kActiveCameraOffset = 2552u;
constexpr uint32_t kSceneManager = 0x824CF500u;
constexpr uint32_t kPositionOffset = 32u;
uint32_t g_position_scratch = 0;
std::mutex g_position_mutex;
EternalSonataFieldPosition g_live_position{};
uint32_t g_live_object = 0;
bool g_live_valid = false;
EternalSonataFieldFacing g_live_facing{};
EternalSonataFieldCamera g_live_camera{};
uint32_t g_live_camera_object = 0;
bool g_live_camera_valid = false;
std::atomic<bool> g_camera_control_requested{false};
std::atomic<bool> g_collision_disabled{false};
EternalSonataFieldCamera g_camera_target{};
uint32_t g_camera_target_object = 0;
bool g_camera_target_valid = false;
uint32_t g_controlled_camera_object = 0;
uint32_t g_controlled_camera_scene = 0;
uint32_t g_controlled_render_camera = 0;
uint32_t g_previous_camera_mode = 0;

// sub_82178A88 sets the scene transform used by field movement and rendering.
REX_IMPORT(__imp__sub_82178A88, g_set_scene_position,
           void(u32, u32, u32, u32, u32, u32));
REX_IMPORT(__imp__sub_8217BF28, g_get_scene_position,
           u32(u32, u32, u32));
REX_IMPORT(__imp__sub_82178BB0, g_set_scene_rotation,
           void(u32, u32, u32, u32, u32, u32));
REX_IMPORT(__imp__sub_8217BFA8, g_get_scene_rotation,
           u32(u32, u32, u32));
REX_IMPORT(__imp__sub_8217BED0, g_get_render_camera,
           u32(u32, u32));
// The node's forward axis, pulled out of its world matrix. sub_82190438, the
// battle side's reaction-cone test, measures facing with this rather than with
// the euler triple sub_8217BFA8 reports.
REX_IMPORT(__imp__sub_8217C368, g_get_scene_forward, u32(u32, u32, u32));

uint32_t PositionScratch() {
  auto* kernel = rex::system::kernel_state();
  if (!kernel || !kernel->memory()) {
    return 0;
  }
  if (!g_position_scratch) {
    g_position_scratch = kernel->memory()->SystemHeapAlloc(16, 0x20);
  }
  return g_position_scratch;
}

uint32_t FieldLeader() {
  auto* kernel = rex::system::kernel_state();
  if (!kernel || !kernel->memory()) {
    return 0;
  }
  auto* memory = kernel->memory();
  const auto* ptr = memory->TranslateVirtual<const uint8_t*>(
      kMapManager + kFieldLeaderOffset);
  return ptr ? rex::memory::load_and_swap<uint32_t>(ptr) : 0;
}

uint8_t* FieldLeaderHost(uint32_t* object_out = nullptr) {
  if (!g_runtime || !GetRoomPresence().IsFieldActive() ||
      GetRoomPresence().IsBattleActive()) {
    return nullptr;
  }
  const uint32_t object = FieldLeader();
  if (!object || object == 0xFFFFFFFFu) {
    return nullptr;
  }
  auto* host = rex::system::kernel_state()->memory()->TranslateVirtual<uint8_t*>(object);
  if (!host || rex::memory::load_and_swap<uint16_t>(host + 8) != 1) {
    return nullptr;
  }
  if (object_out) {
    *object_out = object;
  }
  return host;
}

uint8_t* FieldCameraHost(uint32_t* object_out = nullptr) {
  if (!g_runtime || !GetRoomPresence().IsFieldActive() ||
      GetRoomPresence().IsBattleActive()) {
    return nullptr;
  }
  auto* kernel = rex::system::kernel_state();
  if (!kernel || !kernel->memory()) {
    return nullptr;
  }
  auto* memory = kernel->memory();
  const auto* active = memory->TranslateVirtual<const uint8_t*>(
      kMapManager + kActiveCameraOffset);
  uint32_t object = active ? rex::memory::load_and_swap<uint32_t>(active) : 0;
  if (!object || object == 0xFFFFFFFFu) {
    const auto* field = memory->TranslateVirtual<const uint8_t*>(
        kMapManager + kFieldCameraOffset);
    object = field ? rex::memory::load_and_swap<uint32_t>(field) : 0;
  }
  if (!object || object == 0xFFFFFFFFu) {
    return nullptr;
  }
  auto* host = memory->TranslateVirtual<uint8_t*>(object);
  if (!host || host[20] != 4) {
    return nullptr;
  }
  if (object_out) {
    *object_out = object;
  }
  return host;
}

EternalSonataFieldPosition ReadVector(const uint8_t* host) {
  return {rex::memory::load_and_swap<float>(host),
          rex::memory::load_and_swap<float>(host + 4),
          rex::memory::load_and_swap<float>(host + 8)};
}

void WriteVector(uint8_t* host, EternalSonataFieldPosition vector) {
  rex::memory::store_and_swap<float>(host, vector.x);
  rex::memory::store_and_swap<float>(host + 4, vector.y);
  rex::memory::store_and_swap<float>(host + 8, vector.z);
}

void RestoreCameraMode() {
  if (!g_controlled_render_camera) {
    return;
  }
  const uint32_t render_camera = g_get_render_camera(
      kSceneManager, g_controlled_camera_scene);
  if (render_camera == g_controlled_render_camera) {
    auto* mode = rex::system::kernel_state()->memory()->TranslateVirtual<uint8_t*>(
        render_camera + 992);
    rex::memory::store_and_swap<uint32_t>(mode, g_previous_camera_mode);
  }
  g_controlled_camera_object = 0;
  g_controlled_camera_scene = 0;
  g_controlled_render_camera = 0;
}

void UpdateCameraControl() {
  uint32_t object = 0;
  auto* host = FieldCameraHost(&object);
  if (!host || !g_camera_control_requested.load()) {
    RestoreCameraMode();
    std::lock_guard<std::mutex> lock(g_position_mutex);
    g_camera_target_valid = false;
    if (!GetRoomPresence().IsFieldActive()) {
      g_camera_control_requested.store(false);
    }
    return;
  }
  const uint32_t scene = rex::memory::load_and_swap<uint32_t>(host + 4);
  if (!scene || scene == 0xFFFFFFFFu) {
    RestoreCameraMode();
    return;
  }
  if (g_controlled_camera_object != object ||
      g_controlled_camera_scene != scene) {
    RestoreCameraMode();
    const uint32_t render_camera = g_get_render_camera(kSceneManager, scene);
    if (!render_camera) {
      return;
    }
    g_controlled_camera_object = object;
    g_controlled_camera_scene = scene;
    g_controlled_render_camera = render_camera;
    auto* mode = rex::system::kernel_state()->memory()->TranslateVirtual<uint8_t*>(
        render_camera + 992);
    g_previous_camera_mode = rex::memory::load_and_swap<uint32_t>(mode);
  }
  auto* mode = rex::system::kernel_state()->memory()->TranslateVirtual<uint8_t*>(
      g_controlled_render_camera + 992);
  // Follow mode replaces the requested position when the view is built.
  rex::memory::store_and_swap<uint32_t>(mode, 0);
}

void RefreshFieldCamera() {
  UpdateCameraControl();
  uint32_t object = 0;
  auto* host = FieldCameraHost(&object);
  const uint32_t scratch = host ? PositionScratch() : 0;
  const uint32_t scene = host ? rex::memory::load_and_swap<uint32_t>(host + 4) : 0;
  if (!scratch || !scene || scene == 0xFFFFFFFFu) {
    std::lock_guard<std::mutex> lock(g_position_mutex);
    g_live_camera_valid = false;
    return;
  }
  auto* vec = rex::system::kernel_state()->memory()->TranslateVirtual<uint8_t*>(scratch);
  g_get_scene_position(scratch, kSceneManager, scene);
  EternalSonataFieldCamera camera{};
  camera.position = ReadVector(vec);
  g_get_scene_rotation(scratch, kSceneManager, scene);
  camera.rotation = ReadVector(vec);
  std::lock_guard<std::mutex> lock(g_position_mutex);
  g_live_camera = camera;
  g_live_camera_object = object;
  g_live_camera_valid = true;
  if (g_camera_control_requested.load() &&
      (!g_camera_target_valid || g_camera_target_object != object)) {
    g_camera_target = camera;
    g_camera_target_object = object;
    g_camera_target_valid = true;
  }
}

void RefreshFieldPosition() {
  uint32_t object = 0;
  auto* host = FieldLeaderHost(&object);
  const uint32_t scratch = host ? PositionScratch() : 0;
  const uint32_t scene = host ? rex::memory::load_and_swap<uint32_t>(host + 4) : 0;
  if (!scratch || !scene || scene == 0xFFFFFFFFu) {
    std::lock_guard<std::mutex> lock(g_position_mutex);
    g_live_valid = false;
  } else {
    g_get_scene_position(scratch, kSceneManager, scene);
    const auto* vec = rex::system::kernel_state()->memory()->TranslateVirtual<const uint8_t*>(
        scratch);
    EternalSonataFieldPosition position{};
    position.x = rex::memory::load_and_swap<float>(vec);
    position.y = rex::memory::load_and_swap<float>(vec + 4);
    position.z = rex::memory::load_and_swap<float>(vec + 8);
    EternalSonataFieldFacing facing{};
    g_get_scene_rotation(scratch, kSceneManager, scene);
    facing.rotation = ReadVector(vec);
    g_get_scene_forward(scratch, kSceneManager, scene);
    facing.forward = ReadVector(vec);
    facing.yaw = std::atan2(facing.forward.x, facing.forward.z);
    std::lock_guard<std::mutex> lock(g_position_mutex);
    g_live_position = position;
    g_live_facing = facing;
    g_live_object = object;
    g_live_valid = true;
  }
  RefreshFieldCamera();
  PostToGuestMainThread([] { RefreshFieldPosition(); });
}

void ApplyFieldPosition(EternalSonataFieldPosition position, std::string area_id) {
  if (GetRoomPresence().CurrentArea().id != area_id) {
    return;
  }
  uint32_t object = 0;
  auto* host = FieldLeaderHost(&object);
  if (!host) {
    return;
  }
  const uint32_t scene = rex::memory::load_and_swap<uint32_t>(host + 4);
  if (!scene || scene == 0xFFFFFFFFu) {
    return;
  }
  const uint32_t scratch = PositionScratch();
  if (!scratch) {
    return;
  }
  auto* vec = rex::system::kernel_state()->memory()->TranslateVirtual<uint8_t*>(scratch);
  rex::memory::store_and_swap<float>(vec, position.x);
  rex::memory::store_and_swap<float>(vec + 4, position.y);
  rex::memory::store_and_swap<float>(vec + 8, position.z);
  g_set_scene_position(kSceneManager, scene, scratch, 0, 0,
                       0xFFFFFFFFu);
  std::lock_guard<std::mutex> lock(g_position_mutex);
  g_live_position = position;
  g_live_object = object;
  g_live_valid = true;
}

void ApplyFieldFacing(EternalSonataFieldPosition rotation, std::string area_id) {
  if (GetRoomPresence().CurrentArea().id != area_id) {
    return;
  }
  uint32_t object = 0;
  auto* host = FieldLeaderHost(&object);
  if (!host) {
    return;
  }
  const uint32_t scene = rex::memory::load_and_swap<uint32_t>(host + 4);
  const uint32_t scratch = PositionScratch();
  if (!scratch || !scene || scene == 0xFFFFFFFFu) {
    return;
  }
  auto* vec = rex::system::kernel_state()->memory()->TranslateVirtual<uint8_t*>(scratch);
  WriteVector(vec, rotation);
  g_set_scene_rotation(kSceneManager, scene, scratch, 0, 0, 0xFFFFFFFFu);
  std::lock_guard<std::mutex> lock(g_position_mutex);
  g_live_facing.rotation = rotation;
}

void ApplyFieldCamera(EternalSonataFieldCamera camera, std::string area_id) {
  if (!g_camera_control_requested.load() ||
      GetRoomPresence().CurrentArea().id != area_id) {
    return;
  }
  UpdateCameraControl();
  uint32_t object = 0;
  auto* host = FieldCameraHost(&object);
  if (!host) {
    return;
  }
  const uint32_t scene = rex::memory::load_and_swap<uint32_t>(host + 4);
  const uint32_t scratch = PositionScratch();
  if (!scratch || !scene || scene == 0xFFFFFFFFu) {
    return;
  }
  auto* vec = rex::system::kernel_state()->memory()->TranslateVirtual<uint8_t*>(scratch);
  WriteVector(vec, camera.position);
  g_set_scene_position(kSceneManager, scene, scratch, 0, 0, 0xFFFFFFFFu);
  WriteVector(vec, camera.rotation);
  g_set_scene_rotation(kSceneManager, scene, scratch, 0, 0, 0xFFFFFFFFu);
  std::lock_guard<std::mutex> lock(g_position_mutex);
  g_live_camera = camera;
  g_live_camera_object = object;
  g_live_camera_valid = true;
  g_camera_target = camera;
  g_camera_target_object = object;
  g_camera_target_valid = true;
}

bool PrepareCameraUpdate(uint32_t object) {
  if (!g_camera_control_requested.load()) {
    return false;
  }
  uint32_t active_object = 0;
  auto* host = FieldCameraHost(&active_object);
  if (!host || active_object != object) {
    return false;
  }
  EternalSonataFieldCamera target{};
  {
    std::lock_guard<std::mutex> lock(g_position_mutex);
    if (!g_camera_target_valid || g_camera_target_object != object) {
      return false;
    }
    target = g_camera_target;
  }
  const uint32_t scene = rex::memory::load_and_swap<uint32_t>(host + 4);
  const uint32_t scratch = PositionScratch();
  if (!scratch || !scene || scene == 0xFFFFFFFFu) {
    return false;
  }
  auto* vec = rex::system::kernel_state()->memory()->TranslateVirtual<uint8_t*>(scratch);
  WriteVector(vec, target.position);
  g_set_scene_position(kSceneManager, scene, scratch, 0, 0, 0xFFFFFFFFu);
  WriteVector(vec, target.rotation);
  g_set_scene_rotation(kSceneManager, scene, scratch, 0, 0, 0xFFFFFFFFu);
  return true;
}

void CopyText(char* destination, size_t capacity, const std::string& source) {
  if (!capacity) {
    return;
  }
  const size_t count = std::min(capacity - 1, source.size());
  std::memcpy(destination, source.data(), count);
  destination[count] = 0;
}

EternalSonataOverworldEvent MakeEvent(int action) {
  EternalSonataOverworldEvent event{};
  event.action = action;
  const auto area = GetRoomPresence().CurrentArea();
  CopyText(event.area_id, sizeof(event.area_id), area.id);
  CopyText(event.area_name, sizeof(event.area_name), area.name);
  return event;
}

void Publish(const char* name, const EternalSonataOverworldEvent& event,
             uint64_t value) {
  if (!g_runtime || !g_runtime->mod_registry()) {
    return;
  }
  rex::system::ModRegistry::EventPayload payload;
  payload.u64 = value;
  payload.f64 = static_cast<double>(value);
  payload.bytes = std::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(&event), sizeof(event));
  g_runtime->mod_registry()->Publish(name, payload);
}

}  // namespace

void BindOverworldSystem(rex::Runtime* runtime) {
  g_runtime = runtime;
  PostToGuestMainThread([] { RefreshFieldPosition(); });
}

void NotifyOverworldAreaEntered(const char* area_id) {
  EternalSonataOverworldEvent event = MakeEvent(
      ETERNALSONATA_OVERWORLD_ACTION_ENTER_AREA);
  const auto area = GetRoomPresence().DescribeArea(area_id ? area_id : "");
  CopyText(event.area_id, sizeof(event.area_id), area.id);
  CopyText(event.area_name, sizeof(event.area_name), area.name);
  Publish(ETERNALSONATA_OVERWORLD_EVENT_AREA_ENTERED, event, 0);
}

void NotifyOverworldFieldAction(uint32_t object, int32_t animation) {
  if (animation < 16 || animation > 35 ||
      !GetRoomPresence().IsFieldLeader(object)) {
    return;
  }
  EternalSonataOverworldEvent event = MakeEvent(
      ETERNALSONATA_OVERWORLD_ACTION_FIELD_ANIMATION);
  event.character = FieldPlayerModelOverride::PartyLeaderCharacter();
  event.animation_id = animation;
  auto* memory = rex::system::kernel_state()->memory();
  const auto* host = memory->TranslateVirtual<const uint8_t*>(object);
  if (host) {
    event.field_object_id = rex::memory::load_and_swap<uint32_t>(host + 4);
    event.position_x = rex::memory::load_and_swap<float>(host + 32);
    event.position_y = rex::memory::load_and_swap<float>(host + 36);
    event.position_z = rex::memory::load_and_swap<float>(host + 40);
  }
  Publish(ETERNALSONATA_OVERWORLD_EVENT_ACTION, event,
          static_cast<uint64_t>(animation));
}

// A conversation line is just a SetText on the text manager window the field
// script owns, so `window` is published to tell concurrent windows apart. That
// box is reused, so object prompts arrive here too.
void NotifyOverworldDialogue(uint32_t window, const char* text) {
  if (!text || !GetRoomPresence().IsFieldActive() ||
      GetRoomPresence().IsBattleActive()) {
    return;
  }
  EternalSonataOverworldEvent event = MakeEvent(
      ETERNALSONATA_OVERWORLD_ACTION_START_DIALOGUE);
  event.character = FieldPlayerModelOverride::PartyLeaderCharacter();
  CopyText(event.dialogue, sizeof(event.dialogue), text);
  Publish(ETERNALSONATA_OVERWORLD_EVENT_DIALOGUE_STARTED, event, window);
}

void NotifyOverworldBattleStarted(uint32_t encounter, uint32_t music,
                                  uint32_t rule, uint32_t target) {
  EternalSonataOverworldEvent event = MakeEvent(
      ETERNALSONATA_OVERWORLD_ACTION_START_BATTLE);
  event.character = FieldPlayerModelOverride::PartyLeaderCharacter();
  event.encounter_id = static_cast<int32_t>(encounter);
  event.battle_music_id = static_cast<int32_t>(music);
  event.battle_rule_id = static_cast<int32_t>(rule);
  event.battle_target_id = static_cast<int32_t>(target);
  Publish(ETERNALSONATA_OVERWORLD_EVENT_BATTLE_STARTED, event, encounter);
}

}  // namespace eternalsonata

// Public C ABI (see eternalsonata_overworld_api.h). AreaNameTable() is an
// unordered_map; sort once for a stable, readable enumeration order.
namespace {

const std::vector<std::pair<std::string, std::string>>& SortedAreas() {
  static const std::vector<std::pair<std::string, std::string>> sorted = [] {
    std::vector<std::pair<std::string, std::string>> v;
    for (const auto& [id, name] : eternalsonata::AreaNameTable()) {
      v.emplace_back(id, name);
    }
    std::sort(v.begin(), v.end());
    return v;
  }();
  return sorted;
}

}  // namespace

using namespace eternalsonata;

REX_EXTERN(__imp__sub_820EF020);

REX_HOOK_RAW(sub_820EF020) {
  const uint32_t object = ctx.r3.u32;
  if (!PrepareCameraUpdate(object)) {
    __imp__sub_820EF020(ctx, base);
    return;
  }
  const uint32_t flags = REX_LOAD_U32(object + 12);
  const uint32_t timer = REX_LOAD_U32(object + 300);
  const uint32_t state = REX_LOAD_U32(object + 304);
  REX_STORE_U32(object + 12, flags & ~0x200000u);
  REX_STORE_U32(object + 300, 0);
  REX_STORE_U32(object + 304, 0);
  __imp__sub_820EF020(ctx, base);
  REX_STORE_U32(object + 304, state);
  REX_STORE_U32(object + 300, timer);
  REX_STORE_U32(object + 12, flags);
}

REX_EXTERN(__imp__sub_820E91D0);

// sub_820E91D0 is script native 1039: object.pos += vector. Map scripts use
// it for scripted pushes such as the ice slopes in bel01, as a task that loops
// "native1039(leader, &vec); sleep 1" with a constant vector, so the push is
// per frame while the player's own walk step is per 300/fps tick. At 60 fps
// the slope pushes twice as hard and walking against it crawls. Scale the
// vector to 30 fps when the call is followed by a one-frame sleep (bytes
// 86 01 01 7e: pop8, acc=1, sleep), which is what marks a per-frame loop.
// dword_824405FC is the VM context being run; ctx+0x20 is its ip, already
// past this call's operand.
constexpr uint32_t kRunningScriptVm = 0x824405FCu;
constexpr uint32_t kStockFieldFps = 30u;

REX_HOOK_RAW(sub_820E91D0) {
  const uint32_t args = ctx.r3.u32;
  const uint32_t fps = REX_LOAD_U8(0x82465F90);
  const uint32_t vm = REX_LOAD_U32(kRunningScriptVm);
  const uint32_t ip = vm ? REX_LOAD_U32(vm + 0x20) : 0;
  const uint32_t scratch = PositionScratch();
  if (fps == kStockFieldFps || fps == 0 || !ip || !scratch ||
      REX_LOAD_U32(ip) != 0x8601017Eu) {
    __imp__sub_820E91D0(ctx, base);
    return;
  }
  const uint32_t vec = REX_LOAD_U32(args + 4);
  const float scale = static_cast<float>(kStockFieldFps) / static_cast<float>(fps);
  for (uint32_t off = 0; off < 12; off += 4) {
    const float v = std::bit_cast<float>(REX_LOAD_U32(vec + off)) * scale;
    REX_STORE_U32(scratch + off, std::bit_cast<uint32_t>(v));
  }
  REX_STORE_U32(args + 4, scratch);
  __imp__sub_820E91D0(ctx, base);
  REX_STORE_U32(args + 4, vec);
}

REX_EXTERN(__imp__sub_8217DE48);

// sub_8217DE48 resolves a requested position against a scene object's collider:
// r5 is the request, r6 receives the result, and a non-zero return says it had
// to be clipped or clamped. Handing the request straight back is the whole of
// "no collisions", and it keeps the walk mesh query untouched, which the game
// also uses to place and light characters.
REX_HOOK_RAW(sub_8217DE48) {
  if (!g_collision_disabled.load(std::memory_order_relaxed) ||
      GetRoomPresence().IsBattleActive()) {
    __imp__sub_8217DE48(ctx, base);
    return;
  }
  const uint32_t requested = ctx.r5.u32;
  const uint32_t resolved = ctx.r6.u32;
  for (uint32_t offset = 0; offset < 12; offset += 4) {
    REX_STORE_U32(resolved + offset, REX_LOAD_U32(requested + offset));
  }
  ctx.r3.u64 = 0;
}

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t EternalSonataOverworldAbiVersion(void) {
  return ETERNALSONATA_OVERWORLD_ABI_VERSION;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetFieldPosition(
    EternalSonataFieldPosition* out) {
  if (!out) {
    return ETERNALSONATA_OVERWORLD_ERR_INVALID_ARGUMENT;
  }
  if (GetRoomPresence().IsBattleActive()) {
    return ETERNALSONATA_OVERWORLD_ERR_IN_BATTLE;
  }
  uint32_t object = 0;
  auto* host = FieldLeaderHost(&object);
  if (!host) {
    return ETERNALSONATA_OVERWORLD_ERR_UNAVAILABLE;
  }
  std::lock_guard<std::mutex> lock(g_position_mutex);
  if (!g_live_valid || g_live_object != object) {
    return ETERNALSONATA_OVERWORLD_ERR_UNAVAILABLE;
  }
  *out = g_live_position;
  return ETERNALSONATA_OVERWORLD_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSetFieldPosition(
    const EternalSonataFieldPosition* position) {
  if (!position || !std::isfinite(position->x) || !std::isfinite(position->y) ||
      !std::isfinite(position->z)) {
    return ETERNALSONATA_OVERWORLD_ERR_INVALID_ARGUMENT;
  }
  if (GetRoomPresence().IsBattleActive()) {
    return ETERNALSONATA_OVERWORLD_ERR_IN_BATTLE;
  }
  if (!FieldLeaderHost()) {
    return ETERNALSONATA_OVERWORLD_ERR_UNAVAILABLE;
  }
  const auto copy = *position;
  const auto area_id = GetRoomPresence().CurrentArea().id;
  PostToGuestMainThread([copy, area_id] { ApplyFieldPosition(copy, area_id); });
  return ETERNALSONATA_OVERWORLD_QUEUED;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetFieldFacing(
    EternalSonataFieldFacing* out) {
  if (!out) {
    return ETERNALSONATA_OVERWORLD_ERR_INVALID_ARGUMENT;
  }
  if (GetRoomPresence().IsBattleActive()) {
    return ETERNALSONATA_OVERWORLD_ERR_IN_BATTLE;
  }
  uint32_t object = 0;
  auto* host = FieldLeaderHost(&object);
  if (!host) {
    return ETERNALSONATA_OVERWORLD_ERR_UNAVAILABLE;
  }
  std::lock_guard<std::mutex> lock(g_position_mutex);
  if (!g_live_valid || g_live_object != object) {
    return ETERNALSONATA_OVERWORLD_ERR_UNAVAILABLE;
  }
  *out = g_live_facing;
  return ETERNALSONATA_OVERWORLD_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSetFieldFacing(
    const EternalSonataFieldPosition* rotation) {
  if (!rotation || !std::isfinite(rotation->x) || !std::isfinite(rotation->y) ||
      !std::isfinite(rotation->z)) {
    return ETERNALSONATA_OVERWORLD_ERR_INVALID_ARGUMENT;
  }
  if (GetRoomPresence().IsBattleActive()) {
    return ETERNALSONATA_OVERWORLD_ERR_IN_BATTLE;
  }
  if (!FieldLeaderHost()) {
    return ETERNALSONATA_OVERWORLD_ERR_UNAVAILABLE;
  }
  const auto copy = *rotation;
  const auto area_id = GetRoomPresence().CurrentArea().id;
  PostToGuestMainThread([copy, area_id] { ApplyFieldFacing(copy, area_id); });
  return ETERNALSONATA_OVERWORLD_QUEUED;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSetFieldCollisionEnabled(int enabled) {
  if (enabled != 0 && enabled != 1) {
    return ETERNALSONATA_OVERWORLD_ERR_INVALID_ARGUMENT;
  }
  if (GetRoomPresence().IsBattleActive()) {
    return ETERNALSONATA_OVERWORLD_ERR_IN_BATTLE;
  }
  g_collision_disabled.store(enabled == 0);
  return ETERNALSONATA_OVERWORLD_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataIsFieldCollisionEnabled(void) {
  return g_collision_disabled.load() ? 0 : 1;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetFieldCamera(
    EternalSonataFieldCamera* out) {
  if (!out) {
    return ETERNALSONATA_OVERWORLD_ERR_INVALID_ARGUMENT;
  }
  if (GetRoomPresence().IsBattleActive()) {
    return ETERNALSONATA_OVERWORLD_ERR_IN_BATTLE;
  }
  uint32_t object = 0;
  if (!FieldCameraHost(&object)) {
    return ETERNALSONATA_OVERWORLD_ERR_UNAVAILABLE;
  }
  std::lock_guard<std::mutex> lock(g_position_mutex);
  if (!g_live_camera_valid || g_live_camera_object != object) {
    return ETERNALSONATA_OVERWORLD_ERR_UNAVAILABLE;
  }
  *out = g_live_camera;
  return ETERNALSONATA_OVERWORLD_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSetFieldCameraControl(int enabled) {
  if (enabled != 0 && enabled != 1) {
    return ETERNALSONATA_OVERWORLD_ERR_INVALID_ARGUMENT;
  }
  if (enabled && GetRoomPresence().IsBattleActive()) {
    return ETERNALSONATA_OVERWORLD_ERR_IN_BATTLE;
  }
  if (enabled && !FieldCameraHost()) {
    return ETERNALSONATA_OVERWORLD_ERR_UNAVAILABLE;
  }
  g_camera_control_requested.store(enabled != 0);
  PostToGuestMainThread([] { UpdateCameraControl(); });
  return ETERNALSONATA_OVERWORLD_QUEUED;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSetFieldCamera(
    const EternalSonataFieldCamera* camera) {
  if (!camera || !std::isfinite(camera->position.x) ||
      !std::isfinite(camera->position.y) || !std::isfinite(camera->position.z) ||
      !std::isfinite(camera->rotation.x) || !std::isfinite(camera->rotation.y) ||
      !std::isfinite(camera->rotation.z)) {
    return ETERNALSONATA_OVERWORLD_ERR_INVALID_ARGUMENT;
  }
  if (GetRoomPresence().IsBattleActive()) {
    return ETERNALSONATA_OVERWORLD_ERR_IN_BATTLE;
  }
  if (!g_camera_control_requested.load() || !FieldCameraHost()) {
    return ETERNALSONATA_OVERWORLD_ERR_UNAVAILABLE;
  }
  const auto copy = *camera;
  const auto area_id = GetRoomPresence().CurrentArea().id;
  PostToGuestMainThread([copy, area_id] { ApplyFieldCamera(copy, area_id); });
  return ETERNALSONATA_OVERWORLD_QUEUED;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataIsAreaLoadingAvailable(void) {
  return !GetRoomPresence().IsBattleActive() ? 1 : 0;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataForceLoadArea(const char* area_id) {
  if (!area_id || area_id[0] == '\0') {
    return ETERNALSONATA_OVERWORLD_ERR_INVALID_ARGUMENT;
  }
  if (GetRoomPresence().IsBattleActive()) {
    return ETERNALSONATA_OVERWORLD_ERR_IN_BATTLE;
  }
  GetForceLoadArea().Request(area_id);
  return ETERNALSONATA_OVERWORLD_QUEUED;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetAreaCount(void) {
  return static_cast<int>(SortedAreas().size());
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetAreaInfo(int index, char* id_out, int id_len,
                                                             char* name_out, int name_len) {
  const auto& areas = SortedAreas();
  if (index < 0 || index >= static_cast<int>(areas.size())) {
    return ETERNALSONATA_OVERWORLD_ERR_INVALID_ARGUMENT;
  }
  if (id_out && id_len > 0) {
    const std::string& id = areas[index].first;
    const size_t n = std::min(static_cast<size_t>(id_len - 1), id.size());
    std::memcpy(id_out, id.data(), n);
    id_out[n] = '\0';
  }
  if (name_out && name_len > 0) {
    const std::string& name = areas[index].second;
    const size_t n = std::min(static_cast<size_t>(name_len - 1), name.size());
    std::memcpy(name_out, name.data(), n);
    name_out[n] = '\0';
  }
  return ETERNALSONATA_OVERWORLD_OK;
}
