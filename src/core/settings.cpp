// eternalsonata - ReXGlue Recompiled Project
// See settings.h for details.

#include "settings.h"

#include "eternalsonata_options_api.h"
#include "eternalsonata_settings_api.h"

// Exported by game_settings.cpp for mods; the overlay uses the same entry points.
extern "C" int EternalSonataGetSetting(int setting);
extern "C" int EternalSonataSetSetting(int setting, int value);
#include "field_player_model_override.h"
#include "game_settings.h"
#include "host_timer_resolution.h"
#include "native_renderer.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdlib>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/input_system.h>
#include <rex/platform.h>
#include <rex/platform/process.h>
#include <rex/system/auto_updater.h>
#include <rex/system/mod_registry.h>
#include <rex/ui/imgui_widgets.h>
#include <rex/ui/overlay/settings_overlay.h>
#include <rex/ui/window.h>
#include <imgui.h>

#if REX_HAS_VULKAN
#include <rex/ui/vulkan/provider.h>
#endif

// In-game frame rate (see DrawFrameRateRow). The engine is frame clocked: the
// sim advances `300 / byte_82465F90` units per present and nothing reads the
// wall clock, so a fixed rate is only exact when the step is an integer and
// the host actually holds the rate. 30 and 60 are the two the content was
// paced for; the host limiter in eternalsonata_framerate.cpp pins them.
//
// "unlocked" runs without a limiter at correct speed: the byte is re-chosen
// every frame from the measured frame time (an integer step that keeps the
// sim on the wall clock on average) and the float delta getter is corrected
// to the exact value. See the wall-clock section of eternalsonata_framerate.cpp
// for what that patches and why a single byte above 60 can't do it.
//
// "adaptive" frame-skips (see AdaptiveFrameRate): it targets 60, and if the
// host can't sustain it the declared rate drops to 30 and the game skips
// frames at correct speed rather than running the sim in slow motion, then
// climbs back once there is headroom.
//
// "60" is the same target pinned: it never steps down, so a host that can't
// keep up runs the sim in slow motion instead. Worth keeping as its own value
// because a false step-down is more visible than mild slow motion. This was a
// separate adaptive_framerate bool until the two menus folded it in here: as a
// pair the two cvars only ever had these four states, and one ordered list is
// what both the overlay slider and the native Options row can draw. A config
// written before that carries frame_rate = 60 and reads back as pinned 60.
//
// Declared here (global scope) so the hooks can read it via
// REXCVAR_DECLARE/REXCVAR_GET.
REXCVAR_DEFINE_STRING(frame_rate, "30", "Eternal Sonata",
                      "In-game scene/sim advance rate: 30 (as the game asks - 30 for gameplay, 60 "
                      "on the title and the save menu), 60, adaptive, or unlocked. adaptive is 60 "
                      "with frame skipping if the PC can't sustain it, to avoid slow motion; "
                      "unlocked has no cap and steps the game by measured frame time.")
    .allowed({"stock", "30", "60", "adaptive", "unlocked"});

// Per-second frame pacing summary, off by default. Answers, in one line: what
// rate are we declaring to the sim, how many presents/sec are we actually
// getting, how long does a frame's work really take, and what is the adaptive
// ladder doing about it. See the present hook in eternalsonata_framerate.cpp.
REXCVAR_DEFINE_BOOL(frame_debug, false, "Eternal Sonata",
                    "Log a per-second frame pacing summary (declared rate, achieved presents/sec, "
                    "frame work time, adaptive ladder state)");

// Debug aid for reverse-engineering the Options screen: with this on, every
// left/right press on the Subtitles row snapshots all committed guest memory
// and intersects it against the previous snapshots of the *other* value, which
// narrows down where a row's value (and the highlight bar that follows it)
// actually lives. Costs a full memory diff per press, so it is off by default.
// See the "Value-highlight hunt" section in eternalsonata_options.cpp.
REXCVAR_DEFINE_BOOL(menu_scan, false, "Eternal Sonata",
                    "Debug: diff guest memory across Subtitles row toggles to locate menu "
                    "value state (logs candidates)");

// The native renderer's per-phase timing (native_renderer_profile.h) reads the
// clock twice per zone, many times per draw call, measured as a real,
// several-percent chunk of frame time on its own. Off by default so playing
// isn't paying for it; turn on to get the frame summary and CPU/GPU verdict.
REXCVAR_DEFINE_BOOL(native_profile_zones, false, "Eternal Sonata",
                    "Time the native renderer's per-phase zones (present, draw, texture bind, "
                    "etc.). Off skips every clock read those zones make, at the cost of the "
                    "frame time summary and CPU/GPU verdict going blank.");

// Each mip chain adds a second source range to the texture cache, and the
// aperture walk, content hash and write watch are all paid per range.
REXCVAR_DEFINE_BOOL(native_texture_mips, false, "Eternal Sonata",
                    "Upload full mip chains for guest textures. Off uploads only level 0, "
                    "which halves the texture cache's source ranges and with them the "
                    "aperture walks, content hashing and write watches. Toggling at runtime "
                    "leaves the textures cached under the old setting behind until they "
                    "age out of the mirror.");

// The mirror's own working set is a few hundred textures; the rest of what it
// accumulates is areas the game has left. Counted in uploaded bytes rather than
// in entries because a 32x32 icon and a 2048x2048 background are both one entry
// and four thousand times apart in cost.
REXCVAR_DEFINE_INT32(native_texture_budget_mb, 384, "Eternal Sonata",
                   "How much texture memory the native renderer's mirror may hold, in MiB. "
                   "Over budget it drops the textures bound least recently. 0 removes the "
                   "limit, which lets the mirror grow for as long as the session lasts.")
    .range(0, 4096);

// Applied through ImGui's style (see UiScaleApplier), so it reaches every overlay
// window without each one knowing about it.
REXCVAR_DEFINE_DOUBLE(ui_scale, 1.0, "Eternal Sonata",
                      "Scale of the overlay windows and their text")
    .range(0.5, 3.0);

// A multiplier rather than an angle, because the guest picks its own vertical
// field of view per camera and cutscenes set theirs deliberately. Applied in
// the sub_82108180 hook, which is the single place the projection matrix, the
// stored field of view and the cull extents are all derived from it.
REXCVAR_DEFINE_DOUBLE(camera_fov_scale, 1.0, "Eternal Sonata",
                      "Multiplier on the game's vertical field of view. 1 leaves every camera "
                      "as the game framed it; above 1 pulls the view back.")
    .range(0.5, 2.0);

// Which model the overworld leader wears. "party" tracks the active party's
// first member (the game itself always spawns Allegretto regardless of party
// order); a character name pins that character; "default" leaves the game's
// own choice alone. Read by the spawn hook in field_player_model_override.cpp.
//
// The tokens double as the debug overlay's combo values, so the two stay in
// step -- see FieldPlayerModelOverride::SelectionNames().
REXCVAR_DEFINE_STRING(field_leader_model, "default", "Eternal Sonata",
                      "Model used by the overworld leader: default (the game's own), party (the "
                      "active party's first member), or a specific character")
    .allowed({"default", "party", "allegretto", "polka", "beat", "frederic", "viola", "salsa",
              "jazz", "falsetto", "claves", "march"});

REXCVAR_DEFINE_BOOL(field_action_default_model, true, "Eternal Sonata",
                    "Use the story character model for field interaction animations");

// Applied by the sub_821C55A8 hook in aim_input.cpp, which is the only place
// either reaches: ordinary movement and camera control keep the real stick.
// The defaults are what retail does, which differs per axis.
REXCVAR_DEFINE_BOOL(aim_invert_x, false, "Eternal Sonata",
                    "Invert horizontal aiming with long range attacks");

REXCVAR_DEFINE_BOOL(aim_invert_y, true, "Eternal Sonata",
                    "Invert vertical aiming with long range attacks");

// Read every frame by TurboHeld in eternalsonata_framerate.cpp, the pad
// counterpart of holding Tab.
REXCVAR_DEFINE_STRING(fast_forward_button, "rs", "Eternal Sonata",
                      "Controller button that fast forwards while held: off, a, b, x, y, lb, "
                      "rb, lt, rt, ls, rs, back, start, dpad_up, dpad_down, dpad_left, "
                      "dpad_right")
    .allowed({"off", "a", "b", "x", "y", "lb", "rb", "lt", "rt", "ls", "rs", "back", "start",
              "dpad_up", "dpad_down", "dpad_left", "dpad_right"});

// Which voice bank suffix the game loads. The two the game ships with are
// selected by its own byte at 0x8243FC06, which this cvar mirrors rather than
// replaces: while it names "jpn" or "usa" the path hook stands down entirely
// and the guest's byte is the only authority. A mod-added id is what turns the
// hook on and points it at that mod's `_<suffix>` banks.
//
// Hot reload: the guest's bank caches are keyed on its byte, and while a mod
// language is active the voice hooks swap in a key of its own (see
// ActiveVoiceKey), so every switch invalidates them like a stock one does.
//
// Not `.allowed(...)`: the valid set is not known until the mods have
// registered, and an id left behind by a mod that was since disabled has to
// read back as entry 0 rather than be rejected at parse time.
REXCVAR_DEFINE_STRING(voice_language, "usa", "Eternal Sonata",
                      "Spoken language: jpn, usa, or the id a mod's [[voice_language]] declared")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace eternalsonata {

namespace {

// Everything build.py used to write into the shipped eternalsonata.toml.
// These are the game's defaults, not necessarily the SDK's; kept here so
// there is one place that owns "what Eternal Sonata ships with".
struct DefaultValue {
  const char* cvar;
  const char* value;
};

constexpr std::array kGameDefaults = {
    DefaultValue{"gpu_plugin", "plume"},
    DefaultValue{"game_data_root", "assets"},
    DefaultValue{"gpu_allow_invalid_fetch_constants", "true"},
    // "fast" always copies the resolve readback to CPU memory every submission
    // (reading a one-frame-delayed buffer to avoid a GPU stall); "some" skips
    // that copy whenever the delayed buffer is still valid and only copies on
    // a cache miss, so it is the lighter of the two. This ships with "fast"
    // anyway: the unconditional copy is what keeps the readback in step with
    // what was drawn. The legacy per-backend
    // d3d12_readback_resolve/vulkan_readback_resolve bools are aliases this
    // shared cvar overrides whenever it has a non-default value, so setting
    // this is enough; they are not set here.
    DefaultValue{"readback_resolve", "fast"},
    DefaultValue{"clear_memory_page_state", "true"},
    // The game binds an 8-tile-wide render target at EDRAM tile 1720 whose draws
    // give no usable height estimate, so it claims all 2048 tiles and takes
    // ownership of the scene color target at tile 0 by wrapping. That range then
    // resolves out of the wrong surface as zeros, which is the black cross-fade
    // source on camera transitions and the black half of the save screenshot.
    DefaultValue{"no_edram_wrap_claim", "true"},
    // Static mirror of the mod catalog, attached to every release of the mods
    // repo; used when the Firestore backend is rate limited or down.
    DefaultValue{"mod_catalog_fallback_url",
                 "https://github.com/birabittoh/EternalSonataReprise-Mods/releases/latest/"
                 "download/catalog.json"},
    // Tearing off by default, on both renderers. The old default was false,
    // because the SDK's vblank pump ties the presentation-interval wait to
    // vsync and a real presentation interval then made the frame-clocked sim
    // run at half speed (game speed = actual fps / declared fps). The frame
    // limiter no longer asks for a real interval at all -- it declares the rate
    // and paces in the host, which is independent of vsync; see the NOTE in
    // eternalsonata_framerate.cpp. On the native renderer this is the swap
    // chain's own vsync (native_renderer_plume.cpp), where the cost of leaving
    // it on is quantisation: two buffers plus the present's fence wait means a
    // frame over 16.7 ms lands on 30 rather than somewhere between.
    DefaultValue{"vsync", "true"},
    DefaultValue{"swap_post_effect", "fxaa"},
    DefaultValue{"mnk_mode", "true"},
    DefaultValue{"gyro_aim", "true"},
    DefaultValue{"keybind_a", "LMB"},
    DefaultValue{"keybind_b", "RMB"},
    DefaultValue{"keybind_x", "MMB"},
    DefaultValue{"keybind_y", "Space"},
    DefaultValue{"keybind_left_trigger", "1"},
    DefaultValue{"keybind_right_trigger", "3"},
    DefaultValue{"keybind_left_shoulder", "WheelUp"},
    DefaultValue{"keybind_right_shoulder", "WheelDown"},
    DefaultValue{"keybind_lstick_press", "T"},
    DefaultValue{"keybind_back", "Backspace"},
    DefaultValue{"keybind_start", "Escape"},
    DefaultValue{"resolution", "720p"},
    DefaultValue{"resolution_scale", "1"},
    DefaultValue{"fullscreen", "false"},
    DefaultValue{"audio_mute", "false"},
    DefaultValue{"audio_volume", "1"},
    DefaultValue{"shader_dump_enabled", "false"},
    DefaultValue{"texture_dump_enabled", "false"},
    DefaultValue{"texture_dump_format", "png"},
    DefaultValue{"texture_dump_skip_sizes", "1280x720,640x360,720x720"},
};

// cvars persisted to the friendly settings.toml by the Basic section.
// vulkan_device gets a custom row (a dynamic dropdown) rather than the generic
// DrawCvarWidget path, but is still listed here so the generic Reset-All /
// restart-tracking loops cover it; GetFlagInfo/ResetToDefault etc. no-op
// harmlessly for it on a build without Vulkan. gpu_backend no longer has a row
// at all (it is set from the config file or the All Settings browser), but stays
// listed so an existing saved value survives a Reset-All round trip.
// host_timer_resolution_ms is likewise listed unconditionally even though the
// cvar only exists on Windows, for the same reason vulkan_device is: the
// generic loops no-op on a name that is not registered, and keeping the list
// platform-independent means a settings.toml written on Windows round-trips
// unharmed through a Linux build.
// vsync is listed here even though neither renderer registers it in the
// executable: the Xenos plugin defines it in its command processor and the
// native renderer registers it itself (see RegisterNativeRendererCvars), so
// exactly one of the two owns the name by the time this UI draws. The generic
// loops no-op on a name that is not registered, same as vulkan_device.
// voice_language is listed even though user_language needed special handling to
// be (see SaveBasicCvars): this subset is saved from *live* cvar values, and the
// live voice_language is always exactly what the player chose. There is no
// donor rewrite for voice (a mod voice language gets a bank path of its own
// rather than borrowing a built-in's), so nothing ever shadows it.
// render_scale is listed for the same no-op reason as vsync: only the native
// renderer registers it, and resolution_scale stays beside it so a settings.toml
// still round-trips through Xenos.
constexpr std::array kBasicCvarNames = {
    "fullscreen",  "resolution",   "resolution_scale", "user_language",
    "input_backend", "gpu_backend", "vulkan_device", "frame_rate",
    "audio_mute", "audio_volume", "field_leader_model", "field_action_default_model",
    "host_timer_resolution_ms", "vsync", "voice_language", "render_scale",
    "camera_fov_scale", "render_pixelated_scaling", "aim_invert_x", "aim_invert_y",
    "gyro_aim", "gyro_sensitivity", "gyro_invert_x", "gyro_invert_y", "fast_forward_button",
    "enemy_exp_multiplier", "enemy_gold_multiplier", "enemy_hp_multiplier", "ui_scale"};

// Steps for the Game tab's multiplier rows. HP stops short of zero, the
// same floor enemy_hp_multiplier has.
constexpr std::array kRewardMultiplierSteps = {0.0, 0.25, 0.5, 0.75, 1.0, 1.5,
                                               2.0, 3.0,  4.0, 5.0,  10.0};
constexpr std::array kUiScaleSteps = {0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0};
constexpr std::array kHpMultiplierSteps = {0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 4.0, 5.0, 10.0};

// audio_volume is stored (and applied to samples by the SDL audio driver) as
// linear amplitude, but human loudness perception is roughly logarithmic --
// a linear slider (amplitude == percent/100) would spend most of its travel
// on barely-perceptible changes near the top end and cram all the audible
// range into the last few percent at the bottom. Map the displayed 0-100%
// through a dB curve instead: -40dB at 0% (quiet enough to treat as silence
// below) up to 0dB (full amplitude) at 100%, evenly spaced in dB rather than
// in amplitude.
constexpr double kMinVolumeDb = -40.0;

double VolumeAmplitudeFromPercent(int percent) {
  if (percent <= 0)
    return 0.0;
  if (percent >= 100)
    return 1.0;
  double db = kMinVolumeDb * (100 - percent) / 100.0;
  return std::pow(10.0, db / 20.0);
}

int VolumePercentFromAmplitude(double amplitude) {
  if (amplitude <= 0.0)
    return 0;
  double db = 20.0 * std::log10(amplitude);
  if (db <= kMinVolumeDb)
    return 0;
  return std::clamp(static_cast<int>(std::lround(100.0 - db * 100.0 / kMinVolumeDb)), 0, 100);
}

// The languages the game shipped with. XLanguage IDs per the Xbox 360 kernel's
// user_language cvar; `code` is the two-letter form the native Options screen's
// Text row draws, since that row shares one line between all of its values and
// full names do not fit (about 126px a column against roughly 25px a
// character). `btx_slot` is the BTX block each one's text lives in; see
// assets::kBtxLanguages, which is the authority for the seven fourccs.
constexpr std::array kBuiltinLanguages = {
    LanguageOption{"1", "English", "EN", "USA "},
    LanguageOption{"3", "German", "DE", "DEU "},
    LanguageOption{"4", "French", "FR", "FRA "},
    LanguageOption{"5", "Spanish", "ES", "ESP "},
    LanguageOption{"6", "Italian", "IT", "ITA "},
};

// Entries mods added, through either the "settings.language_option" event or
// assets.toml's [[language]] block. Owns its strings: the event's payload.bytes
// only lives for the duration of Publish(), and GetLanguageOptions hands out
// c_str() pointers that have to outlive the call. Only ever appended to and
// never cleared, so those pointers stay valid for the process.
struct ModLanguage {
  std::string id;
  std::string label;
  std::string code;
  std::string btx_slot;  // fourcc with its trailing space, or empty
};
std::vector<ModLanguage> g_mod_languages;

// Strings mods published through "settings.native_string", keyed
// "<XLanguage id>:<key>", value UTF-8. Owns its storage, same reason.
std::map<std::string, std::string> g_native_strings;

// What the config actually said at boot, and whether ApplyBootLanguageDonorSlot
// has since pointed the live cvar somewhere else for the guest's benefit. Once
// the player picks something this session the live cvar is authoritative again
// (the guest has long since booted), which is what g_language_selection_changed
// distinguishes. Comparing ids cannot: the player is free to select the donor
// language itself.
std::string g_boot_language_id;
bool g_boot_language_latched = false;
bool g_language_donor_applied = false;
bool g_language_selection_changed = false;

// Normalises a BTX fourcc to the four-byte, trailing-space form the container
// code uses. Empty in, empty out.
std::string NormalizeBtxSlot(std::string_view slot) {
  std::string out(slot);
  while (!out.empty() && out.back() == ' ')
    out.pop_back();
  if (out.empty())
    return out;
  for (char& c : out)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  out.resize(4, ' ');
  return out;
}

// The two voice languages the game shipped with. `guest_byte` is what
// BYTE2(dword_8243FC04) holds for each: the selector is a single byte and the
// path builders (sub_821BD0D0, sub_821BD480) do nothing with it but choose
// between appending "_usa" and appending nothing.
//
// English first, which is NOT the byte's own order, and that is the point.
// The Options screen draws this row English then Japanese, and the game places
// its highlight at `base + 200 * (BYTE2(FC04) ^ 1)`: the byte counts the
// opposite way round from the column. Ordering the list the way the row is
// drawn makes a list index and a column index the same number everywhere, and
// leaves exactly one place, guest_byte, where the two disagree. Do not
// "tidy" this back into byte order; that mismatch is what made the row skip
// Japanese entirely the first time round.
//
// Safe to reorder because nothing indexes this list positionally across runs:
// the voice_language cvar stores an id, not a position.
constexpr std::array kBuiltinVoiceLanguages = {
    VoiceLanguageOption{"usa", "English", "EN", "_usa", 1},
    VoiceLanguageOption{"jpn", "Japanese", "JP", "", 0},
};

// Voice languages mods added, through either "settings.voice_language_option"
// or assets.toml's [[voice_language]] block. Owns its strings for the same
// reason ModLanguage does.
struct ModVoiceLanguage {
  std::string id;
  std::string label;
  std::string code;
  std::string suffix;  // with its leading underscore
};
std::vector<ModVoiceLanguage> g_mod_voice_languages;

// VoiceLanguageIndex, cached for the guest thread: the path hook reads it on
// every file probe in the game.
std::atomic<int> g_active_voice{0};

// Normalises a bank filename suffix to the one form the path hook and the TOC
// writer both use: lowercase, exactly one leading underscore. Empty in, empty
// out. That is Japanese, whose banks carry the bare name.
std::string NormalizeVoiceSuffix(std::string_view suffix) {
  std::string out(suffix);
  size_t start = 0;
  while (start < out.size() && out[start] == '_')
    ++start;
  out = out.substr(start);
  if (out.empty())
    return out;
  for (char& c : out)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  return "_" + out;
}

struct FrameRateOption {
  const char* id;  // value stored by the frame_rate cvar
  const char* label;
};

// The authority for both menus: the overlay's Frame Rate slider and the native
// Options screen's Frame Rate row (eternalsonata_options.cpp) draw this same
// list through FrameRateOptionLabel, so the two cannot drift.
//
// Ordered by how far each state lets the rate climb: 30, 60 pinned, 60 with the
// ladder, then uncapped, which also keeps the two 60-based states adjacent.
constexpr std::array kFrameRateOptions = {
    // "30" follows the game's own requests rather than pinning every screen to
    // 30: the title and the save menu ask for 60, and their logic is written for
    // it. The host limiter paces whatever is asked for, so speed stays correct.
    FrameRateOption{"30", "30 FPS"},
    FrameRateOption{"60", "60 FPS"},
    FrameRateOption{"adaptive", "Adaptive"},
    FrameRateOption{"unlocked", "Unlocked"},
};

#if defined(_WIN32)
struct TimerResolutionOption {
  const char* id;  // value stored by the host_timer_resolution_ms cvar
  const char* label;
};

// How fine a host timer tick to ask Windows for. The game's audio thread arms a
// 5ms periodic timer and Windows rounds any period up to the current system
// timer resolution, so this is what decides whether the guest's 200Hz sequencer
// can actually hit its period. See src/host_timer_resolution.h.
//
// Ordered coarsest to finest, which is also least to most power drawn.
constexpr std::array kTimerResolutionOptions = {
    // The host's own tick, about 15.6ms. That is 3.1x coarser than the period
    // the game asks for, so music, voices and anything else the sequencer
    // drives run about 3x slow. Kept as an option because it is the honest
    // "change nothing" state and the quickest way to confirm this row is what
    // an audio timing problem is sensitive to, not because anyone should play
    // on it.
    TimerResolutionOption{"0", "Host"},
    // 5ms, exactly what the guest asks NtSetTimerEx for, and what the console
    // itself ran at. Correct on any machine; it costs the least power of the
    // two working settings and is the default.
    TimerResolutionOption{"5", "Xbox 360"},
    // 1ms. Finer than the guest asks for, so the sequencer retires a finished
    // line sooner and the next one starts with less of a pause between them.
    // Purely a matter of taste - the pause at 5ms is the console's own pacing -
    // and it draws more power, since timeBeginPeriod raises the tick rate
    // process wide.
    TimerResolutionOption{"1", "Instantaneous"},
};
#endif  // _WIN32

// True once `name`'s cvar has actually been changed at runtime this session
// and needs a relaunch to take effect. GetPendingRestartFlags() only tracks
// cvars changed at runtime (settings UI, console, mods), not values applied
// while loading a config file at boot, so a saved preference that merely
// differs from the SDK's factory default doesn't trip it on a fresh launch.
// See SetFlagByNameImpl's mark_restart parameter in the SDK's cvar.cpp.
bool CvarPendingRestart(const char* name) {
  auto pending = rex::cvar::GetPendingRestartFlags();
  return std::find(pending.begin(), pending.end(), name) != pending.end();
}

// True if any cvar this settings UI owns still needs a restart. Filtered to
// the rows we actually draw rather than "any pending flag at all" so a
// developer poking an unrelated cvar from the console doesn't put the game's
// own menus into a restart-pending state.
bool AnyKnownPendingRestart() {
  auto pending = rex::cvar::GetPendingRestartFlags();
  auto is_tracked = [&pending](const char* name) {
    return std::find(pending.begin(), pending.end(), name) != pending.end();
  };
  for (const char* name : kBasicCvarNames) {
    if (is_tracked(name))
      return true;
  }
  return false;
}

// resolution_scale value that renders at "100%" (native) for a given display
// resolution. The SDK's resolution_scale is an integer EDRAM/draw
// supersampling factor (range 1-8), not a fractional multiplier, so this
// table is the source of truth for what "100%" means per resolution;
// DrawRenderScaleRow derives 50%-100% steps from it at runtime.
int ResolutionScaleFor(const std::string& resolution) {
  if (resolution == "1080p")
    return 2;
  if (resolution == "1440p")
    return 3;
  if (resolution == "4K")
    return 4;
  return 1;  // 720p, and fallback for anything unrecognized.
}

// Vertical pixel count of each named resolution preset.
int ResolutionHeightFor(const char* option) {
  std::string opt = option;
  if (opt == "1080p")
    return 1080;
  if (opt == "1440p")
    return 1440;
  if (opt == "4K")
    return 2160;
  return 720;  // 720p
}

// Remembered from CreateSettingsDialog so settings changed outside the overlay
// - e.g. the native Fullscreen row in the game's own Options screen - can be
// persisted to the same file the overlay writes. See SaveUserSettings. Also
// used by DesktopDisplayHeight below, since it's the only handle to the
// engine's Window this file has outside the settings dialog itself.
std::filesystem::path g_user_settings_path;
rex::ui::Window* g_window = nullptr;

// Height in pixels of the display the window is (or would be) shown on.
// Falls back to 4K (no filtering) if it can't be determined.
//
// Routed through rex::ui::Window::GetDesktopDisplayHeight() (backed by
// SDL_GetDisplayForWindow/SDL_GetDesktopDisplayMode) rather than a direct
// platform or SDL call from here: rex::runtime ships as rexruntimerd.dll
// with SDL3-static as an *interface* link dependency, so the DLL and this
// exe each get their own statically-linked copy of SDL3 with independent
// subsystem state. The DLL's copy is the one that calls SDL_Init/creates
// the window, so a direct SDL_GetPrimaryDisplay() call made from this exe's
// own copy would see no video subsystem and always fail; going through the
// Window object's virtual method instead runs inside the DLL, against the
// copy of SDL that actually owns the window. That also makes this
// cross-platform for free (Windows/X11/Wayland) instead of the previous
// GetSystemMetrics(SM_CYSCREEN), which only ever worked on Windows.
int DesktopDisplayHeight() {
  uint32_t height = g_window ? g_window->GetDesktopDisplayHeight() : 0;
  return height > 0 ? static_cast<int>(height) : 2160;
}

std::vector<std::string> BasicCvarNames() {
  return std::vector<std::string>(kBasicCvarNames.begin(), kBasicCvarNames.end());
}

// Writes the basic subset, with user_language put back to what the player
// actually chose for the duration of the write.
//
// user_language is in that subset, and while ApplyBootLanguageDonorSlot's
// override is in force the live cvar holds the donor's id, not the player's.
// Saving it as-is would quietly rewrite the config from "Portugues" to "Spanish",
// so the next launch would come up in the donor language and the mod's language
// would look like it had unselected itself. Any of the other basic settings
// changing (frame rate, resolution, fullscreen) is enough to trigger that,
// which is what makes it worth handling here rather than at each call site.
void SaveBasicCvars(const std::filesystem::path& path) {
  auto* entry = rex::cvar::GetFlagInfo("user_language");
  const bool shadowed =
      g_language_donor_applied && !g_language_selection_changed && entry && entry->setter;
  std::string donor_id;
  if (shadowed) {
    donor_id = entry->getter();
    entry->setter(g_boot_language_id);
  }
  rex::cvar::SaveConfigSubset(path, BasicCvarNames());
  if (shadowed) {
    // Straight back to the donor: the guest is running on it.
    entry->setter(donor_id);
  }
}

// Populated once by InitSettingsCaches() at startup; CuratedSettingsDialog
// reads from these instead of re-enumerating Vulkan devices every time the
// F4 overlay is opened.
#if REX_HAS_VULKAN
std::vector<rex::ui::vulkan::DeviceInfo> g_vulkan_devices_cache;
#endif

// The restart banner and the per-row restart asterisk.
constexpr ImVec4 kRestartColor(1.0f, 0.85f, 0.2f, 1.0f);

// Fixed pixel sizes in the settings overlay, which ScaleAllSizes cannot reach.
float Px(float pixels) { return pixels * UiScale(); }

// Draws nothing; it is a dialog only to get a per frame call inside the ImGui
// frame. A change lands in full on the next frame, since the font size is
// fixed at NewFrame. Sizes are rescaled from the unscaled style captured on
// the first call, because ScaleAllSizes compounds; colors are left alone, so
// a theme change made at runtime survives.
class UiScaleApplier : public rex::ui::ImGuiDialog {
 public:
  explicit UiScaleApplier(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& /*io*/) override {
    ImGuiStyle& style = ImGui::GetStyle();
    if (!base_) {
      base_ = style;
    }
    const float scale = UiScale();
    if (scale == applied_) {
      return;
    }
    ImGuiStyle scaled = *base_;
    scaled.ScaleAllSizes(scale);
    scaled.FontScaleMain = base_->FontScaleMain * scale;
    std::copy(std::begin(style.Colors), std::end(style.Colors), std::begin(scaled.Colors));
    style = scaled;
    applied_ = scale;
  }

 private:
  std::optional<ImGuiStyle> base_;
  float applied_ = 1.0f;
};

class CuratedSettingsDialog : public rex::ui::ImGuiDialog {
 public:
  CuratedSettingsDialog(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window,
                        std::filesystem::path user_settings_path,
                        std::filesystem::path app_config_path,
                        rex::input::InputSystem* input_system)
      : rex::ui::ImGuiDialog(drawer),
        window_(window),
        user_settings_path_(std::move(user_settings_path)),
        app_config_path_(std::move(app_config_path)),
        input_system_(input_system) {
#if REX_HAS_VULKAN
    vulkan_devices_ = g_vulkan_devices_cache;
#endif
  }

 protected:
  void OnDraw(ImGuiIO& /*io*/) override {
    ImGui::SetNextWindowBgAlpha(0.9f);
    if (!ImGui::Begin("Settings##rex", nullptr,
                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::End();
      return;
    }

    if (AnyPendingRestart()) {
      ImGui::PushStyleColor(ImGuiCol_Text, kRestartColor);
      ImGui::TextWrapped("Some changes require a restart to take effect.");
      ImGui::PopStyleColor();
      ImGui::SameLine();
      if (ImGui::SmallButton("Restart Now")) {
        // Not Relaunch()+RequestClose() inline: this dialog is not always drawn
        // on the UI thread. Under the native renderer the overlay runs on the
        // guest's own render thread (see OnCreateImmediateDrawer), and closing
        // the window from there tears the title down from inside itself and
        // hangs, leaving the old window on screen next to the relaunched one.
        // RestartNow() marshals the close back to the UI thread.
        RestartNow();
      }
    }

    DrawUpdateSection();

    if (ImGui::BeginTabBar("##settings_tabs")) {
      if (ImGui::BeginTabItem("Graphics")) {
        DrawCvarRow("Fullscreen", "fullscreen");
        DrawUiScaleRow();
        DrawRenderScaleRow();
        DrawFieldOfViewRow();
        DrawFrameRateRow();
        DrawRenderFilterRow();
        // Takes effect immediately on both renderers: the Xenos plugin reads
        // the cvar per vblank, the native renderer on the next present.
        DrawCvarRow("VSync", "vsync");
#if REX_HAS_VULKAN
        if (rex::cvar::GetFlagByName("gpu_backend") == "vulkan") {
          DrawVulkanDeviceRow();
        }
#endif
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Audio")) {
        DrawCvarRow("Mute Audio", "audio_mute");
        DrawAudioVolumeRow();
        DrawGameVolumeRow("Music Volume", ETERNALSONATA_SETTING_VOLUME_MUSIC, 0);
        DrawGameVolumeRow("Sound Effects Volume", ETERNALSONATA_SETTING_VOLUME_SFX, 1);
        DrawGameVolumeRow("Voice Volume", ETERNALSONATA_SETTING_VOLUME_VOICE, 2);
#if defined(_WIN32)
        DrawTimerResolutionRow();
#endif
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Input")) {
        DrawCvarRow("Input Backend", "input_backend");
        DrawCvarRow("Invert Aim X", "aim_invert_x");
        DrawCvarRow("Invert Aim Y", "aim_invert_y");
        DrawCvarRow("Fast Forward", "fast_forward_button",
                    "Controller button that fast forwards the game while held, like Tab "
                    "on the keyboard.");
        ImGui::Separator();
        DrawCvarRow("Gyro Aiming", "gyro_aim");
        if (rex::cvar::GetFlagByName("gyro_aim") == "true") {
          DrawSensitivityRow("Gyro Sensitivity", "gyro_sensitivity");
          DrawCvarRow("Invert Gyro X", "gyro_invert_x");
          DrawCvarRow("Invert Gyro Y", "gyro_invert_y");
        }
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Game")) {
        DrawLanguageRow();
        DrawVoiceLanguageRow();
        ImGui::Separator();
        DrawMultiplierRow("EXP", "enemy_exp_multiplier", kRewardMultiplierSteps,
                          "Multiplies the EXP every enemy is worth. Stacks with mods that "
                          "rebalance enemies, and with EXP bonus equipment.");
        DrawMultiplierRow("Gold", "enemy_gold_multiplier", kRewardMultiplierSteps,
                          "Multiplies the gold every enemy drops. Stacks with mods that "
                          "rebalance enemies.");
        DrawMultiplierRow("Enemy HP", "enemy_hp_multiplier", kHpMultiplierSteps,
                          "Multiplies every enemy's max HP, for longer or shorter fights. "
                          "Stacks with mods that rebalance enemies.");
        ImGui::Separator();
        DrawFieldLeaderModelRow();
        DrawFieldActionModelRow();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }

    ImGui::Separator();
    if (ImGui::Button("Reset All to Defaults")) {
      for (const char* name : kBasicCvarNames) {
        rex::cvar::ResetToDefault(name);
      }
      SaveBasic();
    }
    ImGui::SameLine();
    // Opens the SDK's own full cvar browser (the same one bind_settings/F4
    // would show if settings_manager_enabled were false) for anything not
    // surfaced above. It's a separate top-level ImGuiDialog -- constructing
    // it registers it with the drawer (see ImGuiDialog's ctor), so it starts
    // drawing/receiving input immediately, independent of this dialog; this
    // button just toggles that lifetime, mirroring bind_settings's own
    // open/close toggle in rex_app.cpp. Given a distinct window_title
    // ("All Settings##rexdev") so its ImGui window doesn't share an ID
    // with this dialog's own "Settings##rex" -- same ID would merge both
    // dialogs' draws into a single squeezed window instead of two.
    if (ImGui::Button(dev_settings_overlay_ ? "Close All Settings" : "All Settings...")) {
      if (dev_settings_overlay_) {
        dev_settings_overlay_.reset();
      } else {
        // config_path here is where the SDK's dialog writes ("Save to
        // config"), not where it reads from; cvars are already loaded from
        // app_config_path_ at boot (see ReXApp::SetupEnvironment). Pointing
        // saves at user_settings_path_ instead keeps <game>.toml read-only:
        // it can still be hand-edited for dev-only setup, but nothing the
        // running game does ever writes to it.
        dev_settings_overlay_ = std::make_unique<rex::ui::SettingsDialog>(
            imgui_drawer(), user_settings_path_, input_system_, "All Settings##rexdev");
      }
    }

    ImGui::End();
  }

 private:
  // Same state the game's own Options screen reads through
  // AnyCvarPendingRestart (see settings.h), so the banner here and the
  // "(Restart)" markers there can never disagree.
  bool AnyPendingRestart() { return AnyKnownPendingRestart(); }

  void SaveBasic() { SaveBasicCvars(user_settings_path_); }
  // Game self-update (see rex::system::AutoUpdater), surfaced here rather
  // than the SDK's mod manager overlay (F1) since a player who never touches
  // mods should still be told about an available update
  void DrawUpdateSection() {
    if (!rex::system::AutoUpdater::SupportsSelfUpdate()) {
      return;
    }

    if (!update_check_requested_) {
      update_check_requested_ = true;
      auto_updater_.CheckAsync();
    }

    // A previous session already downloaded and staged an update (whether or
    // not this one ever calls CheckAsync/InstallAsync again); offer the
    // restart regardless of auto_updater_'s own in-memory state.
    if (rex::system::AutoUpdater::HasPendingSelfUpdate(
            rex::system::AutoUpdater::InstallRoot())) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.85f, 0.55f, 1.0f));
      ImGui::TextWrapped("An update has been downloaded.");
      ImGui::PopStyleColor();
      ImGui::SameLine();
#if REX_PLATFORM_ANDROID
      // The system installer takes it from here and replaces the app itself;
      // closing the game would only dismiss the installer prompt.
      if (ImGui::SmallButton("Install Update##autoupdate")) {
        rex::system::AutoUpdater::ApplyAndRestart(rex::system::AutoUpdater::InstallRoot(), {});
      }
#else
      if (ImGui::SmallButton("Restart & Apply##autoupdate")) {
        // This install root contains the running executable itself,
        // which stays locked for this process's whole lifetime (see
        // AutoUpdater::ApplyAndRestart's contract). The spawned helper
        // outlives this process, applies the swap, and launches the new exe.
        if (rex::system::AutoUpdater::ApplyAndRestart(rex::system::AutoUpdater::InstallRoot(),
                                                      rex::filesystem::GetExecutablePath()) &&
            window_) {
          // Marshalled for the same reason as "Restart Now" above: the overlay
          // may be drawing on the guest render thread, and RequestClose has to
          // run on the thread that owns the window.
          rex::ui::Window* window = window_;
          window->app_context().CallInUIThread([window] { window->RequestClose(); });
        }
      }
#endif
      return;
    }

    auto install = auto_updater_.InstallSnapshot();
    if (install.in_progress) {
      if (install.total_bytes > 0) {
        ImGui::TextDisabled("Downloading update... %.0f%%",
                            100.0 * static_cast<double>(install.downloaded_bytes) /
                                static_cast<double>(install.total_bytes));
      } else {
        ImGui::TextDisabled("Downloading update...");
      }
      return;
    }
    if (install.done && !install.ok) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
      ImGui::TextWrapped("%s", install.message.c_str());
      ImGui::PopStyleColor();
      return;
    }

    if (auto_updater_.state() != rex::system::UpdateCheckState::kUpdateAvailable) {
      return;  // kIdle/kChecking/kUpToDate/kFailed: nothing worth showing.
    }
    auto info = auto_updater_.Available();
    if (!info) {
      return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.85f, 0.55f, 1.0f));
    ImGui::TextWrapped("Update available: v%s", info->version.c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::SmallButton("Download Update")) {
      auto_updater_.InstallAsync(*info, rex::system::AutoUpdater::InstallRoot());
    }
  }

  // A row's label, with a yellow asterisk when `cvar` only takes effect after a
  // restart, and in yellow itself once it has been changed and is waiting for
  // one. Returns whether the label was hovered, for the row's own tooltip.
  bool DrawRowLabel(const char* label, const char* cvar) {
    const bool pending = cvar && CvarPendingRestart(cvar);
    if (pending) {
      ImGui::TextColored(kRestartColor, "%s", label);
    } else {
      ImGui::TextUnformatted(label);
    }
    bool hovered = ImGui::IsItemHovered();
    const auto* entry = cvar ? rex::cvar::GetFlagInfo(cvar) : nullptr;
    if (entry && entry->lifecycle == rex::cvar::Lifecycle::kRequiresRestart) {
      ImGui::SameLine(0.0f, Px(2.0f));
      ImGui::TextColored(kRestartColor, "*");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Requires a restart.");
      }
    }
    return hovered;
  }

  // A plain cvar row: label, the SDK's generic widget, and the cvar's own
  // description as a tooltip unless the row has a better one.
  void DrawCvarRow(const char* label, const char* name, const char* tooltip = nullptr) {
    const auto* entry = rex::cvar::GetFlagInfo(name);
    if (!entry)
      return;
    if (DrawRowLabel(label, name)) {
      if (tooltip) {
        ImGui::SetTooltip("%s", tooltip);
      } else if (!entry->description.empty()) {
        ImGui::SetTooltip("%s", entry->description.c_str());
      }
    }
    ImGui::SameLine(Px(180.0f));
    ImGui::PushID(name);
    if (rex::ui::DrawCvarWidget(*entry, Px(160.0f), /*persist=*/true)) {
      SaveBasic();
    }
    ImGui::PopID();
  }

  // Discrete steps, so the shipped balance is always exactly one notch. The
  // label shows the cvar's real value, which a hand edited config may have
  // put between two steps.
  void DrawMultiplierRow(const char* label, const char* name, std::span<const double> steps,
                         const char* tooltip) {
    const auto* entry = rex::cvar::GetFlagInfo(name);
    if (!entry)
      return;
    const double value = std::atof(entry->getter().c_str());
    int idx = 0;
    for (int i = 1; i < static_cast<int>(steps.size()); ++i) {
      if (std::abs(steps[i] - value) < std::abs(steps[idx] - value))
        idx = i;
    }
    char text[16];
    std::snprintf(text, sizeof(text), "%gx", value);

    if (DrawRowLabel(label, name)) {
      ImGui::SetTooltip("%s", tooltip);
    }
    ImGui::SameLine(Px(180.0f));
    ImGui::SetNextItemWidth(Px(160.0f));
    ImGui::PushID(name);
    if (ImGui::SliderInt("##v", &idx, 0, static_cast<int>(steps.size()) - 1, text,
                         ImGuiSliderFlags_NoInput)) {
      char step[16];
      std::snprintf(step, sizeof(step), "%g", steps[idx]);
      rex::cvar::SetFlagByName(name, step, /*persist=*/true);
      SaveBasic();
    }
    ImGui::PopID();
  }

  // Committed on release: applied live, the slider would rescale under the
  // cursor mid drag and jump between steps.
  void DrawUiScaleRow() {
    const auto* entry = rex::cvar::GetFlagInfo("ui_scale");
    if (!entry)
      return;
    const double value = std::atof(entry->getter().c_str());
    int idx = ui_scale_drag_idx_;
    if (idx < 0) {
      idx = 0;
      for (int i = 1; i < static_cast<int>(kUiScaleSteps.size()); ++i) {
        if (std::abs(kUiScaleSteps[i] - value) < std::abs(kUiScaleSteps[idx] - value))
          idx = i;
      }
    }
    char text[16];
    std::snprintf(text, sizeof(text), "%d%%%%",
                  static_cast<int>(std::lround(
                      (ui_scale_drag_idx_ < 0 ? value : kUiScaleSteps[idx]) * 100.0)));

    if (DrawRowLabel("UI Scale", "ui_scale")) {
      ImGui::SetTooltip("Size of these overlay windows and their text.");
    }
    ImGui::SameLine(Px(180.0f));
    ImGui::SetNextItemWidth(Px(160.0f));
    ImGui::PushID("ui_scale");
    if (ImGui::SliderInt("##v", &idx, 0, static_cast<int>(kUiScaleSteps.size()) - 1, text,
                         ImGuiSliderFlags_NoInput)) {
      ui_scale_drag_idx_ = idx;
    }
    if (ImGui::IsItemDeactivated() && ui_scale_drag_idx_ >= 0) {
      char step[16];
      std::snprintf(step, sizeof(step), "%g", kUiScaleSteps[ui_scale_drag_idx_]);
      rex::cvar::SetFlagByName("ui_scale", step, /*persist=*/true);
      SaveBasic();
      ui_scale_drag_idx_ = -1;
    }
    ImGui::PopID();
  }

  // The generic Double widget is a bare input box; a slider reads better, and
  // the file is written once the drag ends rather than on every frame of it.
  void DrawSensitivityRow(const char* label, const char* name) {
    const auto* entry = rex::cvar::GetFlagInfo(name);
    if (!entry)
      return;
    float value = static_cast<float>(std::atof(entry->getter().c_str()));
    if (DrawRowLabel(label, name) && !entry->description.empty()) {
      ImGui::SetTooltip("%s", entry->description.c_str());
    }
    ImGui::SameLine(Px(180.0f));
    ImGui::SetNextItemWidth(Px(160.0f));
    ImGui::PushID(name);
    if (ImGui::SliderFloat("##v", &value, 0.1f, 5.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) {
      char text[16];
      std::snprintf(text, sizeof(text), "%.2f", value);
      rex::cvar::SetFlagByName(name, text, /*persist=*/true);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      SaveBasic();
    }
    ImGui::PopID();
  }

  // audio_volume is a Double cvar (0.0-1.0 linear amplitude, applied directly
  // to samples by the SDL audio driver); DrawCvarWidget's generic Double path
  // is a plain InputDouble box, not a slider, so this draws its own row the
  // same way DrawRenderScaleRow does for resolution_scale -- displaying and
  // editing a perceptually-spaced percentage (see VolumeAmplitudeFromPercent)
  // rather than the raw amplitude directly.
  void DrawAudioVolumeRow() {
    const auto* entry = rex::cvar::GetFlagInfo("audio_volume");
    if (!entry)
      return;

    int percent = VolumePercentFromAmplitude(std::atof(entry->getter().c_str()));

    DrawRowLabel("Master Volume", "audio_volume");
    ImGui::SameLine(Px(180.0f));
    ImGui::SetNextItemWidth(Px(160.0f));
    ImGui::PushID("audio_volume");
    bool changed = ImGui::SliderInt("##v", &percent, 0, 100, "%d%%");
    if (changed) {
      rex::cvar::SetFlagByName("audio_volume", std::to_string(VolumeAmplitudeFromPercent(percent)),
                               /*persist=*/true);
      SaveBasic();
    }
    ImGui::PopID();
  }

  // The game's own three volume sliders, through the settings API so they go
  // through its mixer and land in the save exactly as the Options screen's do.
  // From this thread a write is queued to the next guest frame, so the value
  // being dragged is held here until then rather than snapping back.
  void DrawGameVolumeRow(const char* label, int setting, int slot) {
    const int current = EternalSonataGetSetting(setting);
    int& pending = volume_drag_[slot];
    int percent = pending >= 0 ? pending : current;

    if (DrawRowLabel(label, nullptr)) {
      ImGui::SetTooltip("The game's own %s, as in its Options screen. Saved with "
                        "your game data rather than the host settings.",
                        label);
    }
    ImGui::SameLine(Px(180.0f));
    ImGui::SetNextItemWidth(Px(160.0f));
    ImGui::PushID(setting);
    const bool available = current >= 0;
    if (!available) {
      percent = 0;
      ImGui::BeginDisabled();
    }
    if (ImGui::SliderInt("##v", &percent, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) {
      pending = percent;
      EternalSonataSetSetting(setting, percent);
    }
    if (!ImGui::IsItemActive() && pending == current) {
      pending = -1;
    }
    if (!available) {
      ImGui::EndDisabled();
    }
    ImGui::PopID();
  }

  // How fine a host timer tick to request, which is what lets the game's audio
  // sequencer run at the 200Hz it was written for. Applies live rather than
  // needing a restart: timeBeginPeriod takes effect immediately and the guest's
  // already-armed periodic timer is serviced by the system tick, so it changes
  // pace on its next expiry without being re-armed.
  //
  // Windows only: a POSIX host honours the guest's 5ms period natively, so
  // there is no trade-off to offer and host_timer_resolution_ms is not defined
  // there at all.
#if defined(_WIN32)
  void DrawTimerResolutionRow() {
    const auto* entry = rex::cvar::GetFlagInfo("host_timer_resolution_ms");
    if (!entry)
      return;
    const std::string current = entry->getter();
    int sel = 1;  // "Xbox 360", the default and the value that matches the guest
    for (int i = 0; i < static_cast<int>(kTimerResolutionOptions.size()); ++i) {
      if (current == kTimerResolutionOptions[i].id) {
        sel = i;
        break;
      }
    }

    if (DrawRowLabel("Audio Timing", "host_timer_resolution_ms")) {
      ImGui::SetTooltip(
          "How precisely the game's audio clock is allowed to run.\n\n"
          "\"Host\" leaves your system's own timer alone, which is too coarse "
          "for this game and makes everything the audio clock drives run about "
          "3x slow.\n\n"
          "\"Xbox 360\" matches the console: music and voices play at the tempo "
          "they were written for, and spoken lines are followed by a short "
          "pause before the next one, exactly as they were on the original "
          "hardware.\n\n"
          "\"Instantaneous\" keeps that same tempo but moves on to the next "
          "line as soon as the current one finishes, trimming those pauses. "
          "Down to taste; it draws more power and can spin fans up.");
    }
    ImGui::SameLine(Px(180.0f));
    // Match the combo boxes in this menu (Language, Input Backend, ...).
    ImGui::SetNextItemWidth(Px(160.0f));
    ImGui::PushID("host_timer_resolution_ms");
    // Discrete 0..N-1 slider; format shows the label of the current option
    // (re-evaluated per frame). NoInput keeps it snapping between presets.
    if (ImGui::SliderInt("##v", &sel, 0, static_cast<int>(kTimerResolutionOptions.size()) - 1,
                         kTimerResolutionOptions[sel].label, ImGuiSliderFlags_NoInput)) {
      // ApplyHostTimerResolution is registered as this cvar's change callback,
      // so writing it through SetFlagByName is what applies the new tick rate.
      rex::cvar::SetFlagByName("host_timer_resolution_ms", kTimerResolutionOptions[sel].id,
                               /*persist=*/true);
      SaveBasic();
    }
    ImGui::PopID();
  }
#endif  // _WIN32

  // Applies live: the extent is republished every present, so this rebuilds the
  // render targets the same way dragging the window's corner does.
  void DrawRenderScaleRow() {
    if (!RenderScaleRowAvailable())
      return;
    int idx = RenderScaleOptionIndex();
    char label[16];
    // ImGui runs the format string through printf itself, so the percent sign
    // has to survive that pass as well as this one.
    std::snprintf(label, sizeof(label), "%d%%%%", RenderScaleOptionPercent(idx));

    ImGui::PushID("render_scale");
    // Whichever cvar SetRenderScalePercent writes: only the fallback restarts.
    DrawRowLabel("Render Resolution", rex::cvar::GetFlagInfo("render_scale") ? "render_scale"
                                                                              : "resolution_scale");
    ImGui::SameLine(Px(180.0f));
    ImGui::SetNextItemWidth(Px(160.0f));
    // Discrete 0..N-1 slider; the format string carries the percentage.
    if (ImGui::SliderInt("##v", &idx, 0, RenderScaleOptionCount() - 1, label,
                         ImGuiSliderFlags_NoInput)) {
      SetRenderScaleOption(idx);
    }
    ImGui::PopID();
  }

  // The sampler every upscale of the world image uses, so it takes effect on the
  // very next frame.
  void DrawRenderFilterRow() { DrawCvarRow("Pixelated Scaling", "render_pixelated_scaling"); }

  // Applies to the next camera setup rather than instantly: the guest calls
  // sub_82108180 when a camera changes, so the view widens on the next cut or
  // area rather than under the player's feet.
  void DrawFieldOfViewRow() {
    const auto* entry = rex::cvar::GetFlagInfo("camera_fov_scale");
    if (!entry)
      return;
    int idx = CameraFovOptionIndex();
    char label[16];
    std::snprintf(label, sizeof(label), "%d%%%%", CameraFovOptionPercent(idx));

    ImGui::PushID("camera_fov_scale");
    DrawRowLabel("Field of View", "camera_fov_scale");
    ImGui::SameLine(Px(180.0f));
    ImGui::SetNextItemWidth(Px(160.0f));
    // Discrete 0..N-1 slider on the same steps the native Options gauge moves
    // through, so the two rows cannot land on values the other cannot show.
    if (ImGui::SliderInt("##v", &idx, 0, CameraFovOptionCount() - 1, label,
                         ImGuiSliderFlags_NoInput)) {
      SetCameraFovOption(idx);
    }
    ImGui::PopID();
  }

  void DrawLanguageRow() {
    const auto* entry = rex::cvar::GetFlagInfo("user_language");
    if (!entry)
      return;
    // The registry, not a fixed array: a translation mod's language shows up
    // here and in the game's own Options screen from the one registration.
    const std::vector<LanguageOption> options = GetLanguageOptions();
    const int cur_idx = UserLanguageIndex();

    DrawRowLabel("Language", nullptr);  // Applies live, whatever the SDK declares.
    ImGui::SameLine(Px(180.0f));
    ImGui::SetNextItemWidth(Px(160.0f));
    ImGui::PushID("user_language");
    if (ImGui::BeginCombo("##v", options[cur_idx].label)) {
      for (int i = 0; i < static_cast<int>(options.size()); ++i) {
        bool selected = (i == cur_idx);
        if (ImGui::Selectable(options[i].label, selected)) {
          // Goes through SetUserLanguageSetting rather than the cvar directly,
          // so the donor-slot override stops shadowing the selection.
          SetUserLanguageSetting(i);
          SaveBasic();
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    ImGui::PopID();
  }

  // Applies live; the setter persists on its own.
  void DrawVoiceLanguageRow() {
    const int count = VoiceLanguageCount();
    if (count <= 0)
      return;
    const int cur_idx = VoiceLanguageIndex();

    DrawRowLabel("Voice Language", "voice_language");
    ImGui::SameLine(Px(180.0f));
    ImGui::SetNextItemWidth(Px(160.0f));
    ImGui::PushID("voice_language");
    if (ImGui::BeginCombo("##v", VoiceLanguageLabel(cur_idx))) {
      for (int i = 0; i < count; ++i) {
        bool selected = (i == cur_idx);
        if (ImGui::Selectable(VoiceLanguageLabel(i), selected)) {
          // Also moves the guest's byte, which is what applies it live.
          EternalSonataSetSetting(ETERNALSONATA_SETTING_VOICE_LANGUAGE, i);
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    ImGui::PopID();
  }

  // Which character model the overworld leader wears. The game always spawns
  // Allegretto there regardless of party order; the spawn hook in
  // field_player_model_override.cpp substitutes a different cached model
  // handle. Selection is owned by FieldPlayerModelOverride (which mirrors the
  // field_leader_model cvar into an atomic for the guest thread), so this row
  // goes through it rather than touching the cvar directly.
  void DrawFieldLeaderModelRow() {
    int selection = eternalsonata::FieldPlayerModelOverride::Selection();

    if (DrawRowLabel("Overworld Model", "field_leader_model")) {
      ImGui::SetTooltip(
          "Which character is shown walking around the overworld.\n\n"
          "\"Party Leader\" uses whoever is first in the party, which you reorder "
          "from the status screen.\n\n"
          "The model can only be swapped while the field is paused, so a change takes "
          "effect the next time you close a menu or move between areas. "
          "Characters whose model has not been loaded yet fall back to the default.");
    }
    ImGui::SameLine(Px(180.0f));
    ImGui::SetNextItemWidth(Px(160.0f));
    ImGui::PushID("field_leader_model");
    if (ImGui::Combo("##v", &selection,
                     eternalsonata::FieldPlayerModelOverride::SelectionNames(),
                     eternalsonata::FieldPlayerModelOverride::kSelectionCount)) {
      // SetSelection persists via SaveUserSettings itself.
      eternalsonata::FieldPlayerModelOverride::SetSelection(selection);
    }
    ImGui::PopID();
  }

  void DrawFieldActionModelRow() {
    // Only matters when the leader wears a model other than the game's own.
    if (eternalsonata::FieldPlayerModelOverride::Selection() ==
        eternalsonata::FieldPlayerModelOverride::kSelectionDefault)
      return;
    DrawCvarRow("Compatible Actions", "field_action_default_model",
                "Temporarily use the story character model for chest, door, climbing, "
                "and other field animations. Disable this to keep the selected model, "
                "which may contort because those animations use an incompatible rig.");
  }

  // Controls the frame rate the game runs at. The hooks in
  // eternalsonata_framerate.cpp read the frame_rate cvar, declare that rate to the
  // sim (byte_82465F90) and hold the present thread to it with a host limiter.
  // Applied at runtime, no restart needed.
  void DrawFrameRateRow() {
    const int cur_idx = FrameRateOptionIndex();

    if (DrawRowLabel("Frame Rate", "frame_rate")) {
      ImGui::SetTooltip(
          "How often the in-game scene and simulation advance. The original "
          "game is capped at 30 FPS; Unlocked runs as fast as the CPU and GPU "
          "allow, at normal game speed. Adaptive targets 60 but drops to 30 "
          "rather than running the game in slow motion, returning to 60 once "
          "there is headroom again.");
    }
    ImGui::SameLine(Px(180.0f));
    // Match the combo boxes in this menu (Language, Input Backend, ...).
    ImGui::SetNextItemWidth(Px(160.0f));
    ImGui::PushID("frame_rate");
    int sel = cur_idx;
    // Discrete 0..N-1 slider; format shows the label of the current option
    // (re-evaluated per frame). NoInput keeps it snapping between presets.
    if (ImGui::SliderInt("##v", &sel, 0,
                         static_cast<int>(kFrameRateOptions.size()) - 1,
                         kFrameRateOptions[sel].label,
                         ImGuiSliderFlags_NoInput)) {
      // Sets both cvars, and persists them itself - hence no SaveBasic here.
      SetFrameRateOption(sel);
    }
    ImGui::PopID();
  }

#if REX_HAS_VULKAN
  // vulkan_device is a raw index into the physical-device list the Vulkan
  // provider enumerates at graphics setup time (-1 = auto-select). Entries
  // flagged is_duplicate_of_earlier are the same physical device as an
  // earlier entry (a driver/ICD quirk, not a second GPU) -- skipped here
  // since offering them would be a redundant, indistinguishable choice, not
  // just a duplicate label; the earlier entry's index selects the exact same
  // device.
  void DrawVulkanDeviceRow() {
    const auto* entry = rex::cvar::GetFlagInfo("vulkan_device");
    if (!entry || vulkan_devices_.empty())
      return;

    int current = std::atoi(entry->getter().c_str());
    auto label_for = [this](int real_idx) -> const std::string& {
      static const std::string kAuto = "Auto";
      return real_idx < 0 ? kAuto : vulkan_devices_[real_idx].name;
    };

    if (DrawRowLabel("Vulkan Device", "vulkan_device") && !entry->description.empty()) {
      ImGui::SetTooltip("%s", entry->description.c_str());
    }
    ImGui::SameLine(Px(180.0f));
    ImGui::SetNextItemWidth(Px(160.0f));
    ImGui::PushID("vulkan_device");
    if (ImGui::BeginCombo("##v", label_for(current).c_str())) {
      {
        bool selected = (current < 0);
        ImGui::PushID(-1);
        if (ImGui::Selectable("Auto", selected)) {
          rex::cvar::SetFlagByName("vulkan_device", "-1", /*persist=*/true);
          SaveBasic();
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
        ImGui::PopID();
      }
      for (int i = 0; i < static_cast<int>(vulkan_devices_.size()); ++i) {
        if (vulkan_devices_[i].is_duplicate_of_earlier)
          continue;
        bool selected = (current == i);
        ImGui::PushID(i);
        if (ImGui::Selectable(vulkan_devices_[i].name.c_str(), selected)) {
          rex::cvar::SetFlagByName("vulkan_device", std::to_string(i), /*persist=*/true);
          SaveBasic();
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
    ImGui::PopID();
  }
#endif

  rex::ui::Window* window_;
  std::filesystem::path user_settings_path_;
  std::filesystem::path app_config_path_;
#if REX_HAS_VULKAN
  std::vector<rex::ui::vulkan::DeviceInfo> vulkan_devices_;
#endif
  rex::input::InputSystem* input_system_;
  // The UI Scale step being dragged to, or -1 when not dragging.
  int ui_scale_drag_idx_ = -1;
  // Game volumes set but not yet applied by the guest, or -1; see
  // DrawGameVolumeRow.
  int volume_drag_[3] = {-1, -1, -1};
  std::unique_ptr<rex::ui::SettingsDialog> dev_settings_overlay_;

  rex::system::AutoUpdater auto_updater_;
  bool update_check_requested_ = false;
};

}  // namespace

// Ordered ascending; both this overlay's Resolution row and the native
// Options screen's Resolution row (eternalsonata_options.cpp) offer the same
// four presets, each truncated to what the display can actually show by
// AllowedResolutionCount.
constexpr std::array<const char*, 4> kResolutionPresetsAscending = {"720p", "1080p", "1440p", "4K"};

int AllowedResolutionCount() {
  int display_height = DesktopDisplayHeight();
  int count = 0;
  for (const char* opt : kResolutionPresetsAscending) {
    if (ResolutionHeightFor(opt) > display_height)
      break;
    ++count;
  }
  return count > 0 ? count : 1;  // Always leave at least 720p.
}

// ---------------------------------------------------------------------------
// Render resolution, as a percentage
// ---------------------------------------------------------------------------
//
// Backed by the native renderer's `render_scale` or, where only Xenos exists,
// by the integer `resolution_scale`; the percentage is the common language, and
// the integer path rounds to its nearest step in both directions.

// Below 30% the resolve rectangle's rounding shows on the EDRAM band edges.
constexpr int kRenderScaleMinPercent = 30;
constexpr int kRenderScaleStepPercent = 10;

// The largest resolution_scale worth offering: the one that renders at the
// display's own height.
static int IntegerRenderScaleBase() {
  const int display_height = DesktopDisplayHeight();
  if (display_height >= 2160) return 4;
  if (display_height >= 1440) return 3;
  if (display_height >= 1080) return 2;
  return 1;
}

static bool HasContinuousRenderScale() {
  return rex::cvar::GetFlagInfo("render_scale") != nullptr;
}

int RenderScaleOptionCount() {
  if (HasContinuousRenderScale()) {
    return (100 - kRenderScaleMinPercent) / kRenderScaleStepPercent + 1;
  }
  return rex::cvar::GetFlagInfo("resolution_scale") ? IntegerRenderScaleBase() : 0;
}

int RenderScaleOptionPercent(int index) {
  if (index < 0 || index >= RenderScaleOptionCount()) {
    return 100;
  }
  if (HasContinuousRenderScale()) {
    return kRenderScaleMinPercent + index * kRenderScaleStepPercent;
  }
  // Steps 1..base, so index 0 is the coarsest and the last is native.
  return static_cast<int>(
      std::lround(100.0 * (index + 1) / IntegerRenderScaleBase()));
}

// One valid step means there is nothing to offer, and the row is not drawn.
bool RenderScaleRowAvailable() { return RenderScaleOptionCount() > 1; }

int RenderScaleOptionIndex() {
  const int count = RenderScaleOptionCount();
  const int percent = RenderScalePercent();
  int best = 0;
  for (int i = 1; i < count; ++i) {
    if (std::abs(RenderScaleOptionPercent(i) - percent) <
        std::abs(RenderScaleOptionPercent(best) - percent)) {
      best = i;
    }
  }
  return best;
}

void SetRenderScaleOption(int index) {
  if (index < 0 || index >= RenderScaleOptionCount()) {
    return;
  }
  SetRenderScalePercent(RenderScaleOptionPercent(index));
}

int RenderScalePercent() {
  if (const auto* entry = rex::cvar::GetFlagInfo("render_scale")) {
    // Zero is the cvar's "no opinion", which renders at the window's own size.
    const double current = std::atof(entry->getter().c_str());
    const int percent =
        static_cast<int>(std::lround((current > 0.0 ? current : 1.0) * 100.0));
    return std::clamp(percent, kRenderScaleMinPercent, 100);
  }
  const auto* scale_entry = rex::cvar::GetFlagInfo("resolution_scale");
  if (!scale_entry) {
    return 100;
  }
  const int base = IntegerRenderScaleBase();
  const int scale = std::clamp(std::atoi(scale_entry->getter().c_str()), 1, base);
  return static_cast<int>(std::lround(100.0 * scale / base));
}

void SetRenderScalePercent(int percent) {
  if (HasContinuousRenderScale()) {
    percent = std::clamp(percent, kRenderScaleMinPercent, 100);
    rex::cvar::SetFlagByName("render_scale", std::to_string(percent / 100.0),
                             /*persist=*/true);
    SaveUserSettings();
    return;
  }
  if (!rex::cvar::GetFlagInfo("resolution_scale")) {
    return;
  }
  const int base = IntegerRenderScaleBase();
  const int scale = std::clamp(
      static_cast<int>(std::lround(percent * base / 100.0)), 1, base);
  rex::cvar::SetFlagByName("resolution_scale", std::to_string(scale),
                           /*persist=*/true);
  SaveUserSettings();
}

// ---------------------------------------------------------------------------
// Camera field of view, as a percentage
// ---------------------------------------------------------------------------
//
// The bounds are the cvar's own .range(0.5, 2.0), in tens like the render
// scale beside it: the native Options row is a gauge that moves one step per
// press, so the whole range stays a manageable number of presses.
constexpr int kCameraFovMinPercent = 50;
constexpr int kCameraFovMaxPercent = 200;
constexpr int kCameraFovStepPercent = 10;

int CameraFovOptionCount() {
  return (kCameraFovMaxPercent - kCameraFovMinPercent) / kCameraFovStepPercent + 1;
}

int CameraFovOptionPercent(int index) {
  if (index < 0 || index >= CameraFovOptionCount()) {
    return 100;
  }
  return kCameraFovMinPercent + index * kCameraFovStepPercent;
}

int CameraFovPercent() {
  const int percent =
      static_cast<int>(std::lround(REXCVAR_GET(camera_fov_scale) * 100.0));
  return std::clamp(percent, kCameraFovMinPercent, kCameraFovMaxPercent);
}

// Nearest step, so a value the overlay's continuous slider set still lands on
// a row value rather than falling off the end.
int CameraFovOptionIndex() {
  const int steps = (CameraFovPercent() - kCameraFovMinPercent + kCameraFovStepPercent / 2) /
                    kCameraFovStepPercent;
  return std::clamp(steps, 0, CameraFovOptionCount() - 1);
}

void SetCameraFovPercent(int percent) {
  percent = std::clamp(percent, kCameraFovMinPercent, kCameraFovMaxPercent);
  rex::cvar::SetFlagByName("camera_fov_scale", std::to_string(percent / 100.0),
                           /*persist=*/true);
  SaveUserSettings();
}

void SetCameraFovOption(int index) {
  if (index < 0 || index >= CameraFovOptionCount()) {
    return;
  }
  SetCameraFovPercent(CameraFovOptionPercent(index));
}

void ApplySettingDefaults() {
  for (const auto& d : kGameDefaults) {
    rex::cvar::SetDefaultValue(d.cvar, d.value);
  }
}

void InitSettingsCaches() {
  // Latch the boot language before anything can change it (see
  // ApplyBootLanguageDonorSlot). The voice selection is resolved here too, now that
  // mods have registered their voice languages.
  BootUserLanguageIndex();
  g_active_voice.store(VoiceLanguageIndex());
#if REX_HAS_VULKAN
  g_vulkan_devices_cache = rex::ui::vulkan::EnumerateDevices();
#endif
}

void BindSettingsTargets(rex::ui::Window* window,
                         std::filesystem::path user_settings_path) {
  g_window = window;
  g_user_settings_path = std::move(user_settings_path);
  rex::cvar::RegisterChangeCallback("fullscreen", [](std::string_view, std::string_view) {
    SaveUserSettings();
  });
}

bool IsCvarPendingRestart(const char* name) {
  return name && CvarPendingRestart(name);
}

bool AnyCvarPendingRestart() { return AnyKnownPendingRestart(); }

bool RestartNow() {
  if (!g_window) {
    return false;
  }
  if (!rex::platform::process::Relaunch()) {
    return false;
  }
  // RequestClose ends up in WindowSDL::RequestCloseImpl, which destroys the
  // window directly rather than going through SDL's event queue, so it has to
  // run on the thread that owns the window. Neither caller is guaranteed to be
  // on it: the game's own Options screen runs on the guest CPU thread, and so
  // does the F4 overlay under the native renderer, which draws it from the
  // guest's render thread. Calling straight through from there hangs the close
  // (the title is torn down from inside itself), leaving the old window on
  // screen contending with the relaunched one for the GPU and audio devices.
  // Marshalling covers every caller.
  rex::ui::Window* window = g_window;
  window->app_context().CallInUIThread([window] { window->RequestClose(); });
  return true;
}

void SaveUserSettings() {
  if (g_user_settings_path.empty()) {
    return;
  }
  SaveBasicCvars(g_user_settings_path);
}

void PersistWindowSize() {
  if (g_user_settings_path.empty() || g_window == nullptr) {
    return;
  }
  // Fullscreen size is the display's, not a window size worth coming back to.
  if (g_window->IsFullscreen()) {
    return;
  }
  // Logical, because that is the unit window_width/window_height are read in;
  // the renderer's own extent is in physical pixels and would grow the window
  // on every launch on a scaled display.
  const uint32_t width = g_window->GetActualLogicalWidth();
  const uint32_t height = g_window->GetActualLogicalHeight();
  if (width == 0 || height == 0 || width > 8192 || height > 8192) {
    return;
  }

  auto* width_entry = rex::cvar::GetFlagInfo("window_width");
  auto* height_entry = rex::cvar::GetFlagInfo("window_height");
  if (!width_entry || !height_entry) {
    return;
  }
  const std::string want_width = std::to_string(width);
  const std::string want_height = std::to_string(height);
  if (width_entry->getter() == want_width && height_entry->getter() == want_height) {
    return;
  }

  // Both are kRequiresRestart, and they are deliberately outside
  // kBasicCvarNames: the settings UI's pending-restart banner scans that list,
  // and a resize the player just performed is already in effect on screen.
  rex::cvar::SetFlagByName("window_width", want_width, /*persist=*/true);
  rex::cvar::SetFlagByName("window_height", want_height, /*persist=*/true);
  rex::cvar::SaveConfigSubset(g_user_settings_path, {"window_width", "window_height"});
}

void SetFrameRateSetting(const char* value) {
  auto* entry = rex::cvar::GetFlagInfo("frame_rate");
  if (!entry || !entry->setter || entry->getter() == value) {
    return;
  }
  // frame_rate is hot-reload, not kRequiresRestart, so this doesn't mark a
  // pending restart -- but going through SetFlagByName (as SetResolutionSetting
  // does) rather than entry->setter directly still matters: it's what runs
  // registered change callbacks and sets persist_to_config, same as every
  // other settings path in this file (DrawFrameRateRow included).
  rex::cvar::SetFlagByName("frame_rate", value, /*persist=*/true);
  SaveUserSettings();
}

int FrameRateOptionCount() {
  return static_cast<int>(kFrameRateOptions.size());
}

const char* FrameRateOptionLabel(int index) {
  if (index < 0 || index >= static_cast<int>(kFrameRateOptions.size())) {
    return nullptr;
  }
  return kFrameRateOptions[index].label;
}

int FrameRateOptionIndex() {
  const std::string cur = rex::cvar::GetFlagByName("frame_rate");
  for (int i = 0; i < static_cast<int>(kFrameRateOptions.size()); ++i) {
    if (cur == kFrameRateOptions[i].id) {
      return i;
    }
  }
  // Anything unrecognised - notably "stock", the old name for "30" - reads as
  // the first entry, which is what the hooks themselves fall back to.
  return 0;
}

void SetFrameRateOption(int index) {
  if (index < 0 || index >= static_cast<int>(kFrameRateOptions.size())) {
    return;
  }
  SetFrameRateSetting(kFrameRateOptions[index].id);
}

int MaxLanguageOptions() {
  return ETERNALSONATA_MAX_ROW_VALUES;
}

std::vector<LanguageOption> GetLanguageOptions() {
  std::vector<LanguageOption> options(kBuiltinLanguages.begin(), kBuiltinLanguages.end());
  options.reserve(options.size() + g_mod_languages.size());
  for (const auto& mod : g_mod_languages) {
    options.push_back({mod.id.c_str(), mod.label.c_str(), mod.code.c_str(),
                       mod.btx_slot.empty() ? nullptr : mod.btx_slot.c_str()});
  }
  return options;
}

bool RegisterModLanguage(std::string_view id, std::string_view label, std::string_view code,
                         std::string_view slot) {
  ModLanguage entry;
  entry.id = std::string(id);
  entry.label = std::string(label);
  entry.code = std::string(code);
  entry.btx_slot = NormalizeBtxSlot(slot);

  if (entry.id.empty() || entry.label.empty()) {
    REXLOG_WARN("[settings] ignoring a language with an empty id or label");
    return false;
  }
  // The Text row draws the code, so it cannot be left blank; the label's first
  // two characters are the obvious stand-in and match what the built-ins do.
  if (entry.code.empty()) {
    entry.code = entry.label.substr(0, 2);
  }
  for (char& c : entry.code)
    c = char(std::toupper(static_cast<unsigned char>(c)));

  for (const auto& opt : GetLanguageOptions()) {
    if (entry.id == opt.id) {
      REXLOG_WARN("[settings] ignoring duplicate language id {} ('{}'); '{}' registered it first",
                  entry.id, entry.label, opt.label);
      return false;
    }
  }
  // Sharing a slot with a *built-in* language is the whole point: there are
  // seven BTX blocks and no eighth to add, so a new language borrows one and
  // the built-in that owns it becomes unselectable. Only another mod-added
  // language is a real conflict, because then neither could say which of them
  // the block's text belongs to.
  if (!entry.btx_slot.empty()) {
    for (const auto& mod : g_mod_languages) {
      if (mod.btx_slot != entry.btx_slot)
        continue;
      REXLOG_WARN(
          "[settings] '{}' cannot claim BTX slot '{}': '{}' already borrowed it, so its text "
          "patches are dropped (its other patches still apply)",
          entry.label, entry.btx_slot, mod.label);
      return false;
    }
  }
  if (static_cast<int>(kBuiltinLanguages.size() + g_mod_languages.size()) >= MaxLanguageOptions()) {
    REXLOG_WARN("[settings] ignoring language '{}': the list is full at {} entries", entry.label,
                MaxLanguageOptions());
    return false;
  }

  REXLOG_INFO("[settings] language '{}' added as id {} ({}), BTX slot '{}'", entry.label, entry.id,
              entry.code, entry.btx_slot.empty() ? "none" : entry.btx_slot);
  g_mod_languages.push_back(std::move(entry));
  return true;
}

int MaxVoiceLanguageOptions() {
  return ETERNALSONATA_MAX_ROW_VALUES;
}

std::vector<VoiceLanguageOption> GetVoiceLanguageOptions() {
  std::vector<VoiceLanguageOption> options(kBuiltinVoiceLanguages.begin(),
                                           kBuiltinVoiceLanguages.end());
  options.reserve(options.size() + g_mod_voice_languages.size());
  for (const auto& mod : g_mod_voice_languages) {
    options.push_back(
        {mod.id.c_str(), mod.label.c_str(), mod.code.c_str(), mod.suffix.c_str(), -1});
  }
  return options;
}

bool RegisterModVoiceLanguage(std::string_view id, std::string_view label, std::string_view code,
                              std::string_view suffix) {
  ModVoiceLanguage entry;
  entry.id = std::string(id);
  entry.label = std::string(label);
  entry.code = std::string(code);
  entry.suffix = NormalizeVoiceSuffix(suffix.empty() ? id : suffix);

  for (char& c : entry.id)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  if (entry.id.empty() || entry.label.empty()) {
    REXLOG_WARN("[settings] ignoring a voice language with an empty id or label");
    return false;
  }
  if (entry.code.empty())
    entry.code = entry.label.substr(0, 2);
  for (char& c : entry.code)
    c = char(std::toupper(static_cast<unsigned char>(c)));

  // The one hard limit, and it is tight: an index.vmtoc record's path field is
  // 32 bytes, and the longest voice path a mod can make is
  // "btldata\voice\bosfga" + suffix + ".csf". Truncating the record instead
  // would serve the bank under a path nothing ever asks for, which fails as
  // silence rather than as an error.
  if (entry.suffix.size() > kMaxVoiceSuffixBytes) {
    REXLOG_WARN(
        "[settings] ignoring voice language '{}': its suffix '{}' is {} bytes and only {} fit in "
        "an index.vmtoc path record",
        entry.label, entry.suffix, entry.suffix.size(), kMaxVoiceSuffixBytes);
    return false;
  }

  for (const auto& opt : GetVoiceLanguageOptions()) {
    if (entry.id == opt.id) {
      REXLOG_WARN(
          "[settings] ignoring duplicate voice language id '{}' ('{}'); '{}' registered it first",
          entry.id, entry.label, opt.label);
      return false;
    }
    // Unlike a BTX slot, a voice suffix is never shared: two mods writing
    // pcNNN_x.csf would each build the same cache path from different clips,
    // and the built-ins' own suffixes are what the fall-back chain depends on.
    if (entry.suffix == opt.suffix) {
      REXLOG_WARN("[settings] ignoring voice language '{}': '{}' already uses suffix '{}'",
                  entry.label, opt.label, entry.suffix);
      return false;
    }
  }
  if (static_cast<int>(kBuiltinVoiceLanguages.size() + g_mod_voice_languages.size()) >=
      MaxVoiceLanguageOptions()) {
    REXLOG_WARN("[settings] ignoring voice language '{}': the list is full at {} entries",
                entry.label, MaxVoiceLanguageOptions());
    return false;
  }

  REXLOG_INFO("[settings] voice language '{}' added as id '{}' ({}), bank suffix '{}'", entry.label,
              entry.id, entry.code, entry.suffix);
  g_mod_voice_languages.push_back(std::move(entry));
  return true;
}

int VoiceLanguageCount() {
  return static_cast<int>(GetVoiceLanguageOptions().size());
}

const char* VoiceLanguageCode(int index) {
  const auto options = GetVoiceLanguageOptions();
  if (index < 0 || index >= static_cast<int>(options.size()))
    return nullptr;
  return options[index].code;
}

const char* VoiceLanguageLabel(int index) {
  const auto options = GetVoiceLanguageOptions();
  if (index < 0 || index >= static_cast<int>(options.size()))
    return nullptr;
  return options[index].label;
}

int VoiceLanguageGuestByte(int index) {
  const auto options = GetVoiceLanguageOptions();
  if (index < 0 || index >= static_cast<int>(options.size()))
    return -1;
  return options[index].guest_byte;
}

int VoiceLanguageIndexForGuestByte(int guest_byte) {
  const auto options = GetVoiceLanguageOptions();
  for (int i = 0; i < static_cast<int>(options.size()); ++i) {
    if (options[i].guest_byte == guest_byte)
      return i;
  }
  return 0;
}

int VoiceLanguageIndex() {
  const auto* entry = rex::cvar::GetFlagInfo("voice_language");
  const std::string current = entry ? entry->getter() : std::string();
  const auto options = GetVoiceLanguageOptions();
  for (int i = 0; i < static_cast<int>(options.size()); ++i) {
    if (current == options[i].id)
      return i;
  }
  // Same unknown-id fallback as UserLanguageIndex, and for the same reason: a
  // mod that added a voice language and was then disabled leaves its id in the
  // config, and that has to read as the first entry rather than clamp.
  return 0;
}

void SetVoiceLanguageSetting(int index) {
  const auto options = GetVoiceLanguageOptions();
  if (index < 0 || index >= static_cast<int>(options.size()))
    return;
  rex::cvar::SetFlagByName("voice_language", options[index].id, /*persist=*/true);
  g_active_voice.store(index);
  SaveUserSettings();
}

int ActiveVoiceLanguageIndex() { return g_active_voice.load(); }

int ActiveVoiceKey() {
  const int builtin = static_cast<int>(kBuiltinVoiceLanguages.size());
  const int index = g_active_voice.load();
  return index < builtin ? -1 : kModVoiceKeyBase + (index - builtin);
}

const char* ActiveVoiceSuffix() {
  const int index = g_active_voice.load();
  if (index < static_cast<int>(kBuiltinVoiceLanguages.size()))
    return nullptr;  // The guest's own byte decides; the hook stands down.
  const auto options = GetVoiceLanguageOptions();
  return index < static_cast<int>(options.size()) ? options[index].suffix : nullptr;
}

void RegisterLanguageListeners(rex::system::ModRegistry* registry) {
  if (!registry)
    return;

  registry->Subscribe(
      "settings.voice_language_option",
      [](const rex::system::ModRegistry::EventPayload& payload) {
        // "id|Label|CODE|SUFFIX"; everything past the label is optional.
        // payload.bytes only lives for this call, so it is copied.
        std::string spec(reinterpret_cast<const char*>(payload.bytes.data()),
                         payload.bytes.size());
        std::string fields[4];
        size_t field = 0, start = 0;
        for (size_t i = 0; i <= spec.size() && field < 4; ++i) {
          if (i == spec.size() || spec[i] == '|') {
            fields[field++] = spec.substr(start, i - start);
            start = i + 1;
          }
        }
        RegisterModVoiceLanguage(fields[0], fields[1], fields[2], fields[3]);
      });

  registry->Subscribe(
      "settings.language_option", [](const rex::system::ModRegistry::EventPayload& payload) {
        // "Label", or "Label|CODE|SLOT" for a mod that wants to pick both
        // without a second publish.
        std::string spec(reinterpret_cast<const char*>(payload.bytes.data()),
                         payload.bytes.size());
        std::string fields[3];
        size_t field = 0, start = 0;
        for (size_t i = 0; i <= spec.size() && field < 3; ++i) {
          if (i == spec.size() || spec[i] == '|') {
            fields[field++] = spec.substr(start, i - start);
            start = i + 1;
          }
        }
        RegisterModLanguage(std::to_string(payload.u64), fields[0], fields[1], fields[2]);
      });

  registry->Subscribe(
      "settings.language_slot", [](const rex::system::ModRegistry::EventPayload& payload) {
        const std::string id = std::to_string(payload.u64);
        const std::string slot = NormalizeBtxSlot(std::string_view(
            reinterpret_cast<const char*>(payload.bytes.data()), payload.bytes.size()));
        if (slot.empty()) {
          REXLOG_WARN("[settings] ignoring settings.language_slot for id {} with an empty fourcc",
                      id);
          return;
        }
        ModLanguage* target = nullptr;
        for (auto& mod : g_mod_languages) {
          if (mod.id == id)
            target = &mod;
        }
        if (!target) {
          REXLOG_WARN(
              "[settings] ignoring settings.language_slot '{}' for id {}: no such language was "
              "registered (publish settings.language_option first)",
              slot, id);
          return;
        }
        if (!target->btx_slot.empty()) {
          REXLOG_WARN("[settings] '{}' already owns BTX slot '{}'; ignoring '{}'", target->label,
                      target->btx_slot, slot);
          return;
        }
        // Built-ins are excluded on purpose; borrowing one is the mechanism.
        // See the matching note in RegisterModLanguage.
        for (const auto& mod : g_mod_languages) {
          if (mod.btx_slot == slot) {
            REXLOG_WARN("[settings] '{}' cannot claim BTX slot '{}': '{}' already borrowed it",
                        target->label, slot, mod.label);
            return;
          }
        }
        target->btx_slot = slot;
        REXLOG_INFO("[settings] language '{}' claimed BTX slot '{}'", target->label, slot);
      });

  registry->Subscribe(
      "settings.native_string", [](const rex::system::ModRegistry::EventPayload& payload) {
        const std::string_view kv(reinterpret_cast<const char*>(payload.bytes.data()),
                                  payload.bytes.size());
        const size_t eq = kv.find('=');
        if (eq == std::string_view::npos) {
          REXLOG_WARN("[settings] ignoring a settings.native_string payload with no '='");
          return;
        }
        RegisterNativeString(uint32_t(payload.u64), kv.substr(0, eq), kv.substr(eq + 1));
      });
}

bool RegisterNativeString(uint32_t language_id, std::string_view key, std::string_view value) {
  if (key.empty() || value.empty()) {
    REXLOG_WARN("[settings] ignoring a translated string for language {} with an empty {}",
                language_id, key.empty() ? "key" : "value");
    return false;
  }
  std::string map_key = std::to_string(language_id) + ":" + std::string(key);
  if (g_native_strings.contains(map_key)) {
    REXLOG_WARN("[settings] ignoring duplicate translation of '{}' for language {}", key,
                language_id);
    return false;
  }
  g_native_strings.emplace(std::move(map_key), std::string(value));
  return true;
}

const char* FindNativeString(uint32_t language_id, std::string_view key) {
  const auto it = g_native_strings.find(std::to_string(language_id) + ":" + std::string(key));
  return it != g_native_strings.end() ? it->second.c_str() : nullptr;
}

int UserLanguageCount() {
  return static_cast<int>(GetLanguageOptions().size());
}

const char* UserLanguageCode(int index) {
  const auto options = GetLanguageOptions();
  if (index < 0 || index >= static_cast<int>(options.size())) {
    return nullptr;
  }
  return options[index].code;
}

const char* UserLanguageLabel(int index) {
  const auto options = GetLanguageOptions();
  if (index < 0 || index >= static_cast<int>(options.size())) {
    return nullptr;
  }
  return options[index].label;
}

// What the player has selected, which is not always what the cvar holds:
// ApplyBootLanguageDonorSlot may have pointed the live cvar at a donor language
// so the guest boots into the right BTX block. Until the player changes the
// setting this session, the boot selection is the honest answer.
std::string SelectedLanguageId() {
  if (g_language_donor_applied && !g_language_selection_changed) {
    return g_boot_language_id;
  }
  const auto* entry = rex::cvar::GetFlagInfo("user_language");
  return entry ? entry->getter() : std::string();
}

int UserLanguageIndex() {
  const std::string current = SelectedLanguageId();
  const auto options = GetLanguageOptions();
  for (int i = 0; i < static_cast<int>(options.size()); ++i) {
    if (current == options[i].id) {
      return i;
    }
  }
  // Unrecognised, which is the normal state after a mod that added a language
  // is disabled with its id still saved: fall back to the first entry rather
  // than clamping to the last, or the player ends up in a language they never
  // chose.
  return 0;
}

uint32_t UserLanguageId() {
  const auto options = GetLanguageOptions();
  const int index = UserLanguageIndex();
  if (index < 0 || index >= static_cast<int>(options.size()))
    return 0;
  return uint32_t(std::strtoul(options[index].id, nullptr, 10));
}

uint32_t BootUserLanguageId() {
  const auto options = GetLanguageOptions();
  const int index = BootUserLanguageIndex();
  if (index < 0 || index >= static_cast<int>(options.size()))
    return 0;
  return uint32_t(std::strtoul(options[index].id, nullptr, 10));
}

void ApplyBootLanguageDonorSlot() {
  BootUserLanguageIndex();  // Latch first: the write below moves the cvar.
  const auto options = GetLanguageOptions();
  const int index = BootUserLanguageIndex();
  if (index < static_cast<int>(kBuiltinLanguages.size())) {
    return;  // A built-in language; the guest already understands its id.
  }
  const char* slot = options[index].btx_slot;
  if (!slot) {
    REXLOG_WARN(
        "[settings] language '{}' claimed no BTX slot, so the guest keeps its own text; only "
        "the labels this host draws will be translated",
        options[index].label);
    return;
  }
  // The donor is the built-in language that owns the same block. Booting the
  // guest as that language is what makes it read the block the mod patched.
  for (const auto& builtin : kBuiltinLanguages) {
    if (std::string_view(builtin.btx_slot) != slot)
      continue;
    // entry->setter, not SetFlagByName: SetFlagByName is what records a change
    // with MarkPendingRestart, and this is not a change the player made. Left
    // on the pending list it would read as "user_language was just edited"
    // forever, and leaving the main-menu Options screen relaunches the process
    // whenever anything is pending (see eternalsonata_options.cpp's
    // OnLeaveMainMenuOptions), so the game would restart every time the player
    // so much as opened Options. It also must not persist: the config has to
    // keep naming the language the player actually chose.
    auto* entry = rex::cvar::GetFlagInfo("user_language");
    if (!entry || !entry->setter || !entry->setter(builtin.id)) {
      REXLOG_WARN("[settings] could not point the guest at BTX slot '{}' for '{}'", slot,
                  options[index].label);
      return;
    }
    g_language_donor_applied = true;
    REXLOG_INFO(
        "[settings] booting the guest as {} (id {}) so '{}' reads BTX slot '{}'; {} is not "
        "selectable while that mod is enabled",
        builtin.label, builtin.id, options[index].label, slot, builtin.label);
    return;
  }
  REXLOG_WARN("[settings] language '{}' claims BTX slot '{}', which no built-in language owns",
              options[index].label, slot);
}

int BootUserLanguageIndex() {
  // The *id* is captured on the first call and never again; the index is worked
  // out fresh each time, since a mod-added language only joins the list once
  // that mod has registered and an index latched before then would be stale.
  // InitSettingsCaches calls this at startup so the latch happens before the
  // overlay (or the native Text row) can move the cvar; the lazy form here is
  // only a safety net for callers that run earlier.
  if (!g_boot_language_latched) {
    g_boot_language_latched = true;
    const auto* entry = rex::cvar::GetFlagInfo("user_language");
    g_boot_language_id = entry ? entry->getter() : std::string();
  }
  const auto options = GetLanguageOptions();
  for (int i = 0; i < static_cast<int>(options.size()); ++i) {
    if (g_boot_language_id == options[i].id) {
      return i;
    }
  }
  return 0;  // Same unknown-id fallback as UserLanguageIndex.
}

void SetUserLanguageSetting(int index) {
  const auto options = GetLanguageOptions();
  if (index < 0 || index >= static_cast<int>(options.size())) {
    return;
  }
  // From here on the live cvar is what the player picked, donor override or
  // not.
  g_language_selection_changed = true;
  // entry->setter, not SetFlagByName: the SDK declares user_language
  // kRequiresRestart, but here it applies live, so it must not be marked
  // pending. A mod language writes its donor's block.
  if (auto* entry = rex::cvar::GetFlagInfo("user_language"); entry && entry->setter)
    entry->setter(options[index].id);
  SaveUserSettings();
  WriteGuestTextLanguage(options[index].btx_slot);
}

void SetResolutionSetting(const char* value) {
  auto* res_entry = rex::cvar::GetFlagInfo("resolution");
  // resolution_scale is defined by the Xenos GPU plugin, so it does not exist
  // at all when another plugin (plume) is loaded
  auto* scale_entry = rex::cvar::GetFlagInfo("resolution_scale");
  if (!res_entry || !res_entry->setter || res_entry->getter() == value) {
    return;
  }
  // resolution and resolution_scale are both kRequiresRestart -- go through
  // rex::cvar::SetFlagByName (not entry->setter directly, as the other
  // Set*Setting helpers in this file do) so the change is recorded by
  // MarkPendingRestart. That's what makes AnyPendingRestart() /
  // GetPendingRestartFlags() -- and so the overlay's "restart to apply"
  // banner -- notice a resolution change made from the native Options row,
  // not just from this file's own DrawResolutionRow.
  rex::cvar::SetFlagByName("resolution", value, /*persist=*/true);
  if (scale_entry && scale_entry->setter) {
    rex::cvar::SetFlagByName("resolution_scale", std::to_string(ResolutionScaleFor(value)),
                             /*persist=*/true);
  }
  SaveUserSettings();
}

float UiScale() { return static_cast<float>(std::clamp(REXCVAR_GET(ui_scale), 0.5, 3.0)); }

std::unique_ptr<rex::ui::ImGuiDialog> CreateUiScaleApplier(rex::ui::ImGuiDrawer* drawer) {
  return std::make_unique<UiScaleApplier>(drawer);
}

std::unique_ptr<rex::ui::ImGuiDialog> CreateSettingsDialog(
    rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window,
    std::filesystem::path user_settings_path, std::filesystem::path app_config_path,
    rex::input::InputSystem* input_system) {
  g_user_settings_path = user_settings_path;
  g_window = window;
  return std::make_unique<CuratedSettingsDialog>(drawer, window, std::move(user_settings_path),
                                                 std::move(app_config_path), input_system);
}

}  // namespace eternalsonata
