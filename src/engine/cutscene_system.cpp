// eternalsonata - Cutscene state: observing the game's events, and skipping.
//
// Everything here was derived from the retail xex. The short version:
//
//   * A cutscene is an "event": a map script loads an E%04d.e file and starts
//     it through script native 1011 (sub_820E8968), which records the event's
//     script handle in dword_8243C240 and stops the field camera. Native 1012
//     (sub_820E8A10) ends it through sub_820FC9F8, which clears the handle and
//     hands the camera back to the field. The field stays loaded throughout.
//
//   * Skipping is declared by the script. lib.e's helper calls native 1013
//     (sub_820E8A48) to set byte_82440579 (1: pause then A, 2: any button)
//     and stores its skip handler function in dword_8243C34C. Both are
//     cleared by sub_820FC9F8, so an unskippable scene simply has neither.
//
//   * The player's skip lives in the field tick, sub_820FE7F8: paused during
//     an event, A spawns the handler as a VM task (sub_82101E08 on the map
//     script, priority 9, 512 byte stack) into dword_8243C350 and flips the
//     pause request byte at map+1492. The pause state machine, sub_820FA2F8,
//     then sees an unpause with that task alive and does the actual skip:
//     sub_821020C0(event, 1) suspends every task of the event script,
//     byte_8243C345 is set (lib.e polls it, as sym537, to run the handler's
//     continuation), and sub_820F8378 stops the BGM and the voice line.
//
// EternalSonataSkipCutscene repeats those steps. Unpaused, it spawns the
// handler and does the unpause branch's work itself; paused, it spawns the
// handler and clears the pause request, and the game's own path finishes.
//
//   * Camera. Native 1010 (sub_820E8D10) makes a script object the active
//     camera, dword_8244BEA8, which the overworld camera API then drives.
//     Every event script uses a single camera object (id 3) and animates it,
//     so there is one camera to control per scene.

#include "cutscene_system.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <span>
#include <string>

#include <rex/hook.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/mod_plugin.h>
#include <rex/system/mod_registry.h>

#include "eternalsonata_cutscene_api.h"
#include "guest_main_thread.h"
#include "overworld_system.h"
#include "room_presence.h"
#include "generated/eternalsonata_init.h"

namespace eternalsonata {
namespace {

rex::Runtime* g_runtime = nullptr;

constexpr uint32_t kMapManager = 0x8244B4B0u;
// Pause request, toggled by Start (and by A/B while paused in an event).
constexpr uint32_t kPauseRequestOffset = 1492u;
// Applied pause state; bit 0 is the request above, bit 1 a menu.
constexpr uint32_t kPauseStateOffset = 1513u;

constexpr uint32_t kMapScript = 0x8243C23Cu;
constexpr uint32_t kEventTask = 0x8243C240u;
constexpr uint32_t kSkipRequested = 0x8243C345u;
constexpr uint32_t kSkipHandler = 0x8243C34Cu;
constexpr uint32_t kSkipTask = 0x8243C350u;
constexpr uint32_t kSkipMode = 0x82440579u;
// Id written by the generic cfdata loader, "E1010.e" for an event.
constexpr uint32_t kLoadedId = 0x8244B500u;
constexpr uint32_t kLoadedIdMaxLength = 32u;


constexpr uint32_t kSkipHandlerPriority = 9u;
constexpr uint32_t kSkipHandlerStack = 512u;

// Latched when a skip is seen (ours or the player's) so `ended` can report it.
std::atomic<bool> g_skip_seen{false};
uint32_t g_published_event = 0;

REX_IMPORT(__imp__sub_82101E08, g_spawn_task,
           u32(u32, u32, u32, u32, u32, u32));
REX_IMPORT(__imp__sub_821020C0, g_set_script_suspended, void(u32, u32));
REX_IMPORT(__imp__sub_820F8378, g_stop_audio, void(u32, double));

uint8_t* Host(uint32_t address) {
  auto* kernel = rex::system::kernel_state();
  if (!g_runtime || !kernel || !kernel->memory()) {
    return nullptr;
  }
  return kernel->memory()->TranslateVirtual<uint8_t*>(address);
}

uint32_t LoadU32(uint32_t address) {
  auto* host = Host(address);
  return host ? rex::memory::load_and_swap<uint32_t>(host) : 0;
}

uint8_t LoadU8(uint32_t address) {
  auto* host = Host(address);
  return host ? *host : 0;
}


void StoreU32(uint32_t address, uint32_t value) {
  if (auto* host = Host(address)) {
    rex::memory::store_and_swap<uint32_t>(host, value);
  }
}

void StoreU8(uint32_t address, uint8_t value) {
  if (auto* host = Host(address)) {
    *host = value;
  }
}

void CopyText(char* destination, size_t capacity, const std::string& source) {
  if (!capacity) {
    return;
  }
  const size_t count = std::min(capacity - 1, source.size());
  std::memcpy(destination, source.data(), count);
  destination[count] = 0;
}

std::string LoadedScriptId() {
  const auto* host = Host(kLoadedId);
  if (!host) {
    return {};
  }
  std::string raw;
  for (uint32_t i = 0; i < kLoadedIdMaxLength && host[i]; ++i) {
    raw.push_back(static_cast<char>(host[i]));
  }
  return GetRoomPresence().DescribeArea(raw).id;
}

EternalSonataCutsceneEvent MakeEvent(uint32_t handle) {
  EternalSonataCutsceneEvent event{};
  event.handle = handle;
  event.skip_mode = LoadU8(kSkipMode);
  CopyText(event.script_id, sizeof(event.script_id), LoadedScriptId());
  CopyText(event.area_id, sizeof(event.area_id),
           GetRoomPresence().CurrentArea().id);
  return event;
}

void Publish(const char* name, const EternalSonataCutsceneEvent& event) {
  if (!g_runtime || !g_runtime->mod_registry()) {
    return;
  }
  rex::system::ModRegistry::EventPayload payload;
  payload.u64 = event.handle;
  payload.f64 = static_cast<double>(event.skipped);
  payload.bytes = std::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(&event), sizeof(event));
  g_runtime->mod_registry()->Publish(name, payload);
}

int TranslateOverworldStatus(int status) {
  switch (status) {
    case ETERNALSONATA_OVERWORLD_OK:
      return ETERNALSONATA_CUTSCENE_OK;
    case ETERNALSONATA_OVERWORLD_QUEUED:
      return ETERNALSONATA_CUTSCENE_QUEUED;
    case ETERNALSONATA_OVERWORLD_ERR_INVALID_ARGUMENT:
      return ETERNALSONATA_CUTSCENE_ERR_INVALID_ARGUMENT;
    default:
      return ETERNALSONATA_CUTSCENE_ERR_UNAVAILABLE;
  }
}

// The player's skip leaves no host-visible edge of its own, so the handler
// task slot is sampled once a frame instead.
void Tick() {
  if (LoadU32(kEventTask) && LoadU32(kSkipTask)) {
    g_skip_seen.store(true);
  }
  PostToGuestMainThread([] { Tick(); });
}

void SkipOnGuestThread() {
  const uint32_t event = LoadU32(kEventTask);
  const uint32_t handler = LoadU32(kSkipHandler);
  if (!event || !handler || !LoadU8(kSkipMode) || LoadU32(kSkipTask)) {
    return;
  }
  const uint32_t task = g_spawn_task(LoadU32(kMapScript), handler, 0,
                                     kSkipHandlerPriority, kSkipHandlerStack, 0);
  if (!task) {
    return;
  }
  StoreU32(kSkipTask, task);
  g_skip_seen.store(true);
  if (LoadU8(kMapManager + kPauseStateOffset) & 1) {
    StoreU8(kMapManager + kPauseRequestOffset, 0);
    return;
  }
  g_set_script_suspended(event, 1);
  StoreU8(kSkipRequested, 1);
  g_stop_audio(0, 0.0);
  g_stop_audio(1, 0.0);
}

}  // namespace

void BindCutsceneSystem(rex::Runtime* runtime) {
  g_runtime = runtime;
  PostToGuestMainThread([] { Tick(); });
}

bool IsCutsceneActive() {
  return LoadU32(kEventTask) != 0;
}

}  // namespace eternalsonata

using namespace eternalsonata;

REX_EXTERN(__imp__sub_820E8968);

// Script native 1011: start the event whose handle is the first argument.
REX_HOOK_RAW(sub_820E8968) {
  const uint32_t before = REX_LOAD_U32(kEventTask);
  __imp__sub_820E8968(ctx, base);
  const uint32_t after = REX_LOAD_U32(kEventTask);
  if (after && after != before && after != g_published_event) {
    g_published_event = after;
    g_skip_seen.store(false);
    Publish(ETERNALSONATA_CUTSCENE_EVENT_STARTED, MakeEvent(after));
  }
}

REX_EXTERN(__imp__sub_820FC9F8);

// Ends the running event, if any. Also runs on every field load, so only an
// event that was actually running is reported.
REX_HOOK_RAW(sub_820FC9F8) {
  const uint32_t before = REX_LOAD_U32(kEventTask);
  EternalSonataCutsceneEvent event{};
  if (before) {
    event = MakeEvent(before);
    event.skipped = (g_skip_seen.load() || REX_LOAD_U32(kSkipTask)) ? 1 : 0;
  }
  __imp__sub_820FC9F8(ctx, base);
  if (before) {
    g_published_event = 0;
    g_skip_seen.store(false);
    Publish(ETERNALSONATA_CUTSCENE_EVENT_ENDED, event);
  }
}

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t EternalSonataCutsceneAbiVersion(void) {
  return ETERNALSONATA_CUTSCENE_ABI_VERSION;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataIsCutsceneActive(void) {
  return IsCutsceneActive() ? 1 : 0;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetCutsceneInfo(
    EternalSonataCutsceneInfo* out) {
  if (!out) {
    return ETERNALSONATA_CUTSCENE_ERR_INVALID_ARGUMENT;
  }
  if (!g_runtime) {
    return ETERNALSONATA_CUTSCENE_ERR_UNAVAILABLE;
  }
  *out = {};
  const uint32_t event = LoadU32(kEventTask);
  out->active = event ? 1 : 0;
  out->handle = event;
  out->paused = (LoadU8(kMapManager + kPauseStateOffset) & 1) ? 1 : 0;
  if (!event) {
    return ETERNALSONATA_CUTSCENE_OK;
  }
  out->skip_mode = LoadU32(kSkipHandler) ? LoadU8(kSkipMode)
                                         : ETERNALSONATA_CUTSCENE_SKIP_NONE;
  out->skipping = LoadU32(kSkipTask) ? 1 : 0;
  CopyText(out->script_id, sizeof(out->script_id), LoadedScriptId());
  CopyText(out->area_id, sizeof(out->area_id),
           GetRoomPresence().CurrentArea().id);
  out->camera_controlled = IsCutsceneCameraControlled() ? 1 : 0;
  return ETERNALSONATA_CUTSCENE_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetCutsceneCamera(
    EternalSonataFieldCamera* out) {
  if (!out) {
    return ETERNALSONATA_CUTSCENE_ERR_INVALID_ARGUMENT;
  }
  if (!IsCutsceneActive()) {
    return ETERNALSONATA_CUTSCENE_ERR_NOT_ACTIVE;
  }
  return TranslateOverworldStatus(ReadActiveCamera(out));
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSetCutsceneCameraControl(
    int enabled) {
  if (enabled != 0 && enabled != 1) {
    return ETERNALSONATA_CUTSCENE_ERR_INVALID_ARGUMENT;
  }
  if (enabled && !IsCutsceneActive()) {
    return ETERNALSONATA_CUTSCENE_ERR_NOT_ACTIVE;
  }
  return TranslateOverworldStatus(RequestCutsceneCameraControl(enabled));
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSetCutsceneCamera(
    const EternalSonataFieldCamera* camera) {
  if (!IsCutsceneCameraControlled()) {
    return IsCutsceneActive() ? ETERNALSONATA_CUTSCENE_ERR_UNAVAILABLE
                              : ETERNALSONATA_CUTSCENE_ERR_NOT_ACTIVE;
  }
  return TranslateOverworldStatus(QueueActiveCamera(camera));
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSkipCutscene(void) {
  if (!g_runtime) {
    return ETERNALSONATA_CUTSCENE_ERR_UNAVAILABLE;
  }
  if (!LoadU32(kEventTask)) {
    return ETERNALSONATA_CUTSCENE_ERR_NOT_ACTIVE;
  }
  if (!LoadU8(kSkipMode) || !LoadU32(kSkipHandler)) {
    return ETERNALSONATA_CUTSCENE_ERR_NOT_SKIPPABLE;
  }
  if (LoadU32(kSkipTask)) {
    return ETERNALSONATA_CUTSCENE_ERR_ALREADY_SKIPPING;
  }
  if (OnGuestMainThread()) {
    SkipOnGuestThread();
  } else {
    PostToGuestMainThread([] { SkipOnGuestThread(); });
  }
  return ETERNALSONATA_CUTSCENE_QUEUED;
}
