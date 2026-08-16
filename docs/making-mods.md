# Making mods

NocturneRecomp mods are folders under `mods/`, layered over the game's data
and, optionally, shipping native code. Two kinds of content can go in a mod,
and a single mod can mix both:

- **Asset replacement**: swap game files, textures, or shaders by mirroring
  the game's own directory layout.
- **Code**: a native DLL that hooks into the app lifecycle (register ImGui
  overlays, keybinds, read guest memory, etc.), via the SDK's mod-plugin ABI.

Mods are enabled in priority order by the `[[mods]]` list in
`mods/mods.toml` (each entry an `id`/`enabled` pair); earlier entries win on
conflicting files.

Mod **source** and build tooling live in a separate repo,
[birabittoh/NocturneRecomp-Mods](https://github.com/birabittoh/NocturneRecomp-Mods)
(`src/<name>/` there), not here -- this repo only ever contains the
built/shipped `mods/<name>/` folders a player enables. Grab prebuilt mods
from that repo's releases (one zip per mod, with binaries for all three
platforms), or clone it to develop a new one; this doc otherwise describes
the mod-plugin ABI and `mod.toml` format that repo's mods target.

## Asset-only mods

An asset mod is just a folder under `mods/<name>/` with any of these
subfolders (all optional; only the ones present are used):

```
mods/<name>/
  game/        overlays the game data partition (game:\ / d:\)
  update/      overlays the update partition
  dlc/<name>/  overlays an installed DLC package
  textures/    texture replacements: <hash16>.dds or .png (flat dir)
  shaders/     shader replacements (DXBC/SPIR-V binaries)
  mod.toml     descriptive metadata (see below)
  icon.png     shown in the F1 mod manager overlay
```

Files under `game/`/`update/`/`dlc/` mirror the exact guest path they
replace, for example `mods/<mod>/game/DATA/sound/bgmusic.wma` replaces
`DATA/sound/bgmusic.wma`. Texture files are named by a 16-hex-digit content
hash (dump one with `texture_dump_enabled = true` in `nocturnerecomp.toml` to
find the hash for a texture you want to replace).

See the `NocturneRecomp-Mods` repo's `src/` directory for some working examples.

## Code mods

A code mod adds a `code = "<stem>"` key to `mod.toml` and ships a built DLL
at `mods/<name>/code/<stem>.dll`. At startup the SDK loads that DLL through a
versioned C ABI (`rex::system::IModPlugin`) and calls its lifecycle hooks
alongside the game's own overlays.

The project ships two builds: vanilla and title-update (TU), which relocates
the whole image and shifts every guest address, and a single mod DLL has to
work with both. The best way is to avoid hardcoded addresses entirely and
read guest state generically, like `memory_peek` does. When a mod genuinely
needs a specific known address (e.g. poking a particular game setting, like
`ui_color` does), don't hardcode it or re-derive the vanilla/TU split
yourself: look it up by name from the SDK's shared mod registry instead. See
[Library mods and the shared registry](#library-mods-and-the-shared-registry)
below.

### 1. Scaffold the mod under `NocturneRecomp-Mods`'s `src/`

Clone [birabittoh/NocturneRecomp-Mods](https://github.com/birabittoh/NocturneRecomp-Mods);
mod **source** lives in `src/<name>/` there, separate from the built/shipped
`mods/<name>/` folder (which only exists locally after a build, or here in
this repo once you've copied a built mod over). Copy an existing mod as a
template: `src/sample_overlay/` is the minimal one:

```
src/sample_overlay/
  CMakeLists.txt
  mod_main.cpp
  mod.toml
  icon.png
```

`CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.25)
project(sample_overlay LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../common/mod_cmake/rexmod.cmake)

rexmod_add_plugin(sample_overlay
    mod_main.cpp
)
```

`rexmod_add_plugin` (from `src/common/mod_cmake/rexmod.cmake`) builds a
shared library, sets C++23, and links `rex::runtime`, the same shared SDK
runtime the game exe links, so your mod shares its ImGui drawer, keybind
registry, and kernel state rather than getting its own copy.

`mod.toml`:

```toml
manifest_version = 1
name = "Sample Overlay"
version = "1.0.0"
description = "Minimal code-mod template: a keybind (F9) and a tiny ImGui overlay."
code = "sample_overlay"
platform = ""
```

`code` must match the CMake target name (and therefore the built DLL's stem).
Everything else is display metadata shown in the F1 mod manager overlay.

`platform` is *written by* `NocturneRecomp-Mods`'s `scripts/make_mods.py`,
not read by it; leave it empty in a fresh mod.toml. After a successful build
it's (re)set to a comma-separated list of whichever platform(s)
`mods/<name>/code/` currently ships a binary for (e.g. `"windows-x64"` after
a `--target windows-x64`-only build, `"windows-x64,linux-x64,linux-arm64"`
once all three have been built into the same tree). It's purely a record of
what's actually on disk, not something you set by hand.

### 2. Implement the plugin ABI

`mod_main.cpp` exports two `extern "C"` functions and returns an
`IModPlugin` subclass:

```cpp
#include <rex/system/mod_plugin.h>

class MyMod : public rex::system::IModPlugin {
 public:
  // Called once, right after the ImGui drawer/overlay stack exists.
  // Register overlays/keybinds here.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}

  // Called once KernelState is fully live (guest module about to launch).
  // Use this for anything needing kernel apps/memory (e.g. filesystem scans).
  void OnModuleLaunched() override {}

  // Called before the host shuts down. Release resources here.
  void OnShutdown() override {}
};

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t rex_mod_abi_version(void) {
  return rex::system::kModPluginAbiVersion;
}

extern "C" REX_MOD_PLUGIN_EXPORT rex::system::IModPlugin* rex_mod_create(
    uint32_t abi_version, const rex::system::ModHostContext* ctx) {
  if (abi_version != rex::system::kModPluginAbiVersion || !ctx) {
    return nullptr;
  }
  return new MyMod();
}
```

All three `IModPlugin` overrides are optional (no-op by default); implement
only what you use. `ModHostContext` (passed to `rex_mod_create`) gives you
`runtime`, `app_context`, `window`, and `input_system` pointers, plus
`mod_root`/`mod_name` for loading your own bundled assets. Everything else,
including registering an overlay, binding a key, and reading guest memory, goes through
the same public SDK headers the base game uses (`rex/ui/imgui_dialog.h`,
`rex/ui/keybinds.h`, `rex/system/xmemory.h`, etc.).

The SDK never unloads a mod's DLL once loaded (guest threads may still be
running plugin code at shutdown), so don't rely on static destructors
running at process exit; use `OnShutdown()` instead.

### Example mods to copy from

- **`src/sample_overlay/`**: smallest possible template: one keybind
  (F9), one ImGui window. Start here for a new mod.
- **`src/memory_peek/`**: reads guest memory via
  `runtime->memory()->TranslateVirtual()` for a user-entered address (F10).
  A good reference for anything that inspects live guest state generically
  (no hardcoded addresses).
- **`src/music_player/`**: a full-featured example: owns a persistent
  singleton (`GetAudioPlayer()`), binds in `OnCreateDialogs`, and uses
  `OnModuleLaunched()` to scan the filesystem once KernelState exists.
- **`src/game_symbols/`**: a *library mod* with no UI of its own;
  publishes reverse-engineered guest addresses into the shared mod registry
  for other mods to depend on. See
  [Library mods and the shared registry](#library-mods-and-the-shared-registry).
- **`src/ui_color/`**: consumes `game_symbols`'s published address
  (`requires = "game_symbols >= 1.0.0"` in its `mod.toml`) instead of
  hardcoding or re-deriving it.
- **`src/event_ping/`** and **`src/event_pong/`**: a
  producer/consumer pair over the shared registry's event bus rather than
  addresses. `event_ping` has no UI: it uses `RegisterTick` to publish a
  `"sample.ping"` event once a second. `event_pong` (F11) declares
  `requires = "event_ping"`, subscribes to that event, and shows the last
  ping in an overlay -- it also republishes a couple of counters to
  `src/blackboard` with no `requires` on it at all, showing that
  Publish/Subscribe coupling can be looser than the `RegisterAddress`
  pattern.
- **`src/blackboard/`**: a shared key/value store (F12) any mod can
  write to purely by publishing `"blackboard.set"`/`"blackboard.delete"`/
  `"blackboard.clear"` events (bytes = `"key=value"` or `"key"`) -- no header
  or linked symbol needed, so even a binary-only third-party mod can
  participate.
- **`src/bus_inspector/`** (F5): subscribes to the events above and
  logs every one it sees. Since it piles a second/third subscriber onto
  event names `event_pong` and `blackboard` already subscribe to, it
  demonstrates that `Subscribe` supports fan-out to multiple listeners
  rather than last-one-wins.
- **`src/xex_patch_potion/`** and **`src/xex_patch_red_rust/`**:
  two independent, no-UI mods that each patch a different item
  name/description baked into `default.xex`'s static data directly in
  guest memory at startup, and coexist with no conflict. See
  [Patching static game text/data](#patching-static-game-textdata) below,
  and `src/common/include/rexmod/text_patch.h` for the shared helper
  both of them call instead of duplicating the read-only-page-unlock/
  zero-fill logic.

### 3. Build it

```
python scripts/make_mods.py
```

run from `NocturneRecomp-Mods`, not this repo. It configures and builds
every `src/<name>/` project and assembles the result into
`mods/<name>/code/<platform>/` (`<name>.dll` / `lib<name>.so`, plus
`mod.toml` and `icon.png` at the mod root). See that repo's README and the
script's own `--help`/docstring for flags (`--mod`, `--target
{windows-x64,linux-x64,linux-arm64}`, `--package`, `--sdk-dir`) and
cross-build details. Once built, copy `mods/<name>/` into this repo's
`mods/` as-is: `LoadModPlugin` checks `code/<platform>/<stem>...` (matching
the running host) before falling back to a flat `code/<stem>...`, so a mod
folder carrying every platform side by side (as a multi-platform
distribution from that repo's releases does) loads correctly with no
flattening step needed. A locally-built, single-platform mod's flat
`code/<stem>...` still works too.

Prebuilt mods (all three platforms, already zipped one-per-mod) are
attached to that repo's [releases](https://github.com/birabittoh/NocturneRecomp-Mods/releases)
if you just want to install one rather than build it yourself.

### 4. Enable it

Add a `[[mods]]` entry for the mod's folder name to `mods/mods.toml`. Order
matters: earlier entries take priority when multiple mods touch the same
file:

```toml
[[mods]]
enabled = true
id = 'music_player'

[[mods]]
enabled = true
id = 'sample_overlay'

[[mods]]
enabled = true
id = 'memory_peek'
```

Then run the game (`./nocturnerecomp.exe`) and press **F1** to open the mod
manager overlay; it lists every enabled mod, in load order, with its icon
and a `[code]` badge on mods that loaded a DLL. Check `logs/` if a code mod
doesn't show up loaded; the loader logs the exact reason (missing DLL, ABI
mismatch, missing exports) at startup.

`mod.toml` also supports three optional dependency fields, each a
comma-separated list of other mods' folder names:

```toml
requires   = "game_symbols"     # must be enabled AND loaded before this mod,
                                 # or the game fails to start
load_after = "some_other_mod"   # soft ordering hint: only warns if violated
conflicts  = "legacy_ui_hack"   # hard error if both this mod and any listed
                                 # one are enabled, regardless of order
```

A missing or misordered `requires` (or a violated `conflicts`) fails the game
at startup with a message naming the mods involved and the fix, instead of
silently loading in a broken state -- if the mod also uses the shared
registry (below) to depend on the other mod's data, this is what actually
guarantees that data exists by the time it's looked up. `load_after` only
warns; it doesn't gate startup.

Each `requires` entry can also pin a minimum version of the mod it names:

```toml
requires = "game_symbols >= 1.0.0"
```

The version is checked against the named mod's own `version` key (dotted
numeric, e.g. `1.0.0`; missing trailing components count as `0`, so `1.0` ==
`1.0.0`). This is a **hard failure** at Setup() only if the enabled
`game_symbols` is actually older. If `game_symbols` has no `version` key at
all (or the constraint itself isn't a valid dotted version), the check can't
be verified either way, so it's accepted with a warning rather than blocking
startup -- this keeps mods and dependencies that predate this feature
working unchanged. A bare `requires = "game_symbols"` (no `>=`) stays
unconstrained.

A mod can similarly require a minimum version of NocturneRecomp itself via
`game_version`, independent of any other mod:

```toml
game_version = "1.0.0"   # or, equivalently: game_version = ">= 1.0.0"
```

This is checked against the build's own version (`nocturnerecomp_app.h`'s
`OnPreSetup` sets it from `src/version.generated.h`, derived from the
nearest `vX.Y.Z` git tag at CMake configure time -- see `CMakeLists.txt`)
and is likewise a hard failure only when the build is actually older; if no
tag is reachable, the version falls back to `0.0.0` and the check is
accepted with a warning instead.

## Library mods and the shared registry

`rex::system::ModRegistry`, reached via `runtime->mod_registry()` from any
mod that holds a `Runtime*`, is a small registry for sharing reverse-engineered
addresses (and generic events) between mods, so that work doesn't have to be
redone, or copy-pasted, by every mod that needs it.

```cpp
// Producer: registers a name once, resolved to a vanilla or TU address
// depending on the running image (no is_patched() check needed by callers).
runtime->mod_registry()->RegisterAddress("ui.accent_color", kVanillaAddr, kTuAddr);

// Consumer: looks the address up by name instead of hardcoding it.
if (auto addr = runtime->mod_registry()->FindAddress("ui.accent_color")) {
  // use *addr
}
```

A **library mod** is a mod that only does this: no UI, no `code` consumers of
its own, just registration calls in `OnCreateDialogs`. `src/game_symbols/`
is exactly that: it registers `"ui.accent_color"` (the same struct
`accent_color.cpp` reads) for other mods to depend on. `src/ui_color/`
consumes it:

```toml
# src/ui_color/mod.toml
code = "ui_color"
requires = "game_symbols >= 1.0.0"
```

```cpp
// src/ui_color/mod_main.cpp, OnModuleLaunched (lazy lookup, not eager
// in OnCreateDialogs -- see "Ordering" below)
if (auto addr = runtime_->mod_registry()->FindAddress("ui.accent_color")) {
  addr_ = *addr;
}
```

`requires = "game_symbols"` (see above) is what makes this safe: the SDK
guarantees `game_symbols` is enabled and ordered before `ui_color`, so the
lookup can't silently return nothing because of a config mistake.

`ModRegistry` also has `Subscribe`/`Publish` for generic events (a payload of
a `uint64_t`, a `double`, and a byte span valid only for the duration of the
`Publish` call), and `RegisterTick`/`DispatchTick` for a callback fired once
per guest frame on GPU swap, useful for anything that needs to react every
frame rather than just at startup or launch. Ticks run on the
command-processor thread, not the render/UI thread.

**Ordering**: producers register in `OnCreateDialogs` (dispatched in
`enabled_mods` order, before `OnModuleLaunched`); consumers look up lazily,
on first use, rather than assuming a specific dispatch order themselves.
`requires` is what actually enforces the producer runs first, not the lookup
site.

**Threading**: `DispatchTick` (and therefore anything a tick callback
publishes) runs on the command-processor thread, not the render/UI thread,
and `Publish` invokes every subscriber synchronously on whatever thread
called it. So a `Subscribe` callback must never touch ImGui directly --
instead copy the payload (including the `bytes` span, which is only valid
for the duration of that one `Publish` call) into a mutex-guarded member,
and render from that snapshot in `OnDraw`, which does run on the UI thread.
See `src/event_pong/`, `src/blackboard/`, and
`src/bus_inspector/` for the pattern.

**Adding a Language dropdown entry**: the curated Settings overlay's
Language row (`src/settings.cpp`) is app code, not SDK code, so it isn't
something a mod can reach through `RegisterAddress`/`FindAddress`. Instead
NocturneRecomp itself subscribes, from `OnPostLoadXexImage()` (after
`Runtime` exists but before any mod's `OnCreateDialogs` runs), to a
`"settings.language_option"` event on the shared registry. A mod publishes
one entry per language it wants to add, from its own `OnCreateDialogs`:

```cpp
void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
  rex::system::ModRegistry::EventPayload payload;
  payload.u64 = 9;  // XLanguage id. e.g. 9 --> Portuguese
  const char* label = "Portuguese";
  payload.bytes = {reinterpret_cast<const uint8_t*>(label), std::strlen(label)};
  runtime_->mod_registry()->Publish("settings.language_option", payload);
}
```

The id is whatever the game itself understands for `user_language` (an
`XLanguage` value); publishing an id doesn't make the game render that
language, it only adds the option to the dropdown so a mod that *does* add
the actual translated text/assets can let the player select it. A duplicate
id (already built in, or already published by an earlier-loaded mod) is
dropped with a WARN log, same first-wins rule as `RegisterAddress`.

**Keybind collisions**: `rex::ui::RegisterBind` auto-resolves collisions
rather than silently shadowing one bind. If two mods both default to the
same key, the later-loaded one (lower `enabled_mods` priority) is moved to
the next free key from a small pool (F5-F12, then F13-F24 as overflow),
logged at WARN, and shown with a "moved" badge in the F1 mod manager. If the
pool is exhausted the bind is left on its requested key but flagged as an
unresolved conflict rather than silently colliding. A key the user has
explicitly set (via config or the F1 overlay's click-to-rebind control) is
never auto-moved. Give every mod's bind both a unique name (it doubles as
the backing CVar name) and, by convention, a unique default key -- the
auto-reassignment is a safety net, not a reason to stop picking distinct
defaults.

The F1 mod manager overlay also lists, per mod, the cvars it defines or
overrides (old -> new value) and flags cvars that two mods have set to
different values, so a silent `SetFlagByName` clash is at least visible --
this is detection only; the underlying cvar is still last-write-wins.

**Overlay visibility, the gamepad overlay menu, and window titles**:
`RegisterBind` takes two optional trailing parameters: `is_visible` -- a
`std::function<bool()>` returning whether the thing this bind toggles is
currently shown -- and `window_title`, the exact string your overlay passes to
`ImGui::Begin` (including any `##id` suffix):

```cpp
rex::ui::RegisterBind(
    "bind_sample_overlay", "F9", "Toggle sample overlay",
    [this] { visible_ = !visible_; },
    [this] { return visible_; },
    "Sample##overlay");
```

Passing `is_visible` costs nothing extra and makes your overlay show up, with
its live shown/hidden state, in two places: the F1 mod manager's per-mod
keybind list, and the gamepad-triggered overlay menu (default **Y**, with an
**Insert** keyboard fallback for controller-less testing; both are ordinary
rebindable binds) that lists every overlay -- vanilla and mod -- with
`is_visible` set, grouped by owner, selectable to toggle without touching a
keyboard.

Passing `window_title` as well makes your overlay fully gamepad-navigable
inside the SDK's two input modes (**Gameplay**, where the pad drives the game
as normal, and **UI**, where it drives the overlays -- toggled by the guide
button, with a **Home** keyboard fallback since guide is frequently
intercepted by Steam/the OS before it reaches the game). In UI mode, one
overlay is "active": left stick/D-pad and A drive ImGui's built-in gamepad nav
inside it, B closes it, Y opens/activates the overlay menu, X cycles the
active overlay among all currently-shown ones, right stick moves its window,
and left-trigger + right stick resizes it. Without `window_title` your overlay
can still be *toggled* from the overlay menu, it just can't be focused, moved,
or resized by the gamepad controller (`rex::ui::GamepadUiController`, see
`gamepad_ui.h`) the way the six base-app overlays are. A bind whose effective
key is a gamepad button name (set via the settings overlay, the mod manager's
rebind control, or by editing `nocturnerecomp.toml` directly) is dispatched on
press while in Gameplay mode only -- in UI mode the gamepad controller owns
the pad for navigation instead, so gamepad-keyed binds don't fire there.

**This requires rebuilding every mod, not just ones adopting `is_visible`/
`window_title`.** `RegisterBind` is a regular (mangled, not `extern "C"`)
exported symbol in `rexruntime(rd).dll`; adding a parameter -- even an
optional one with a default value -- changes that mangled name. Old code
compiles against the new header unchanged (the default fills in the missing
argument), but an **already-built** mod DLL that was never recompiled still
imports the old symbol signature, which no longer exists in the new DLL's
export table, and fails to load (not a graceful skip -- an OS-level
missing-entry-point failure). Rebuild every mod (`make_mods.py`, or the direct
`cmake --build` invocation if you're bypassing it for a config match -- see
"Both builds, one DLL" below) any time you update the SDK, whether or not
you're using anything new it added.

## Adding rows to the game's native Options screen

A mod can add its own rows to the in-game Options screen -- the real one, drawn
by the game's own text renderer and navigated with the game's own cursor, not
an ImGui overlay. There is only one Options screen; a row added this way shows
up both from the main menu and from the in-game status screen.

Options is two pages, paged with LB/RB. Page 1 (`ETERNALSONATA_PAGE_GAME`) is
the Options screen proper, where the game's own Subtitles and Voice rows live
alongside this project's Text row; page 2 (`ETERNALSONATA_PAGE_GRAPHICS`) is the
button-configuration screen, which the project uses for graphics settings --
Resolution and Frame Rate.

**A mod row lands on page 2 by default.** Page 1 is reserved for the game's own
settings and is expected to fill up with them.
`EternalSonataSetOptionRowPage(row, ETERNALSONATA_PAGE_GAME)` moves a row over
if you have a reason to.

The entry points are exported from the host executable, not from the SDK, so
you resolve them at runtime rather than linking against anything. Copy
`src/eternalsonata_options_api.h` from this repo into your mod for the
signatures and the full contract, then:

```cpp
#include "eternalsonata_options_api.h"

static const char* kValues[] = {"Off", "On"};
static int  MyGet(void* user)            { return g_my_setting ? 1 : 0; }
static void MySet(int index, void* user) { g_my_setting = (index == 1); }

void MyMod::OnModuleLaunched() {
  auto reg = reinterpret_cast<EternalSonataRegisterOptionRowFn>(
      GetProcAddress(GetModuleHandle(nullptr), "EternalSonataRegisterOptionRow"));
  if (!reg) {
    return;  // older host build -- degrade gracefully, don't fail to load
  }
  int row = reg("My Setting", kValues, 2, &MyGet, &MySet, nullptr);

  auto set_label = reinterpret_cast<EternalSonataSetOptionRowLabelFn>(
      GetProcAddress(GetModuleHandle(nullptr), "EternalSonataSetOptionRowLabel"));
  if (set_label && row >= 0) {
    set_label(row, ETERNALSONATA_LANG_IT, "Mia impostazione");
  }

  // Optional: put the row on the game page instead of the graphics one.
  auto set_page = reinterpret_cast<EternalSonataSetOptionRowPageFn>(
      GetProcAddress(GetModuleHandle(nullptr), "EternalSonataSetOptionRowPage"));
  if (set_page && row >= 0) {
    set_page(row, ETERNALSONATA_PAGE_GAME);
  }
}
```

Things worth knowing before you use it:

- **Always null-check the `GetProcAddress` result.** A mod built against a
  newer host has to keep loading on an older one. Call
  `EternalSonataOptionsAbiVersion()` if you need to branch on host capability.
- **Register from `OnModuleLaunched()`.** Rows registered later still appear,
  but only the next time the screen is built -- not on a screen already open.
- **Room is finite, and it is per page: 8 rows on page 1, 7 on page 2.** That
  is the game's limit, not an arbitrary one: a selectable group's item array is
  pre-allocated with 10 slots, of which page 1's stock Subtitles and Voice rows
  take 2 and page 2's three button rows take 3. Registration past that is
  refused and logged rather than corrupting the menu. The project's own rows
  take two of page 2's seven (Resolution, Frame Rate) and one of page 1's eight
  (Text).
- **Values are drawn side by side on one line, so keep them short.** They share
  the width between the row's value column and the end of the row rule, about
  630px. Columns are evenly spaced when that fits -- 200px each for two or
  three values, ~157px for four, ~126px for five -- and fall back to
  content-aware placement when the longest value would not fit an even column,
  giving each value the room it needs and sharing the remainder as equal gaps.
  Either way the budget is finite: a row with many values needs abbreviated
  ones, which is why the built-in Text row draws "EN"/"DE"/"FR"/"ES"/"IT"
  rather than language names. Labels have their own budget of about 13
  characters before they reach the value column.
- **Text is single-byte CP1252/Latin-1, not UTF-8.** The game's font draws one
  glyph per byte, so `"é"` written as UTF-8 renders as two garbled glyphs.
  Write `"\xE9"`.
- **Labels are localisable, values are not.** One label covers every language;
  `EternalSonataSetOptionRowLabel` overrides individual ones. Values (FPS
  figures, resolution names) are conventionally left untranslated, which is how
  the built-in rows behave too.
- **`get` is polled, `set` runs on the guest menu thread.** Neither may block,
  and `set` must not re-enter this API.

The built-in Resolution and Frame Rate rows go through this exact registration
path -- there is no privileged internal route -- so anything that works for
them works for a mod row. See `src/eternalsonata_options.cpp` for the
underlying display-list and selection work, and `docs/debug-hooks.md` §14 for
the reverse engineering it rests on.

## Reading and changing the party

A mod can ask who is in the party, read and write their stats, add and remove
members, reorder them and rename them, without knowing a single guest address.
Copy
`src/eternalsonata_party_api.h` from this repo into your mod for the
signatures and the full contract, then resolve the entry points out of the host
executable exactly as with the Options API:

```cpp
#include "eternalsonata_party_api.h"

auto add = reinterpret_cast<EternalSonataAddCharacterToPartyFn>(
    GetProcAddress(GetModuleHandle(nullptr), "EternalSonataAddCharacterToParty"));
if (add) {
  add(ETERNALSONATA_CHAR_VIOLA);
}
```

Things worth knowing before you use it:

- **Always null-check the `GetProcAddress` result**, and call
  `EternalSonataPartyAbiVersion()` if you need to branch on host capability. A
  mod built against a newer host has to keep loading on an older one.
- **Reads answer immediately; writes are queued.** Anything that has to run
  guest code (add, remove, reorder, stat writes) is queued onto the guest main
  thread and returns `ETERNALSONATA_PARTY_QUEUED`, because a guest call from
  the ImGui draw thread crashes the game. The rejections that can be decided
  without running guest code -- unknown character, no save loaded, battle in
  progress, already in the party -- are still returned immediately. Poll
  `EternalSonataIsCharacterInParty` to see a queued change land.
- **Party edits are refused during a battle.** The game's own join sequence
  crashes mid-battle; `EternalSonataIsPartyEditable()` is the flag to gate your
  UI on, and every mutation checks it anyway.
- **There are ten characters and two vacant slots.** The port widens every
  per-character table to twelve, but it puts nothing in the two new slots: ids
  11 and 12 have no name, no stats and no model, and the game's own id gates
  stay shut for them until a mod defines one. See "Adding a new character"
  below.
- **Renaming shows up on the menu screens only.** The status, equipment and
  party screens resolve names through a text lookup the host answers; the battle
  HUD reads the game's packed name tables directly and still shows the built-in
  name.
- **Names are single-byte CP1252/Latin-1, not UTF-8**, like every other string
  the game's own font draws.

`docs/party-system.md` documents the guest-side structures the API sits on, if
you need to know what a call actually does.

## Adding a new character

Ids above `ETERNALSONATA_NATIVE_CHARACTER_COUNT` are vacant slots. The host owns
the widened tables behind them and nothing else, so a slot has no content until
a mod claims it:

```cpp
EternalSonataCharacterDefinition definition{};
definition.struct_size = sizeof(definition);
definition.name = "Cadenza";                             // required
definition.template_source = ETERNALSONATA_CHAR_JAZZ;    // stats and growth
definition.model_id = 0;                                 // 0 = pc011.bop etc.

auto define_next = reinterpret_cast<EternalSonataDefineNextCharacterFn>(
    GetProcAddress(GetModuleHandle(nullptr), "EternalSonataDefineNextCharacter"));
const int character = define_next ? define_next(&definition) : -1;
```

Worth knowing:

- **Claim a slot, do not name one.** `EternalSonataDefineNextCharacter` takes
  whichever slot is free and returns its id, so your mod and another that also
  adds a character can both load. `EternalSonataDefineCharacter` names a
  specific one and is for when you have a reason to.
- **Define on every run.** Definitions are host state and are not written to a
  save. `OnModuleLaunched` is the natural place; a save does not have to be
  loaded yet.
- **The template decides stats and growth.** `template_source` picks which
  retail character's entry in the game's own per-character template table your
  character starts from. Optional `stats` (with `apply_stats` set) overrides the
  numbers once, the first time the character joins a party.
- **Ship the battle model.** `sub_821A03D0` loads
  `btldata\player\pc%03d.bop` by character id, so an asset mod supplying
  `game/btldata/player/pc011.bop` is all character 11 needs. Until you have one,
  point `model_id` at an existing character so a battle does not ask for a file
  that is not there.
- **A vacant slot is inert.** Every mutation refuses it with
  `ETERNALSONATA_PARTY_ERR_SLOT_VACANT`, `EternalSonataGetCharacterName` answers
  `""`, and UI that lists the cast should skip anything
  `EternalSonataIsCharacterDefined` says no to.

`../EternalSonataReprise-Mods/src/demo_characters` is a complete worked example,
and `docs/party-system.md` explains what a definition actually does to the
game's tables.

## Patching static game text/data

Item/enemy names, descriptions, and similar flavor text aren't loaded from
a separate asset file the VFS can overlay -- they're baked into
`default.xex`'s static data (`.rdata`), copied into guest memory once at
startup by the same loader that runs the game's code. Byte layout, text
encoding, and the AES/compression format `default.xex` ships in on disk
are documented in `extracted/README.md`.

Two ways to change one of these strings:

**Replace `default.xex` itself** (`mods/<name>/game/default.xex`).
`XexModule::ReadImage` in the SDK doesn't verify a signature on the base
image load, and accepts a plain, unencrypted/uncompressed XEX2 file
(`scripts/re/rebuild_xex_unencrypted.py` builds one). A mod's
`game/default.xex` **replaces the whole file** with no merging: if two
mods each ship one, `enabled_mods` order picks a single winner and the
other mod's edits are gone, whether or not they touched the same bytes.
Use this only when a mod is the sole thing expected to touch game text,
or needs a structural change a same-length string swap can't do.

**Patch guest memory from a code mod instead**: any number of mods can
each own a different address with no conflict, the same way
`src/ui_color` pokes the accent-color struct. Use
`src/common/include/rexmod/text_patch.h`'s `ApplyTextPatch`
(description fields, plain ASCII) or `ApplyNameFieldPatch` (name fields,
which use a "big first letter" font encoding -- see
`extracted/README.md`) from `OnModuleLaunched()`. Requirements:

1. **The guest address must come from scanning a live, running process**
   (`scripts/re/scan_guest_memory.py`), not from offline-decrypting
   `default.xex` and computing a file offset. A title that ships a
   `default.xexp` (a title-update delta patch -- check the startup log
   for `XEX patch applied successfully`) has that patch applied over the
   base image on every launch, which can shift addresses after the
   patched region by an amount offline decryption of the base file alone
   won't account for.
2. **The target field is very likely in read-only memory.** Writing to it
   in-process access-violates (`0xC0000005`) unless the page is unlocked
   first: `runtime->memory()->LookupHeap(addr)->Protect(addr, size,
   kMemoryProtectRead | kMemoryProtectWrite, &old_protect)` before the
   write, then `Protect(addr, size, old_protect)` after --
   `ApplyTextPatch`/`ApplyNameFieldPatch` already do this.
3. **Field length must be measured from what actually follows the string
   in memory**, not assumed from `len(text) + 1`: some fields end in a
   single null before the next field, some have no terminator at all
   (butting directly against the next field or an entry-end marker), and
   the pattern isn't consistent enough to assume without checking the
   specific field being patched.

## Both builds, one DLL

The project ships two builds (vanilla and title-update) with different guest
addresses. A code mod built as described above works with both as long as it
never hardcodes *just one* build's address: either read guest state
generically (like `memory_peek` does), or look the address up from the
shared registry (like `ui_color` does; see
[Library mods and the shared registry](#library-mods-and-the-shared-registry)
above) instead of branching on `is_patched()` itself. All the sample mods
here follow one of those two rules, so `make_mods.py`'s output loads
unchanged into either build.
