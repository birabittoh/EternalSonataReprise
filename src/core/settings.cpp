// eternalsonata - ReXGlue Recompiled Project
// See settings.h for details.

#include "settings.h"

#include "eternalsonata_asset_system.h"
#include "eternalsonata_battle_api.h"
#include "eternalsonata_options_api.h"
#include "eternalsonata_settings_api.h"

// Exported by game_settings.cpp for mods; the overlay uses the same entry points.
extern "C" int EternalSonataGetSetting(int setting);
extern "C" int EternalSonataSetSetting(int setting, int value);
// Exported by special_attack_input.cpp.
extern "C" int EternalSonataGetEffectiveRightTriggerBehavior(void);
#include "field_player_model_override.h"
#include "game_data.h"
#include "game_settings.h"
#include "host_menu.h"
#include "host_timer_resolution.h"
#include "ui_text.h"
#include "native_renderer.h"
#include "ui_language.h"
#include "touch_input_driver.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
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
#if defined(__ANDROID__)
#include "android_gpu_driver.h"
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

REXCVAR_DEFINE_BOOL(native_deferred_recording, true, "Eternal Sonata",
                    "Record the frame's Vulkan commands on a worker thread instead of the "
                    "guest's render thread. Read at startup; Vulkan only.");

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

REXCVAR_DEFINE_DOUBLE(display_gamma, 1.0, "Eternal Sonata",
                      "Gamma applied on top of the console display ramp when presenting. 1 matches "
                      "the console; above 1 brightens.")
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
              "jazz", "falsetto", "claves", "march", "crescendo", "serenade"});

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

// Applied live by button_glyphs.cpp; disabled keeps the release's own art.
REXCVAR_DEFINE_STRING(override_button_glyphs, "disabled", "Eternal Sonata",
                      "Button prompt art: disabled (the release's own), xbox or ps3")
    .allowed({"disabled", "xbox", "ps3"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Applied live by button_glyphs.cpp; auto follows whichever the player used last.
REXCVAR_DEFINE_STRING(prompt_device, "auto", "Eternal Sonata",
                      "Prompt device: auto (the last one used), controller or mouse_keyboard")
    .allowed({"auto", "controller", "mouse_keyboard"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Read by the sub_821C6E68 hook in battle_system.cpp.
REXCVAR_DEFINE_BOOL(auto_parry, false, "Eternal Sonata",
                    "Counter and parry automatically without pressing any buttons");

// Read every frame by the present hook in eternalsonata_framerate.cpp. Unpaced
// fast forward runs as fast as the host presents, so its speed varies by scene.
REXCVAR_DEFINE_DOUBLE(fast_forward_max_speed, 3.0, "Eternal Sonata",
                      "Fast forward speed cap as a multiple of normal speed, 0 for unlimited")
    .range(0.0, 20.0);

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
    DefaultValue{"game_data_root", "assets"},
    // Static mirror of the mod catalog, attached to every release of the mods
    // repo; used when the Firestore backend is rate limited or down.
    DefaultValue{"mod_catalog_fallback_url",
                 "https://github.com/birabittoh/EternalSonataReprise-Mods/releases/latest/"
                 "download/catalog.json"},
    // The swap chain's own vsync (native_renderer_plume.cpp). The frame limiter
    // paces in the host, independent of it; see eternalsonata_framerate.cpp.
    DefaultValue{"vsync", "true"},
    DefaultValue{"mnk_mode", "true"},
#if defined(__ANDROID__)
    DefaultValue{"gyro_aim", "false"},
#else
    DefaultValue{"gyro_aim", "true"},
#endif
    DefaultValue{"keybind_a", "LMB"},
    DefaultValue{"keybind_b", "RMB"},
    DefaultValue{"keybind_x", "MMB"},
    DefaultValue{"keybind_y", "Space"},
    DefaultValue{"keybind_left_trigger", "Alt"},
    DefaultValue{"keybind_right_trigger", "Control"},
    DefaultValue{"keybind_left_shoulder", "WheelUp"},
    DefaultValue{"keybind_right_shoulder", "WheelDown"},
    DefaultValue{"keybind_lstick_press", "T"},
    DefaultValue{"keybind_back", "Backspace"},
    DefaultValue{"keybind_start", "Escape"},
    // Shift+arrows would collide with the Shift fast forward key.
    DefaultValue{"keybind_dpad_up", ""},
    DefaultValue{"keybind_dpad_down", ""},
    DefaultValue{"keybind_dpad_left", ""},
    DefaultValue{"keybind_dpad_right", ""},
    DefaultValue{"ui_mode_button", "Back"},
    DefaultValue{"fullscreen", "false"},
    DefaultValue{"audio_mute", "false"},
    DefaultValue{"audio_volume", "1"},
};

// cvars persisted to the friendly settings.toml by the Basic section.
// vulkan_device gets a custom row (a dynamic dropdown) rather than the generic
// DrawCvarWidget path, but is still listed here so the generic Reset-All /
// restart-tracking loops cover it; GetFlagInfo/ResetToDefault etc. no-op
// harmlessly for it on a build without Vulkan. gpu_backend only has a row on
// Windows, the one platform with a choice.
// host_timer_resolution_ms is likewise listed unconditionally even though the
// cvar only exists on Windows, for the same reason vulkan_device is: the
// generic loops no-op on a name that is not registered, and keeping the list
// platform-independent means a settings.toml written on Windows round-trips
// unharmed through a Linux build.
// vsync is registered by the native renderer (see RegisterNativeRendererCvars),
// not by the SDK.
// voice_language is listed even though user_language needed special handling to
// be (see SaveBasicCvars): this subset is saved from *live* cvar values, and the
// live voice_language is always exactly what the player chose. There is no
// donor rewrite for voice (a mod voice language gets a bank path of its own
// rather than borrowing a built-in's), so nothing ever shadows it.
constexpr std::array kBasicCvarNames = {
    "fullscreen", "user_language",
    "input_backend", "gpu_backend", "vulkan_device", "frame_rate",
    "audio_mute", "audio_volume", "field_leader_model", "field_action_default_model",
    "host_timer_resolution_ms", "vsync", "voice_language", "render_scale",
    "camera_fov_scale", "display_gamma", "render_pixelated_scaling", "render_msaa", "render_fxaa", "render_anisotropy", "render_generated_mips", "aim_invert_x", "aim_invert_y",
    "touch_controls", "touch_function_keys", "touch_scale", "touch_opacity", "touch_hide_delay", "gyro_aim", "gyro_sensitivity", "gyro_invert_x", "gyro_invert_y", "fast_forward_button", "ui_mode_button", "override_button_glyphs", "prompt_device",
    "fast_forward_max_speed",
    "enemy_exp_multiplier", "benched_exp_multiplier", "incapacitated_exp_multiplier",
    "enemy_gold_multiplier", "photo_price_multiplier", "photo_price_algorithm",
    "progression_curves",
    "enemy_hp_multiplier",
    "enemy_damage_multiplier", "auto_parry", "party_level_select", "ui_scale",
    "right_trigger_behavior", "special_hold_time", "ranged_special_buffer",
    "party_tactical_time_multiplier", "party_action_time_multiplier",
    "enemy_action_time_multiplier",
    "save_row_portraits", "force_japanese_font", "skip_intro", "show_start_screen",
    "discord_activity"};

// Steps for the multiplier rows. HP stops short of zero, the
// same floor enemy_hp_multiplier has.
constexpr std::array kRewardMultiplierSteps = {0.0, 0.25, 0.5, 0.75, 1.0, 1.5,
                                               2.0, 3.0,  4.0, 5.0,  10.0};
constexpr std::array kFastForwardSpeedSteps = {0.0, 2.0, 3.0, 4.0, 5.0, 6.0, 8.0, 10.0};
constexpr std::array kExpShareSteps = {0.0, 0.25, 0.5, 0.75, 1.0};
constexpr std::array kUiScaleSteps = {0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0};
// The last step of each is the cvars' "no limit" value.
constexpr std::array kTacticalTimeSteps = {0.0, 0.25, 0.5, 0.75, 1.0, 1.5,
                                           2.0, 3.0,  4.0, 5.0,  10.0, 11.0};
constexpr std::array kActionTimeSteps = {0.25, 0.5, 0.75, 1.0, 1.5, 2.0,
                                         3.0,  4.0, 5.0,  10.0, 11.0};
constexpr std::array kHpMultiplierSteps ={0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 4.0, 5.0, 10.0};

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
    // Last, so the five PAL display list slots keep their indices. Every
    // release ships its text; it is the only text the JP release has.
    LanguageOption{"2", "Japanese", "JP", "JPN "},
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

// The backend a gpu_backend value ends up using. Auto picks D3D12 on Windows.
std::string EffectiveGpuBackend(const std::string& value) {
  return value == "any" || value.empty() ? "d3d12" : value;
}

// gpu_backend as of the first call, which happens before the player can change it.
const std::string& LaunchGpuBackend() {
  static const std::string value = rex::cvar::GetFlagByName("gpu_backend");
  return value;
}

// True once `name`'s cvar has actually been changed at runtime this session
// and needs a relaunch to take effect. GetPendingRestartFlags() only tracks
// cvars changed at runtime (settings UI, console, mods), not values applied
// while loading a config file at boot, so a saved preference that merely
// differs from the SDK's factory default doesn't trip it on a fresh launch.
// See SetFlagByNameImpl's mark_restart parameter in the SDK's cvar.cpp.
bool CvarPendingRestart(const char* name) {
  auto pending = rex::cvar::GetPendingRestartFlags();
  if (std::find(pending.begin(), pending.end(), name) == pending.end())
    return false;
  // "any" and "d3d12" run the same backend, so swapping them needs no relaunch.
  if (std::string_view(name) == "gpu_backend")
    return EffectiveGpuBackend(rex::cvar::GetFlagByName("gpu_backend")) !=
           EffectiveGpuBackend(LaunchGpuBackend());
  return true;
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
    if (is_tracked(name) && CvarPendingRestart(name))
      return true;
  }
  return false;
}

// Remembered from CreateSettingsDialog so settings changed outside the overlay
// - e.g. the native Fullscreen row in the game's own Options screen - can be
// persisted to the same file the overlay writes. See SaveUserSettings.
std::filesystem::path g_user_settings_path;
rex::ui::Window* g_window = nullptr;

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
// changing (frame rate, fullscreen) is enough to trigger that,
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

// The overlay's text, from the [settings] table of res/lang.
const char* T(const char* key) { return Tr("settings", key); }

// A row's tooltip is its key plus "_tip"; `fallback` when no language has one.
const char* Tip(const char* key, const char* fallback = nullptr) {
  thread_local std::string tip;
  tip.assign(key).append("_tip");
  return TrOr("settings", tip.c_str(), fallback);
}

// A label with a fixed ImGui id, so the window and tabs keep their state when
// the language changes.
const char* WithId(const char* text, const char* id) {
  thread_local std::string label;
  label.assign(text).append("##").append(id);
  return label.c_str();
}

// A modal asking to confirm a restart. Call every frame; `open` requests it.
// `id` keeps the popups of different buttons apart.
bool ConfirmRestart(const char* id, bool open, const char* message_key = "restart_confirm",
                    const char* yes_key = "restart_yes") {
  const char* popup = WithId(T("restart_title"), id);
  if (open) {
    ImGui::OpenPopup(popup);
  }
  bool confirmed = false;
  ImVec2 center = ImGui::GetMainViewport()->GetCenter();
  ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (ImGui::BeginPopupModal(popup, nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
    ImGui::TextUnformatted(T(message_key));
    ImGui::Spacing();
    if (ImGui::Button(T(yes_key))) {
      confirmed = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("restart_cancel"))) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  return confirmed;
}

// The label of a value that has a key per option, else the value as is.
const char* OptionText(const char* prefix, const char* id, const char* fallback) {
  thread_local std::string key;
  key.assign(prefix).append(id);
  return TrOr("settings", key.c_str(), fallback);
}

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
    if (!ImGui::Begin(WithId(T("title"), "rex"), nullptr,
                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::End();
      return;
    }

    if (AnyPendingRestart()) {
      ImGui::PushStyleColor(ImGuiCol_Text, kRestartColor);
      ImGui::TextWrapped("%s", T("restart_banner"));
      ImGui::PopStyleColor();
      ImGui::SameLine();
      bool open_confirm = ImGui::SmallButton(WithId(T("restart_now"), "restart"));
      if (ConfirmRestart("confirm_restart", open_confirm)) {
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
      if (ImGui::BeginTabItem(WithId(T("tab_graphics"), "graphics"))) {
#ifdef _WIN32
        // Elsewhere Vulkan is the only backend.
        DrawGpuBackendRow();
#endif
#if REX_HAS_VULKAN
#ifdef _WIN32
        if (rex::cvar::GetFlagByName("gpu_backend") == "vulkan")
#endif
          DrawVulkanDeviceRow();
#endif
        DrawUiScaleRow();
        DrawRenderScaleRow();
        DrawFieldOfViewRow();
        DrawGammaRow();
        DrawFrameRateRow();
        DrawAnisotropyRow();
        DrawAntiAliasingRow();
        DrawCvarRow("render_fxaa");
        DrawCvarRow("render_generated_mips");
        DrawRenderFilterRow();
        DrawCvarRow("fullscreen");
        DrawCvarRow("vsync");
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem(WithId(T("tab_audio"), "audio"))) {
        DrawCvarRow("audio_mute");
        DrawAudioVolumeRow();
        DrawGameVolumeRow("music_volume", ETERNALSONATA_SETTING_VOLUME_MUSIC, 0);
        DrawGameVolumeRow("sfx_volume", ETERNALSONATA_SETTING_VOLUME_SFX, 1);
        DrawGameVolumeRow("voice_volume", ETERNALSONATA_SETTING_VOLUME_VOICE, 2);
#if defined(_WIN32)
        DrawTimerResolutionRow();
#endif
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem(WithId(T("tab_input"), "input"))) {
        DrawInputBackendRow();
        DrawCvarRow("aim_invert_x");
        DrawCvarRow("aim_invert_y");
        DrawCvarRow("touch_controls");
        if (rex::cvar::GetFlagByName("touch_controls") == "true") {
          DrawCvarRow("touch_function_keys");
          DrawSensitivityRow("touch_scale", 0.5f, 2.0f, true);
          DrawSensitivityRow("touch_opacity", 0.1f, 1.0f, true);
          DrawSensitivityRow("touch_hide_delay", 1.0f, 11.0f, true, true);
        }
        DrawCvarRow("fast_forward_button");
        DrawMultiplierRow("fast_forward_max_speed", kFastForwardSpeedSteps, true);
        DrawUiModeButtonRow();
        ImGui::Separator();
        DrawCvarRow("gyro_aim");
        if (rex::cvar::GetFlagByName("gyro_aim") == "true") {
          DrawSensitivityRow("gyro_sensitivity");
          DrawCvarRow("gyro_invert_x");
          DrawCvarRow("gyro_invert_y");
        }
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem(WithId(T("tab_game"), "game"))) {
        DrawLanguageRow();
        DrawVoiceLanguageRow();
        DrawCvarRow("save_row_portraits");
        DrawCvarRow("skip_intro");
        // A release with only Japanese text is already on the Japanese font.
        if (WesternTextAvailable()) {
          DrawCvarRow("force_japanese_font");
        }
        DrawPromptDeviceRow();
        DrawFieldLeaderModelRow();
        DrawFieldActionModelRow();
        DrawCvarRow("discord_activity");
        DrawCvarRow("show_start_screen");
        DrawStartScreenRow();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem(WithId(T("tab_platform"), "platform"))) {
        DrawOverrideButtonGlyphsRow();
        DrawPlatformChoiceRow("photo_price_algorithm");
        DrawPlatformChoiceRow("progression_curves");
        DrawPlatformChoiceRow("ranged_special_buffer");
        DrawRightTriggerRow();
        if (EternalSonataGetEffectiveRightTriggerBehavior() ==
            ETERNALSONATA_BATTLE_RIGHT_TRIGGER_CAMERA) {
          DrawSensitivityRow("special_hold_time", 0.1f, 5.0f, false, false, 0.1f);
        }
        ImGui::Separator();
        if (ImGui::Button(WithId(T("all_auto"), "all_auto"))) {
          SetAllPlatform("auto", "disabled", "auto");
        }
        ImGui::SameLine();
        if (ImGui::Button(WithId(T("all_xbox"), "all_xbox"))) {
          SetAllPlatform("360", "xbox", "camera");
        }
        ImGui::SameLine();
        if (ImGui::Button(WithId(T("all_ps3"), "all_ps3"))) {
          SetAllPlatform("ps3", "ps3", "special");
        }
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem(WithId(T("tab_balance"), "balance"))) {
        DrawMultiplierRow("enemy_exp_multiplier", kRewardMultiplierSteps);
        DrawMultiplierRow("benched_exp_multiplier", kExpShareSteps);
        DrawMultiplierRow("incapacitated_exp_multiplier", kExpShareSteps);
        DrawMultiplierRow("enemy_gold_multiplier", kRewardMultiplierSteps);
        DrawMultiplierRow("photo_price_multiplier", kRewardMultiplierSteps);
        DrawMultiplierRow("enemy_hp_multiplier", kHpMultiplierSteps);
        DrawMultiplierRow("enemy_damage_multiplier", kRewardMultiplierSteps);
        DrawMultiplierRow("party_tactical_time_multiplier", kTacticalTimeSteps, false, true);
        DrawMultiplierRow("party_action_time_multiplier", kActionTimeSteps, false, true);
        DrawMultiplierRow("enemy_action_time_multiplier",
                          std::span(kActionTimeSteps).first(kActionTimeSteps.size() - 1));
        DrawCvarRow("auto_parry");
        DrawPartyLevelRow();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }

    ImGui::Separator();
    if (ImGui::Button(T("reset_all"))) {
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
    if (ImGui::Button(dev_settings_overlay_ ? T("close_all_settings") : T("all_settings"))) {
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
      ImGui::TextWrapped("%s", T("update_downloaded"));
      ImGui::PopStyleColor();
      ImGui::SameLine();
#if REX_PLATFORM_ANDROID
      // The system installer takes it from here and replaces the app itself;
      // closing the game would only dismiss the installer prompt.
      if (ImGui::SmallButton(WithId(T("install_update"), "autoupdate"))) {
        rex::system::AutoUpdater::ApplyAndRestart(rex::system::AutoUpdater::InstallRoot(), {});
      }
#else
      bool open_confirm = ImGui::SmallButton(WithId(T("restart_apply"), "autoupdate"));
      if (ConfirmRestart("confirm_update", open_confirm)) {
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
        char percent[16];
        std::snprintf(percent, sizeof(percent), "%.0f%%",
                      100.0 * static_cast<double>(install.downloaded_bytes) /
                          static_cast<double>(install.total_bytes));
        ImGui::TextDisabled("%s", Tr("settings", "downloading_percent", percent).c_str());
      } else {
        ImGui::TextDisabled("%s", T("downloading"));
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
    ImGui::TextWrapped("%s", Tr("settings", "update_available", "v" + info->version).c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::SmallButton(T("download_update"))) {
      auto_updater_.InstallAsync(*info, rex::system::AutoUpdater::InstallRoot());
    }
  }

  // A row's label, with a yellow asterisk when `cvar` only takes effect after a
  // restart, and in yellow itself once it has been changed and is waiting for
  // one. Returns whether the label was hovered, for the row's own tooltip.
  bool DrawRowLabel(const char* key, const char* cvar) {
    const char* label = T(key);
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
        ImGui::SetTooltip("%s", T("requires_restart"));
      }
    }
    return hovered;
  }

  // A plain cvar row: label, the SDK's generic widget, and the cvar's own
  // description as a tooltip unless the row has a better one.
  void DrawCvarRow(const char* name) {
    const auto* entry = rex::cvar::GetFlagInfo(name);
    if (!entry)
      return;
    if (DrawRowLabel(name, name)) {
      if (const char* tip = Tip(name, entry->description.empty() ? nullptr
                                                               : entry->description.c_str())) {
        ImGui::SetTooltip("%s", tip);
      }
    }
    ImGui::SameLine(Px(180.0f));
    ImGui::PushID(name);
    if (rex::ui::DrawCvarWidget(*entry, Px(160.0f), /*persist=*/true)) {
      SaveBasic();
    }
    ImGui::PopID();
  }

  // ui_mode_button picked from a few safe choices; a hand edited combination
  // still shows up as its own entry.
  void DrawUiModeButtonRow() {
    const auto* entry = rex::cvar::GetFlagInfo("ui_mode_button");
    if (!entry)
      return;
    auto label = [](std::string id) {
      for (size_t at = id.find('+'); at != std::string::npos; at = id.find('+', at + 3))
        id.replace(at, 1, " + ");
      return id;
    };
    std::vector<std::string> opts = {"Guide", "Back", "Back+Start", "LThumb+RThumb"};
    const std::string current = entry->getter();
    if (std::find(opts.begin(), opts.end(), current) == opts.end())
      opts.push_back(current);
    if (DrawRowLabel("ui_mode_button", "ui_mode_button")) {
      if (const char* tip = Tip("ui_mode_button", entry->description.c_str()))
        ImGui::SetTooltip("%s", tip);
    }
    ImGui::SameLine(Px(180.0f));
    ImGui::SetNextItemWidth(Px(160.0f));
    ImGui::PushID("ui_mode_button");
    if (ImGui::BeginCombo("##v", label(current).c_str())) {
      for (const auto& opt : opts) {
        if (ImGui::Selectable(label(opt).c_str(), opt == current)) {
          rex::cvar::SetFlagByName("ui_mode_button", opt, /*persist=*/true);
          SaveBasic();
        }
      }
      ImGui::EndCombo();
    }
    ImGui::PopID();
  }

  // gpu_backend with display names for its stored values (any, vulkan, d3d12).
  // Windows only, the one platform with a choice.
#ifdef _WIN32
  void DrawGpuBackendRow() {
    const auto* entry = rex::cvar::GetFlagInfo("gpu_backend");
    if (!entry)
      return;
    const auto& opts = entry->constraints.allowed_values;
    if (opts.empty()) {
      DrawCvarRow("gpu_backend");
      return;
    }
    auto label = [](const std::string& id) {
      if (id == "any" || id == "d3d12")
        return OptionText("backend_", "any", "DirectX 12");
      if (id == "vulkan")
        return OptionText("backend_", "vulkan", "Vulkan");
      return id.c_str();
    };
    const std::string current = entry->getter();
    if (DrawRowLabel("gpu_backend", "gpu_backend")) {
      if (const char* tip = Tip("gpu_backend", entry->description.empty()
                                                   ? nullptr
                                                   : entry->description.c_str())) {
        ImGui::SetTooltip("%s", tip);
      }
    }
    ImGui::SameLine(Px(180.0f));
    ImGui::SetNextItemWidth(Px(160.0f));
    ImGui::PushID("gpu_backend");
    if (ImGui::BeginCombo("##v", label(current))) {
      for (const auto& opt : opts) {
        if (opt == "d3d12")
          continue;
        if (ImGui::Selectable(label(opt), opt == current || (opt == "any" && current == "d3d12"))) {
          rex::cvar::SetFlagByName("gpu_backend", opt, /*persist=*/true);
          SaveBasic();
        }
      }
      ImGui::EndCombo();
    }
    ImGui::PopID();
  }
#endif  // _WIN32

  // input_backend with display names for its stored values (sdl, xinput).
  void DrawInputBackendRow() {
    const auto* entry = rex::cvar::GetFlagInfo("input_backend");
    if (!entry)
      return;
    const auto& opts = entry->constraints.allowed_values;
    if (opts.empty()) {
      DrawCvarRow("input_backend");
      return;
    }
    auto label = [](const std::string& id) -> const char* {
      if (id == "sdl")
        return "SDL";
      if (id == "xinput")
        return "XInput";
      return id.c_str();
    };
    const std::string current = entry->getter();
    if (DrawRowLabel("input_backend", "input_backend")) {
      if (const char* tip = Tip("input_backend", entry->description.empty()
                                                     ? nullptr
                                                     : entry->description.c_str())) {
        ImGui::SetTooltip("%s", tip);
      }
    }
    ImGui::SameLine(Px(180.0f));
    ImGui::SetNextItemWidth(Px(160.0f));
    ImGui::PushID("input_backend");
    if (ImGui::BeginCombo("##v", label(current))) {
      for (const auto& opt : opts) {
        if (ImGui::Selectable(label(opt), opt == current)) {
          rex::cvar::SetFlagByName("input_backend", opt, /*persist=*/true);
          SaveBasic();
        }
      }
      ImGui::EndCombo();
    }
    ImGui::PopID();
  }

  // Discrete steps, so the shipped balance is always exactly one notch. The
  // label shows the cvar's real value, which a hand edited config may have
  // put between two steps.
  void DrawMultiplierRow(const char* name, std::span<const double> steps,
                         bool zero_is_unlimited = false, bool infinite_at_max = false) {
    const auto* entry = rex::cvar::GetFlagInfo(name);
    if (!entry)
      return;
    const double value = std::atof(entry->getter().c_str());
    int idx = 0;
    for (int i = 1; i < static_cast<int>(steps.size()); ++i) {
      if (std::abs(steps[i] - value) < std::abs(steps[idx] - value))
        idx = i;
    }
    const bool infinite = infinite_at_max && idx == static_cast<int>(steps.size()) - 1;
    char text[16];
    if (infinite) {
      text[0] = '\0';
    } else if (zero_is_unlimited && value == 0.0) {
      std::snprintf(text, sizeof(text), "%s", T("unlimited"));
    } else {
      std::snprintf(text, sizeof(text), "%gx", value);
    }

    if (DrawRowLabel(name, name)) {
      ImGui::SetTooltip("%s", Tip(name, name));
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
    if (infinite) {
      DrawInfinitySign();
    }
    ImGui::PopID();
  }

  // A lemniscate over the last item, since the font has no infinity sign.
  void DrawInfinitySign() {
    const ImVec2 lo = ImGui::GetItemRectMin();
    const ImVec2 hi = ImGui::GetItemRectMax();
    const ImVec2 mid((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
    const float a = (hi.y - lo.y) * 0.62f;
    ImVec2 pts[48];
    for (int i = 0; i < 48; ++i) {
      const float t = 6.2831853f * float(i) / 48.0f;
      const float s = std::sin(t);
      const float k = 1.0f / (1.0f + s * s);
      pts[i] = ImVec2(mid.x + a * std::cos(t) * k, mid.y + a * 0.9f * s * std::cos(t) * k);
    }
    ImGui::GetWindowDrawList()->AddPolyline(pts, 48, ImGui::GetColorU32(ImGuiCol_Text),
                                            ImDrawFlags_Closed, std::max(1.5f, a * 0.14f));
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

    if (DrawRowLabel("ui_scale", "ui_scale")) {
      ImGui::SetTooltip("%s", Tip("ui_scale", ""));
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
  void DrawSensitivityRow(const char* name, float min = 0.1f, float max = 5.0f,
                          bool touch_preview = false, bool infinite_at_max = false, float step = 0.0f) {
    const auto* entry = rex::cvar::GetFlagInfo(name);
    if (!entry)
      return;
    float value = static_cast<float>(std::atof(entry->getter().c_str()));
    if (DrawRowLabel(name, name)) {
      if (const char* tip = Tip(name, entry->description.c_str()))
        ImGui::SetTooltip("%s", tip);
    }
    ImGui::SameLine(Px(180.0f));
    ImGui::SetNextItemWidth(Px(160.0f));
    ImGui::PushID(name);
    // Whole steps, the last one standing for "never".
    if (infinite_at_max) {
      value = std::round(value);
    }
    if (step > 0.0f) {
      value = std::round(value / step) * step;
    }
    const bool infinite = infinite_at_max && value >= max;
    if (ImGui::SliderFloat("##v", &value, min, max,
                           infinite ? "" : (infinite_at_max ? "%.0f" : (step > 0.0f ? "%.1f" : "%.2f")),
                           ImGuiSliderFlags_AlwaysClamp)) {
      if (infinite_at_max) {
        value = std::round(value);
      }
      if (step > 0.0f) {
        value = std::round(value / step) * step;
      }
      char text[16];
      std::snprintf(text, sizeof(text), "%.2f", value);
      rex::cvar::SetFlagByName(name, text, /*persist=*/true);
    }
    if (infinite) {
      DrawInfinitySign();
    }
    if (touch_preview && ImGui::IsItemActive()) {
      eternalsonata::touch::TouchKeepVisible();
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      SaveBasic();
    }
    ImGui::PopID();
  }

  // audio_volume is a Double cvar (0.0-1.0 linear amplitude, applied directly
  // to samples by the SDL audio driver); DrawCvarWidget's generic Double path
  // is a plain InputDouble box, not a slider, so this draws its own row the
  // same way DrawRenderScaleRow does for render_scale: displaying and
  // editing a perceptually-spaced percentage (see VolumeAmplitudeFromPercent)
  // rather than the raw amplitude directly.
  void DrawAudioVolumeRow() {
    const auto* entry = rex::cvar::GetFlagInfo("audio_volume");
    if (!entry)
      return;

    int percent = VolumePercentFromAmplitude(std::atof(entry->getter().c_str()));

    DrawRowLabel("audio_volume", "audio_volume");
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
  // Restarts into the start screen, where another version can be picked,
  // added or deleted.
  void DrawStartScreenRow() {
    bool open_confirm = ImGui::Button(WithId(T("start_screen"), "start_screen"));
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", T("start_screen_tip"));
    }
    if (ConfirmRestart("confirm_start_screen", open_confirm)) {
      RestartIntoStartScreen();
    }
    ImGui::SameLine();
    bool open_quit = ImGui::Button(WithId(T("quit_game"), "quit_game"));
    if (ConfirmRestart("confirm_quit", open_quit, "quit_confirm", "quit_yes")) {
      QuitNow();
    }
  }

  void DrawGameVolumeRow(const char* key, int setting, int slot) {
    const int current = EternalSonataGetSetting(setting);
    int& pending = volume_drag_[slot];
    int percent = pending >= 0 ? pending : current;

    if (DrawRowLabel(key, nullptr)) {
      ImGui::SetTooltip("%s", Tr("settings", "game_volume_tip", T(key)).c_str());
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

    if (DrawRowLabel("host_timer_resolution_ms", "host_timer_resolution_ms")) {
      ImGui::SetTooltip("%s", Tip("host_timer_resolution_ms"));
    }
    ImGui::SameLine(Px(180.0f));
    // Match the combo boxes in this menu (Language, Input Backend, ...).
    ImGui::SetNextItemWidth(Px(160.0f));
    ImGui::PushID("host_timer_resolution_ms");
    // Discrete 0..N-1 slider; format shows the label of the current option
    // (re-evaluated per frame). NoInput keeps it snapping between presets.
    if (ImGui::SliderInt("##v", &sel, 0, static_cast<int>(kTimerResolutionOptions.size()) - 1,
                         OptionText("timer_", kTimerResolutionOptions[sel].id,
                                    kTimerResolutionOptions[sel].label),
                         ImGuiSliderFlags_NoInput)) {
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
    int idx = RenderScaleOptionIndex();
    char label[16];
    // ImGui runs the format string through printf itself, so the percent sign
    // has to survive that pass as well as this one.
    std::snprintf(label, sizeof(label), "%d%%%%", RenderScaleOptionPercent(idx));

    ImGui::PushID("render_scale");
    DrawRowLabel("render_scale", "render_scale");
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
  void DrawRenderFilterRow() { DrawCvarRow("render_pixelated_scaling"); }

  // The 3D scene's sample count. Applies live: the renderer rebuilds its targets
  // the way a resize does.
  void DrawAntiAliasingRow() {
    const auto* entry = rex::cvar::GetFlagInfo("render_msaa");
    if (!entry)
      return;
    static constexpr const char* kSamples[] = {"1", "2", "4", "8"};
    static constexpr const char* kFallback[] = {"Off", "2x (Xbox 360)", "4x", "8x"};
    const std::string current = entry->getter();
    int sel = 0;
    for (int i = 0; i < 4; ++i) {
      if (current == kSamples[i])
        sel = i;
    }

    if (DrawRowLabel("render_msaa", "render_msaa")) {
      ImGui::SetTooltip("%s", Tip("render_msaa"));
    }
    ImGui::SameLine(Px(180.0f));
    ImGui::SetNextItemWidth(Px(160.0f));
    ImGui::PushID("render_msaa");
    if (ImGui::SliderInt("##v", &sel, 0, 3, OptionText("msaa_", kSamples[sel], kFallback[sel]),
                         ImGuiSliderFlags_NoInput)) {
      rex::cvar::SetFlagByName("render_msaa", kSamples[sel], /*persist=*/true);
      SaveBasic();
    }
    ImGui::PopID();
  }

  // Applies live: samplers are keyed on it and the draw path drops its cached
  // bindings on a change.
  void DrawAnisotropyRow() {
    const auto* entry = rex::cvar::GetFlagInfo("render_anisotropy");
    if (!entry)
      return;
    static constexpr const char* kRatios[] = {"1", "2", "4", "8", "16"};
    static constexpr const char* kFallback[] = {"Off", "2x", "4x", "8x", "16x"};
    const std::string current = entry->getter();
    int sel = 0;
    for (int i = 0; i < 5; ++i) {
      if (current == kRatios[i])
        sel = i;
    }

    if (DrawRowLabel("render_anisotropy", "render_anisotropy")) {
      ImGui::SetTooltip("%s", Tip("render_anisotropy"));
    }
    ImGui::SameLine(Px(180.0f));
    ImGui::SetNextItemWidth(Px(160.0f));
    ImGui::PushID("render_anisotropy");
    if (ImGui::SliderInt("##v", &sel, 0, 4, OptionText("aniso_", kRatios[sel], kFallback[sel]),
                         ImGuiSliderFlags_NoInput)) {
      rex::cvar::SetFlagByName("render_anisotropy", kRatios[sel], /*persist=*/true);
      SaveBasic();
    }
    ImGui::PopID();
  }

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
    DrawRowLabel("camera_fov_scale", "camera_fov_scale");
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

  void DrawGammaRow() {
    if (!rex::cvar::GetFlagInfo("display_gamma"))
      return;
    int idx = DisplayGammaOptionIndex();

    ImGui::PushID("display_gamma");
    if (DrawRowLabel("display_gamma", "display_gamma")) {
      ImGui::SetTooltip("%s", Tip("display_gamma", ""));
    }
    ImGui::SameLine(Px(180.0f));
    ImGui::SetNextItemWidth(Px(160.0f));
    if (ImGui::SliderInt("##v", &idx, 0, DisplayGammaOptionCount() - 1,
                         DisplayGammaOptionLabel(idx), ImGuiSliderFlags_NoInput)) {
      SetDisplayGammaOption(idx);
    }
    ImGui::PopID();
  }

  // The interface and the story text share this choice.
  void DrawLanguageRow() {
    const int cur_idx = UserLanguageIndex();

    DrawRowLabel("ui_language", "ui_language");
    ImGui::SameLine(Px(180.0f));
    ImGui::SetNextItemWidth(Px(160.0f));
    ImGui::PushID("ui_language");
    if (ImGui::BeginCombo("##v", UserLanguageLabel(cur_idx))) {
      for (int i : VisibleUserLanguages()) {
        bool selected = (i == cur_idx);
        if (ImGui::Selectable(UserLanguageLabel(i), selected)) {
          SetUserLanguageSetting(i);
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

    DrawRowLabel("voice_language", "voice_language");
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

    if (DrawRowLabel("field_leader_model", "field_leader_model")) {
      ImGui::SetTooltip("%s", Tip("field_leader_model"));
    }
    ImGui::SameLine(Px(180.0f));
    ImGui::SetNextItemWidth(Px(160.0f));
    ImGui::PushID("field_leader_model");
    if (ImGui::Combo("##v", &selection,
                     eternalsonata::FieldPlayerModelOverride::SelectionNames(),
                     eternalsonata::FieldPlayerModelOverride::SelectionCount())) {
      // SetSelection persists via SaveUserSettings itself.
      eternalsonata::FieldPlayerModelOverride::SetSelection(selection);
    }
    ImGui::PopID();
  }

  void SetAllPlatform(const char* choice, const char* glyphs, const char* trigger) {
    for (const char* name :
         {"photo_price_algorithm", "progression_curves", "ranged_special_buffer"}) {
      rex::cvar::SetFlagByName(name, choice, /*persist=*/true);
    }
    rex::cvar::SetFlagByName("override_button_glyphs", glyphs, /*persist=*/true);
    rex::cvar::SetFlagByName("right_trigger_behavior", trigger, /*persist=*/true);
    SaveBasic();
  }

  struct RadioOption {
    const char* id;
    const char* label;
  };

  // A string cvar picked from a few options, laid out side by side. Platform
  // names stay untranslated.
  void DrawRadioRow(const char* name, std::span<const RadioOption> options) {
    const auto* entry = rex::cvar::GetFlagInfo(name);
    if (!entry)
      return;
    const std::string current = entry->getter();
    int cur_idx = 0;
    for (int i = 0; i < static_cast<int>(options.size()); ++i) {
      if (current == options[i].id)
        cur_idx = i;
    }

    if (DrawRowLabel(name, name)) {
      ImGui::SetTooltip("%s", Tip(name));
    }
    ImGui::PushID(name);
    for (int i = 0; i < static_cast<int>(options.size()); ++i) {
      ImGui::SameLine(i == 0 ? Px(180.0f) : 0.0f);
      if (ImGui::RadioButton(options[i].label, cur_idx == i)) {
        rex::cvar::SetFlagByName(name, options[i].id, /*persist=*/true);
        SaveBasic();
      }
    }
    ImGui::PopID();
  }

  void DrawPlatformChoiceRow(const char* name) {
    const RadioOption options[] = {{"auto", T("auto")}, {"360", "Xbox 360"}, {"ps3", "PS3"}};
    DrawRadioRow(name, options);
  }

  void DrawOverrideButtonGlyphsRow() {
    const RadioOption options[] = {
        {"disabled", T("glyphs_disabled")}, {"xbox", "Xbox 360"}, {"ps3", "PS3"}};
    DrawRadioRow("override_button_glyphs", options);
  }

  void DrawPartyLevelRow() {
    const RadioOption options[] = {{"encore", T("party_level_encore")},
                                   {"always", T("party_level_always")}};
    DrawRadioRow("party_level_select", options);
  }

  void DrawRightTriggerRow() {
    const RadioOption options[] = {
        {"auto", T("auto")}, {"camera", T("rt_camera")}, {"special", T("rt_special")}};
    DrawRadioRow("right_trigger_behavior", options);
  }

  void DrawPromptDeviceRow() {
    const auto* entry = rex::cvar::GetFlagInfo("prompt_device");
    if (!entry)
      return;
    struct Option {
      const char* id;
      const char* label;
    };
    const std::array<Option, 3> options = {{{"mouse_keyboard", T("prompt_mouse_keyboard")},
                                            {"controller", T("prompt_controller")},
                                            {"auto", T("auto")}}};
    const std::string current = entry->getter();
    int cur_idx = 2;
    for (int i = 0; i < static_cast<int>(options.size()); ++i) {
      if (current == options[i].id)
        cur_idx = i;
    }

    if (DrawRowLabel("prompt_device", "prompt_device")) {
      ImGui::SetTooltip("%s", Tip("prompt_device"));
    }
    ImGui::SameLine(Px(180.0f));
    ImGui::SetNextItemWidth(Px(160.0f));
    ImGui::PushID("prompt_device");
    if (ImGui::BeginCombo("##v", options[cur_idx].label)) {
      for (int i = 0; i < static_cast<int>(options.size()); ++i) {
        const bool selected = i == cur_idx;
        if (ImGui::Selectable(options[i].label, selected)) {
          rex::cvar::SetFlagByName("prompt_device", options[i].id, /*persist=*/true);
          SaveBasic();
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    ImGui::PopID();
  }

  void DrawFieldActionModelRow() {
    // Only matters when the leader wears a model other than the game's own.
    if (eternalsonata::FieldPlayerModelOverride::Selection() ==
        eternalsonata::FieldPlayerModelOverride::kSelectionDefault)
      return;
    DrawCvarRow("field_action_default_model");
  }

  // Controls the frame rate the game runs at. The hooks in
  // eternalsonata_framerate.cpp read the frame_rate cvar, declare that rate to the
  // sim (byte_82465F90) and hold the present thread to it with a host limiter.
  // Applied at runtime, no restart needed.
  void DrawFrameRateRow() {
    const int cur_idx = FrameRateOptionIndex();

    if (DrawRowLabel("frame_rate", "frame_rate")) {
      ImGui::SetTooltip("%s", Tip("frame_rate"));
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
                         OptionText("rate_", kFrameRateOptions[sel].id, kFrameRateOptions[sel].label),
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
      static thread_local std::string name;
      name = real_idx < 0 ? T("auto") : vulkan_devices_[real_idx].name;
      return name;
    };

    if (DrawRowLabel("vulkan_device", "vulkan_device")) {
      if (const char* tip = Tip("vulkan_device", entry->description.c_str()))
        ImGui::SetTooltip("%s", tip);
    }
    ImGui::SameLine(Px(180.0f));
    ImGui::SetNextItemWidth(Px(160.0f));
    ImGui::PushID("vulkan_device");
    if (ImGui::BeginCombo("##v", label_for(current).c_str())) {
      {
        bool selected = (current < 0);
        ImGui::PushID(-1);
        if (ImGui::Selectable(T("auto"), selected)) {
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

// ---------------------------------------------------------------------------
// Render resolution, as a percentage
// ---------------------------------------------------------------------------
//
// Backed by the native renderer's `render_scale`.

// Below 30% the resolve rectangle's rounding shows on the EDRAM band edges.
constexpr int kRenderScaleMinPercent = 30;
constexpr int kRenderScaleMaxPercent = 200;
constexpr int kRenderScaleStepPercent = 10;

int RenderScaleOptionCount() {
  return (kRenderScaleMaxPercent - kRenderScaleMinPercent) / kRenderScaleStepPercent + 1;
}

int RenderScaleOptionPercent(int index) {
  if (index < 0 || index >= RenderScaleOptionCount()) {
    return 100;
  }
  return kRenderScaleMinPercent + index * kRenderScaleStepPercent;
}

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
  // Zero is the cvar's "no opinion", which renders at the window's own size.
  const auto* entry = rex::cvar::GetFlagInfo("render_scale");
  const double current = entry ? std::atof(entry->getter().c_str()) : 0.0;
  const int percent =
      static_cast<int>(std::lround((current > 0.0 ? current : 1.0) * 100.0));
  return std::clamp(percent, kRenderScaleMinPercent, kRenderScaleMaxPercent);
}

void SetRenderScalePercent(int percent) {
  percent = std::clamp(percent, kRenderScaleMinPercent, kRenderScaleMaxPercent);
  rex::cvar::SetFlagByName("render_scale", std::to_string(percent / 100.0),
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

namespace {
constexpr std::array kDisplayGammaSteps = {0.8, 0.9, 1.0, 1.1, 1.2, 1.3, 1.4};
constexpr std::array<const char*, 7> kDisplayGammaLabels = {"0.8", "0.9", "1.0", "1.1",
                                                            "1.2", "1.3", "1.4"};
}  // namespace

int DisplayGammaOptionCount() {
  return static_cast<int>(kDisplayGammaSteps.size());
}

const char* DisplayGammaOptionLabel(int index) {
  if (index < 0 || index >= DisplayGammaOptionCount()) {
    return nullptr;
  }
  return kDisplayGammaLabels[index];
}

float DisplayGamma() {
  return static_cast<float>(std::clamp(REXCVAR_GET(display_gamma), 0.5, 2.0));
}

int DisplayGammaOptionIndex() {
  const double cur = REXCVAR_GET(display_gamma);
  int best = 0;
  for (int i = 1; i < DisplayGammaOptionCount(); ++i) {
    if (std::abs(kDisplayGammaSteps[i] - cur) < std::abs(kDisplayGammaSteps[best] - cur)) {
      best = i;
    }
  }
  return best;
}

void SetDisplayGammaOption(int index) {
  if (index < 0 || index >= DisplayGammaOptionCount()) {
    return;
  }
  rex::cvar::SetFlagByName("display_gamma", std::to_string(kDisplayGammaSteps[index]),
                           /*persist=*/true);
  SaveUserSettings();
}

int MasterVolumePercent() {
  return VolumePercentFromAmplitude(std::atof(rex::cvar::GetFlagByName("audio_volume").c_str()));
}

void SetMasterVolumePercent(int percent, bool save) {
  rex::cvar::SetFlagByName("audio_volume",
                           std::to_string(VolumeAmplitudeFromPercent(std::clamp(percent, 0, 100))),
                           /*persist=*/true);
  if (save)
    SaveUserSettings();
}

namespace {
std::span<const double> MultiplierSteps(const char* cvar) {
  const std::string_view name = cvar ? cvar : "";
  if (name == "benched_exp_multiplier" || name == "incapacitated_exp_multiplier")
    return kExpShareSteps;
  if (name == "enemy_hp_multiplier")
    return kHpMultiplierSteps;
  if (name == "enemy_exp_multiplier" || name == "enemy_gold_multiplier" ||
      name == "photo_price_multiplier" || name == "enemy_damage_multiplier")
    return kRewardMultiplierSteps;
  if (name == "party_tactical_time_multiplier")
    return kTacticalTimeSteps;
  if (name == "party_action_time_multiplier")
    return kActionTimeSteps;
  // The overlay leaves the "no limit" step off the enemy gauge.
  if (name == "enemy_action_time_multiplier")
    return std::span(kActionTimeSteps).first(kActionTimeSteps.size() - 1);
  return {};
}
}  // namespace

int MultiplierOptionCount(const char* cvar) {
  return static_cast<int>(MultiplierSteps(cvar).size());
}

double MultiplierOptionValue(const char* cvar, int index) {
  const auto steps = MultiplierSteps(cvar);
  return index >= 0 && index < static_cast<int>(steps.size()) ? steps[index] : 0.0;
}

// Nearest step, as the overlay's slider reads a hand edited value.
int MultiplierOptionIndex(const char* cvar) {
  const auto steps = MultiplierSteps(cvar);
  const double value = std::atof(rex::cvar::GetFlagByName(cvar).c_str());
  int best = 0;
  for (int i = 1; i < static_cast<int>(steps.size()); ++i) {
    if (std::abs(steps[i] - value) < std::abs(steps[best] - value))
      best = i;
  }
  return best;
}

void SetMultiplierOption(const char* cvar, int index) {
  const auto steps = MultiplierSteps(cvar);
  if (index < 0 || index >= static_cast<int>(steps.size()))
    return;
  char text[16];
  std::snprintf(text, sizeof(text), "%g", steps[index]);
  rex::cvar::SetFlagByName(cvar, text, /*persist=*/true);
  SaveUserSettings();
}

void ApplySettingDefaults() {
  for (const auto& d : kGameDefaults) {
    // SetDefaultValue also sets the value, which would undo a command line
    // flag parsed before this runs.
    const auto* entry = rex::cvar::GetFlagInfo(d.cvar);
    const bool keep = entry && entry->source != rex::cvar::Source::kDefault;
    const std::string value = keep ? entry->getter() : std::string();
    rex::cvar::SetDefaultValue(d.cvar, d.value);
    if (keep)
      entry->setter(value);
  }
}

void InitSettingsCaches() {
  // Latch the boot language before anything can change it (see
  // ApplyBootLanguageDonorSlot). The voice selection is resolved here too, now that
  // mods have registered their voice languages.
  BootUserLanguageIndex();
  g_active_voice.store(VoiceLanguageIndex());
#if REX_HAS_VULKAN
#if defined(__ANDROID__)
  // Would load the stock driver next to the custom one adrenotools brings in.
  if (!CustomVulkanDriverSelected())
#endif
    g_vulkan_devices_cache = rex::ui::vulkan::EnumerateDevices();
#endif
}

void BindWindowTarget(rex::ui::Window* window, std::filesystem::path user_settings_path) {
  g_window = window;
  g_user_settings_path = std::move(user_settings_path);
  static bool registered = false;
  if (!registered) {
    registered = true;
    rex::cvar::RegisterChangeCallback("fullscreen", [](std::string_view, std::string_view) {
      SaveUserSettings();
    });
  }
}

void BindSettingsTargets(rex::ui::Window* window,
                         std::filesystem::path user_settings_path) {
  BindWindowTarget(window, std::move(user_settings_path));
  LaunchGpuBackend();
}

bool IsCvarPendingRestart(const char* name) {
  return name && CvarPendingRestart(name);
}

bool AnyCvarPendingRestart() { return AnyKnownPendingRestart(); }

bool RestartNow() {
  if (!g_window) {
    return false;
  }
#if REX_PLATFORM_ANDROID
  // Replaces the process itself, so there is no window left to close here.
  return RestartAndroidApp();
#else
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
#endif
}

bool RestartIntoStartScreen() {
  const auto flag = StartScreenFlagPath(g_user_settings_path);
  RequestStartScreenOnNextLaunch(flag, true);
  if (RestartNow()) {
    return true;
  }
  RequestStartScreenOnNextLaunch(flag, false);
  return false;
}

bool QuitNow() {
  if (!g_window) {
    return false;
  }
  rex::ui::Window* window = g_window;
  window->app_context().CallInUIThread([window] { window->RequestClose(); });
  return true;
}

bool CallInWindowThread(std::function<void()> work) {
  if (!g_window) {
    return false;
  }
  return g_window->app_context().CallInUIThread(std::move(work));
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

  // Outside kBasicCvarNames, so Reset All leaves the window size alone.
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
  // pending restart -- but going through SetFlagByName rather than
  // entry->setter directly still matters: it's what runs
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
      "settings.ui_string", [](const rex::system::ModRegistry::EventPayload& payload) {
        const std::string_view kv(reinterpret_cast<const char*>(payload.bytes.data()),
                                  payload.bytes.size());
        const size_t dot = kv.find('.');
        const size_t eq = kv.find('=');
        if (dot == std::string_view::npos || eq == std::string_view::npos || dot > eq) {
          REXLOG_WARN("[settings] ignoring a settings.ui_string payload that is not "
                      "'section.key=value'");
          return;
        }
        AddUiString(UiLanguageOfXLanguage(uint32_t(payload.u64)), kv.substr(0, dot),
                    kv.substr(dot + 1, eq - dot - 1), kv.substr(eq + 1));
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

bool UserLanguageAvailable(int index) {
  const auto options = GetLanguageOptions();
  if (index < 0 || index >= static_cast<int>(options.size()))
    return false;
  if (index >= static_cast<int>(kBuiltinLanguages.size()))
    return true;
  // Text a mod language patched into a borrowed block is that language's.
  const std::string_view slot = options[index].btx_slot;
  const bool borrowed = std::any_of(g_mod_languages.begin(), g_mod_languages.end(),
                                    [&](const ModLanguage& mod) { return mod.btx_slot == slot; });
  return BtxLanguageAvailable(options[index].btx_slot, !borrowed);
}

bool WesternTextAvailable() {
  const auto options = GetLanguageOptions();
  for (const int i : VisibleUserLanguages())
    if (std::string_view(options[i].btx_slot) != "JPN ")
      return true;
  return false;
}

std::vector<int> VisibleUserLanguages() {
  std::vector<int> visible;
  for (int i = 0; i < UserLanguageCount(); ++i)
    if (UserLanguageAvailable(i))
      visible.push_back(i);
  if (visible.empty())
    visible.push_back(0);
  return visible;
}

void ApplyUnavailableLanguageFallback() {
  const int index = BootUserLanguageIndex();
  if (UserLanguageAvailable(index))
    return;
  // Same setter route and bookkeeping as the donor slot, for the same reasons:
  // no pending restart, nothing persisted, the menus keep the player's choice.
  const auto options = GetLanguageOptions();
  const int fallback = VisibleUserLanguages().front();
  auto* entry = rex::cvar::GetFlagInfo("user_language");
  if (!entry || !entry->setter || !entry->setter(options[fallback].id))
    return;
  g_language_donor_applied = true;
  REXLOG_INFO("[settings] {} has no text in this release; booting the guest in {}",
              options[index].label, options[fallback].label);
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
      // A choice this release has no text for runs in the first language it
      // has (see ApplyUnavailableLanguageFallback), so that is what is selected.
      return UserLanguageAvailable(i) ? i : VisibleUserLanguages().front();
    }
  }
  // Unrecognised, which is the normal state after a mod that added a language
  // is disabled with its id still saved: fall back to the first entry rather
  // than clamping to the last, or the player ends up in a language they never
  // chose.
  return VisibleUserLanguages().front();
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

namespace {

void ApplyUserLanguage(int index) {
  const auto options = GetLanguageOptions();
  // From here on the live cvar is what the player picked, donor override or
  // not.
  g_language_selection_changed = true;
  // entry->setter, not SetFlagByName: the SDK declares user_language
  // kRequiresRestart, but here it applies live, so it must not be marked
  // pending. A mod language writes its donor's block.
  if (auto* entry = rex::cvar::GetFlagInfo("user_language"); entry && entry->setter)
    entry->setter(options[index].id);
  SaveUserSettings();
  InvalidateUiTextLanguage();
  WriteGuestTextLanguage(options[index].btx_slot);
}

}  // namespace

void SetUserLanguageSetting(int index) {
  const auto options = GetLanguageOptions();
  if (index < 0 || index >= static_cast<int>(options.size())) {
    return;
  }
  ApplyUserLanguage(index);
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
  LaunchGpuBackend();
  return std::make_unique<CuratedSettingsDialog>(drawer, window, std::move(user_settings_path),
                                                 std::move(app_config_path), input_system);
}

}  // namespace eternalsonata
