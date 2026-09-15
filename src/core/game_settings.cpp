// eternalsonata - The game's own Options settings: reading, writing and
// announcing them, plus the mod-facing API.
//
// Everything here was derived from the retail xex; docs/settings.md is the
// long form. The short version:
//
//   * sub_821E7090 resets every one of them in fifteen lines, which is what
//     shows the block is exactly 0x8243FBF8..0x8243FC06 and nothing else. The
//     same block is written verbatim by sub_821E5A38 (new game), sub_821E5F58
//     and sub_821DD4D0 (title-screen resets) and sub_820FDFC0.
//
//   * The labels come from the Options display list at 0x8202F388 read against
//     the executable's own BTX blob at 0x82031A00: text id 40 "Battle Camera"
//     with ON/OFF beside the bar sub_82200FE8 places from byte_8243FBFC, id 35
//     "Attack Button" with A/B beside byte_8243FC01, id 37 "Volume" over the
//     three sliders "Music" / "Sound Effects" / "Voice", id 201 "Subtitles"
//     and id 42 "Voice" with English/Japanese. So the volume channels are
//     Music, Sound Effects, Voice in address order, which also matches
//     sub_821E6EA8's channel 1 being the one that touches five mixer bytes.
//
//   * byte_8243FBFD is the odd one out: three values, no row on any display
//     list, and its Stereo / Mono / 5.1ch Surround strings (text ids 134..136)
//     are referenced by nothing. It is the cut Audio Output setting; it still
//     round-trips through the menu scratch and the save, so it is exposed.
//
//   * byte_8243FBF8[0..2] are the controller ports of players 1..3, 0..3 each.
//     sub_82202CB0 is the player-controls screen and reassigns any player
//     whose port has no pad, so this is the one setting the game will overrule.
//
// The whole block sits inside the 2324 bytes sub_82241190 writes from
// 0x8243F3E8, so all of it is saved and the save/load hook already covers it.
//
// Threading. The exported entry points are called from mods, i.e. usually from
// the ImGui draw thread. Everything is a plain guest-memory load or store
// except the volumes, which must go through the game's mixer routine and so go
// through guest_main_thread.h and answer QUEUED. The events are published from
// the mod registry's frame tick.

#include "generated/eternalsonata_init.h"

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include <rex/hook.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/mod_plugin.h>
#include <rex/system/mod_registry.h>

#include "eternalsonata_settings_api.h"
#include "game_settings.h"
#include "guest_main_thread.h"

namespace eternalsonata {
namespace {

// ---------------------------------------------------------------------------
// Guest addresses
// ---------------------------------------------------------------------------

constexpr uint32_t kControllerP1 = 0x8243FBF8u;  // and +1, +2 for players 2/3
constexpr uint32_t kBattleCamera = 0x8243FBFCu;
constexpr uint32_t kAudioOutput = 0x8243FBFDu;
constexpr uint32_t kVolumeMusic = 0x8243FBFEu;   // and +1, +2: SFX, Voice
constexpr uint32_t kAttackButton = 0x8243FC01u;
constexpr uint32_t kSubtitles = 0x8243FC05u;     // BYTE1(dword_8243FC04)
constexpr uint32_t kVoiceLanguage = 0x8243FC06u; // BYTE2, 0 = Japanese

// The whole block, for the readability probe.
constexpr uint32_t kBlockStart = kControllerP1;
constexpr uint32_t kBlockBytes = (kVoiceLanguage - kControllerP1) + 1u;

// sub_821E6EA8(channel, percent): scales percent to the mixer's 0..127, writes
// it to every mixer byte the channel owns and to the setting byte, then
// commits with sub_82141FE0. Writing the setting byte alone would only change
// what the menu draws.
REX_IMPORT(__imp__sub_821E6EA8, g_set_volume, u32(u32, u32));

constexpr int kSettingCount = ETERNALSONATA_SETTING_COUNT;

// ---------------------------------------------------------------------------
// Host state
// ---------------------------------------------------------------------------

std::mutex g_mutex;  // guards everything below
rex::Runtime* g_runtime = nullptr;

// Last observed values, for the event poll. `g_have_snapshot` is false before
// the first tick and after a load, which is what makes a restored save adopt
// silently instead of republishing everything it holds.
bool g_have_snapshot = false;
std::array<int, kSettingCount> g_snapshot{};

rex::memory::Memory* Mem() { return g_runtime ? g_runtime->memory() : nullptr; }

bool Readable(uint32_t address, uint32_t span) {
  auto* memory = Mem();
  if (!memory) {
    return false;
  }
  auto* heap = memory->LookupHeap(address);
  return heap && heap->QueryRangeAccess(address, address + span - 1) !=
                     rex::memory::PageAccess::kNoAccess;
}

bool BlockReadable() { return Readable(kBlockStart, kBlockBytes); }

uint8_t ReadGuestByte(uint32_t address) {
  auto* memory = Mem();
  if (!memory) {
    return 0;
  }
  auto* host = memory->TranslateVirtual<const uint8_t*>(address);
  return host ? *host : uint8_t{0};
}

void WriteGuestByte(uint32_t address, uint8_t value) {
  auto* memory = Mem();
  if (!memory) {
    return;
  }
  auto* host = memory->TranslateVirtual<uint8_t*>(address);
  if (host) {
    *host = value;
  }
}

// ---------------------------------------------------------------------------
// The settings table
// ---------------------------------------------------------------------------

struct SettingInfo {
  uint32_t address;
  int min;
  int max;
  // Volume channel for sub_821E6EA8, or -1 for a setting that is a plain byte.
  int volume_channel;
};

constexpr SettingInfo kSettings[kSettingCount] = {
    /* BATTLE_CAMERA  */ {kBattleCamera, 0, 1, -1},
    /* ATTACK_BUTTON  */ {kAttackButton, 0, 1, -1},
    /* VOLUME_MUSIC   */ {kVolumeMusic + 0u, 0, 100, 0},
    /* VOLUME_SFX     */ {kVolumeMusic + 1u, 0, 100, 1},
    /* VOLUME_VOICE   */ {kVolumeMusic + 2u, 0, 100, 2},
    /* SUBTITLES      */ {kSubtitles, 0, 1, -1},
    /* VOICE_LANGUAGE */ {kVoiceLanguage, 0, 1, -1},
    /* AUDIO_OUTPUT   */ {kAudioOutput, 0, 2, -1},
    /* CONTROLLER_P1  */ {kControllerP1 + 0u, 0, 3, -1},
    /* CONTROLLER_P2  */ {kControllerP1 + 1u, 0, 3, -1},
    /* CONTROLLER_P3  */ {kControllerP1 + 2u, 0, 3, -1},
};

bool ValidSetting(int setting) {
  return setting >= 0 && setting < kSettingCount;
}

// Audio Output's stored byte is not its menu index: sub_82200FE8 turns the
// byte into a column with 0 -> 1, 1 -> 0, 2 -> 2 and anything else -> 1, and
// sub_82201DA8 inverts that on the way back out. The API speaks the column,
// because that is the order the three names are in.
int AudioOutputFromByte(uint8_t raw) {
  switch (raw) {
    case 1:
      return 0;
    case 2:
      return 2;
    default:
      return 1;
  }
}

uint8_t AudioOutputToByte(int value) {
  switch (value) {
    case 0:
      return 1;
    case 2:
      return 2;
    default:
      return 0;
  }
}

// Caller holds g_mutex and has checked BlockReadable().
int ReadSetting(int setting) {
  const uint8_t raw = ReadGuestByte(kSettings[setting].address);
  if (setting == ETERNALSONATA_SETTING_AUDIO_OUTPUT) {
    return AudioOutputFromByte(raw);
  }
  const auto& info = kSettings[setting];
  int value = static_cast<int>(raw);
  // The bytes are whatever the guest last stored; clamp rather than hand a mod
  // a value outside the range it was told to expect.
  if (value < info.min) {
    value = info.min;
  }
  if (value > info.max) {
    value = info.max;
  }
  return value;
}

// ---------------------------------------------------------------------------
// Volumes
// ---------------------------------------------------------------------------

// Runs `work` where guest calls are legal: right here if we are already on the
// guest main thread, otherwise on its next frame. See guest_main_thread.h.
int RunOnGuestThread(std::function<int()> work) {
  if (OnGuestMainThread()) {
    return work();
  }
  PostToGuestMainThread([work] { work(); });
  return ETERNALSONATA_SETTING_QUEUED;
}

// Guest thread only.
int SetVolumeOnGuestThread(int channel, int percent) {
  g_set_volume(static_cast<uint32_t>(channel), static_cast<uint32_t>(percent));
  return ETERNALSONATA_SETTING_OK;
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

// Published on the shared mod registry bus rather than through a callback list
// of our own, so a mod subscribes by name with nothing linked. Called with
// g_mutex NOT held: a subscriber may call straight back into this file from
// its handler.
void PublishChanged(int setting, int value) {
  rex::Runtime* runtime = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    runtime = g_runtime;
  }
  if (!runtime) {
    return;
  }
  auto* registry = runtime->mod_registry();
  if (!registry) {
    return;
  }
  rex::system::ModRegistry::EventPayload payload;
  payload.u64 = static_cast<uint64_t>(setting);
  payload.f64 = static_cast<double>(value);
  registry->Publish(ETERNALSONATA_SETTING_EVENT_CHANGED, payload);
}

// Runs once per guest frame off the mod registry's tick.
void Tick() {
  std::vector<std::pair<int, int>> changed;

  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!BlockReadable()) {
      // Guest memory went away under us (shutdown); start clean next time.
      g_have_snapshot = false;
      return;
    }

    std::array<int, kSettingCount> state{};
    for (int i = 0; i < kSettingCount; ++i) {
      state[static_cast<size_t>(i)] = ReadSetting(i);
    }

    if (g_have_snapshot) {
      for (int i = 0; i < kSettingCount; ++i) {
        const auto slot = static_cast<size_t>(i);
        if (state[slot] != g_snapshot[slot]) {
          changed.emplace_back(i, state[slot]);
        }
      }
    }

    g_snapshot = state;
    g_have_snapshot = true;
  }

  for (const auto& [setting, value] : changed) {
    PublishChanged(setting, value);
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Internal interface
// ---------------------------------------------------------------------------

void BindGameSettings(rex::Runtime* runtime) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_runtime = runtime;
    g_have_snapshot = false;
  }
  if (runtime && runtime->mod_registry()) {
    runtime->mod_registry()->RegisterTick([] { Tick(); });
  }
}

void NotifyGameSettingsSaveLoaded() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_have_snapshot = false;
}

}  // namespace eternalsonata

// ---------------------------------------------------------------------------
// Public C ABI (eternalsonata_settings_api.h)
// ---------------------------------------------------------------------------

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t EternalSonataSettingsAbiVersion(void) {
  return ETERNALSONATA_SETTINGS_ABI_VERSION;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetSetting(int setting) {
  using namespace eternalsonata;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!ValidSetting(setting)) {
    return ETERNALSONATA_SETTING_ERR_INVALID_SETTING;
  }
  if (!BlockReadable()) {
    return ETERNALSONATA_SETTING_ERR_UNAVAILABLE;
  }
  return ReadSetting(setting);
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetSettingRange(int setting, int* min,
                                                                 int* max) {
  using namespace eternalsonata;
  if (!ValidSetting(setting)) {
    return ETERNALSONATA_SETTING_ERR_INVALID_SETTING;
  }
  if (min) {
    *min = kSettings[setting].min;
  }
  if (max) {
    *max = kSettings[setting].max;
  }
  return ETERNALSONATA_SETTING_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSetSetting(int setting, int value) {
  using namespace eternalsonata;
  int channel = -1;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!ValidSetting(setting)) {
      return ETERNALSONATA_SETTING_ERR_INVALID_SETTING;
    }
    const auto& info = kSettings[setting];
    if (value < info.min || value > info.max) {
      return ETERNALSONATA_SETTING_ERR_INVALID_VALUE;
    }
    if (!BlockReadable()) {
      return ETERNALSONATA_SETTING_ERR_UNAVAILABLE;
    }
    if (info.volume_channel < 0) {
      const uint8_t raw = setting == ETERNALSONATA_SETTING_AUDIO_OUTPUT
                              ? AudioOutputToByte(value)
                              : static_cast<uint8_t>(value);
      WriteGuestByte(info.address, raw);
      return ETERNALSONATA_SETTING_OK;
    }
    channel = info.volume_channel;
  }
  // Outside the lock: the work may run here and there is guest code in it.
  return RunOnGuestThread(
      [channel, value] { return SetVolumeOnGuestThread(channel, value); });
}
