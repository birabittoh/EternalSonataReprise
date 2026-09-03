# Eternal Sonata — Debug Menu & Hookable Flags Reverse Engineering

## Table of Contents
1. [Binary Overview](#binary-overview)
2. [Devkit Privilege Gate](#devkit-privilege-gate)
3. [In-Game Debug Console](#in-game-debug-console)
4. [ConsoleSetting](#consolesetting)
5. [Reviving a Console](#reviving-a-console)
6. [Command-Line Argument Parsing](#command-line-argument-parsing)
7. [Dead Debug Artifacts](#dead-debug-artifacts)
8. [Init Callback Tables](#init-callback-tables)
9. [Dev Tool References](#dev-tool-references)
10. [Collision `todo:` Stubs](#collision-todo-stubs)
11. [Implemented Hooks](#implemented-hooks)
12. [Hooking Architecture](#hooking-architecture)
13. [IDA Session Notes](#ida-session-notes)
14. [Options Screen (native settings rows)](#options-screen-native-settings-rows)
15. [Profiling cvars](#profiling-cvars)

---

## 1. Binary Overview

- **Binary**: `assets/default.xex` — Eternal Sonata, Xbox 360
- **Architecture**: PowerPC 32-bit, base `0x82000000`, image size `0x580000`
- **Code segment**: `0x820C0000`–`0x822E7CB4` (size `0x227CB4`)
- **PDB path**: `d:\Tcx\P1\project\P1_eu\Master\P1_EU.pdb`
- **Total functions**: 6,135 (249 named)
- **Total strings**: 29,769
- **IDA session**: `b75ab2d6` (headless, opened 2026-07-26)

---

## 2. Devkit Privilege Gate

### Function: `sub_82254060`

This is the first thing `xstart` (the title entry point) calls after minimal init.
It acts as a kill switch for retail (non-devkit) environments.

### Logic Flow

```
sub_82254060():
  r31 = 0                          // default: pass

  // Check 1: Devkit executable privilege
  r3 = XexCheckExecutablePrivilege(0xA)   // privilege 0xA = devkit
  if r3 == 0 → goto FAIL                   // no devkit privilege

  // Check 2: AV pack type
  r3 = XGetAVPack()
  if r3 == 3 → FAIL   // (VGA?)
  if r3 == 6 → FAIL   // (component?)
  if r3 == 8 → FAIL
  if r3 == 4 → FAIL

  // Check 3: XConfig setting (category 2, key 2)
  ExGetXConfigSetting(2, 2, &out, 4, &out_size)
  if status != 0 → FAIL
  if (out & 0xFF00) != 0x300 → FAIL     // must be build type 3

  // Check 4: XConfig setting (category 3, key 0xA)
  ExGetXConfigSetting(3, 0xA, &out, 4, &out_size)
  if status != 0 → FAIL
  if !(out & 0x800000) && (out & 0x400000) → FAIL

  // Language mapping table at 0x82000698
  r3 = XGetLanguage()
  // maps language → region index via lookup table
  // calls sub_82253FD8 twice (lang → name strings)
  // calls sub_82253F20 (locale finalization)

  r31 = 1   // FAIL: set terminate flag

FAIL:
  r3 = r31
  return
```

### Caller: `xstart` (guest `0x8225424C`)

```asm
  bl  sub_82254060
  cmpwi r3,0
  beq   skip_terminate
  bl    XamLoaderTerminateTitle    // ← kills the process
skip_terminate:
  // ... continues with init ...
```

### Impact

If `sub_82254060` returns 1, `XamLoaderTerminateTitle` is called immediately.
In the recompiled port, the SDK implements this as `TerminateTitle()` which
exits the process. **This is the primary blocker for running the recompiled binary.**

### Implemented Hook

```cpp
// src/eternalsonata_hooks.cpp
REX_EXTERN(__imp__sub_82254060);
REX_HOOK_RAW(sub_82254060) { ctx.r3.u64 = 0; }
```

Overrides the entire function to return 0 (pass). No config entry needed —
`REX_HOOK_RAW` creates a strong symbol that overrides the weak
`DEFINE_REX_FUNC(sub_82254060)` alias in the generated code.

---

## 3. In-Game Debug Console

### Global: `0x8244C140`

A debug console object with its own vtable and command buffer.

### Init Function: `sub_822DFA88`

Initializes the console object at `0x8244C140`. Despite being marked `__noreturn`
by IDA, the function **does return** (confirmed via disassembly — `blr` at `0x822dfb48`).

### Structure

| Offset | Type   | Description |
|--------|--------|-------------|
| +0     | u32    | Vtable pointer (`off_82082DDC`) |
| +4     | u32    | State flags |
| +8     | ...    | 7,245-byte command buffer at `unk_8244C188` |

### Vtable: `off_82082DDC`

| Slot | Address     | Function            | Description |
|------|-------------|---------------------|-------------|
| 0    | `0x821095B8`| `sub_821095B8`      | Destructor: closes handle at +64, resets vtable to base, unregisters from task system |
| 1    | `0x8216C8A8`| `nullsub_1`         | Empty — no-op |
| +8   | string      | `"DrawSync"`        | Name label (not code) |

### State Bytes

| Guest Address | Name               | Init Value | Purpose |
|---------------|--------------------|------------|---------|
| `0x8244DDE0`  | `dword_8244DDE0`   | `-1` (0xFFFFFFFF) | Console mode (-1 = disabled) |
| `0x8244DDE4`  | `dword_8244DDE4`   | `0x80000000` | Flag bit |
| `0x8244DDE8`  | `byte_8244DDE8`    | `1` | Initialized flag |
| `0x8244DDE9`  | `byte_8244DDE9`    | `0` | Dirty/needs-redraw flag |
| `0x8244DDEC`  | `dword_8244DDEC`   | `0` | Unknown |
| `0x8244C164`  | `byte_8244C164`    | `1` | Unknown |

### DORMANT in Retail Build

**Critical finding — the vtable is empty of behaviour.** Raw bytes at
`0x82082DDC` (re-verified 2026-07-29 via `get_bytes`, not via xrefs):

```
82 10 95 b8   -> sub_821095B8   destructor
82 16 c8 a8   -> sub_8216C8A8   nullsub_1 (single blr)
44 72 61 77 53 79 6e 63  "DrawSync"  <- ASCII; the vtable ends here
```

Exactly two slots. `sub_821095B8` resets `*obj` to the base vtable
`off_82082B60`, unlinks via `sub_8212DDA8`, and conditionally frees;
`sub_8216C8A8` is a single `blr`. **There is no render, input, or
command-dispatch method anywhere in the binary**, so no amount of state-byte
patching can make a console appear. Likewise `sub_822E6440` (registered via
`sub_822CCBD8`) is a destructor, NOT a per-frame update.

**This vtable fact is the sole basis for the dormancy verdict, and it is
sufficient on its own.**

> ⚠️ **Do not repeat the earlier "zero reads" argument.** A previous pass
> claimed forward data-flow showed zero reads of `dword_8244DDE0` /
> `byte_8244DDE8` after init. **That claim is unverifiable with this tooling
> and must not be relied on.** Three reasons:
> 1. The xref DB is incomplete for this binary — see the caveats in
>    `script-vm-notes.md`.
> 2. `find_bytes` is not exhaustive: a scan for `7C 08 02 A6` (`mflr r0`,
>    present in nearly all 6,135 functions) returns **2** matches. Its
>    negatives are worthless.
> 3. `sub_822DFA88` addresses the struct **r31-relative**
>    (`stb r11, (byte_8244DDE8 - 0x8244C140)(r31)`), and the object is
>    registered into a named subsystem via `sub_820E64F0(obj, 0xD, "Console")`
>    and `sub_821304E8`. Any consumer reaching it through a pointer would
>    generate no xref on those bytes at all.

The command buffer at `unk_8244C188` is only ever zeroed, in two places:
`sub_822DFA88` (`memset`, 0x1C4D = 7245 bytes) and `sub_820F91A8` (a second
`memset` of the same size). Neither reads it.

### Hook Removed

A `REX_HOOK_RAW(sub_822DFA88)` override used to patch the state bytes after the
original init ran (`dword_8244DDE0 = 0`, `byte_8244DDE9 = 1`) to force the
console active. **Removed** — it had no observable effect, as expected from the
dormancy analysis above: nothing reads those bytes, and the vtable contains no
render or input method to resurrect. Flipping a mode byte cannot bring back a
draw function the linker stripped.

**There is no in-game debug console and no keybinding to open one.** An
on-screen console has to be built host-side — see [Reviving a Console](#reviving-a-console).

---

## 4. ConsoleSetting

### Global: `0x82565850`

A settings console object, separate from the debug console.

### Init Function: `sub_822E5BE8`

Initializes the ConsoleSetting object at `0x82565850`. Also marked `__noreturn`
but likely returns normally (similar pattern to the debug console).

### Structure

| Offset | Type   | Description |
|--------|--------|-------------|
| +0     | u32    | Vtable pointer (`off_820AA048`) |
| +30    | u32    | Sub-object at `0x82565880` with vtable `off_820AA050` |

### Vtable: `off_820AA048`

Contains a shutdown callback `sub_822E71B8` — decrements a counter and
calls `sub_8212DBA8` when it reaches 0, then resets vtable to base.

### DORMANT in Retail Build

Same situation as the debug console — the state is initialized but
the console has no active rendering or input methods in the retail binary.

### Hook Removed

A `REX_HOOK_RAW(sub_822E5BE8)` override used to force the sub-object at
`0x82565880` to vtable `off_820AA050` after init. **Removed** for the same
reason as the debug-console hook — no effect.

---

## 5. Reviving a Console

The game-side machinery is gone, not merely disabled, so a console has to be
built rather than unlocked. Options, roughly in order of practicality:

1. **Host-side overlay via the SDK** — draw an ImGui-style overlay from the
   recompiled host, reading/writing guest memory directly. Check
   `../rexglue-sdk-wiki` for an existing overlay/debug-draw hook first.
2. **Reuse the game's text renderer** — hook a per-frame function and drive the
   dialogue/HUD draw path with custom strings. Renders in-engine, but invasive.
3. **Log to stdout** — `DbgPrint` is imported and the SDK routes it to logging.
   A hook can call it directly with no gating.

---

## 6. Command-Line Argument Parsing

### Gate Global: `0x82426728`

A dword checked by `xstart` after the devkit gate and several init calls.
**On retail, this is always 0** — no code writes to it (single xref from xstart).

### Flow in `xstart`

```c
// After devkit check + init calls:
if ( dword_82426728 )   // always 0 on retail → always skipped
{
    v5 = (char *)sub_82254B18();  // returns 0x101BE (guest address of cmd string)
    // Parse command string into up to 16 tokens (space/tab/quote handling)
    v10 = argarray; v11 = argcount;
}
else
{
    v10 = 0; v11 = 0;
}
sub_82132B28(v11, v10, 0);  // ← arguments are IGNORED (see below)
sub_822CFE10();
```

### CRITICAL: Arguments Are Ignored

`sub_82132B28` is **NOT a CLI dispatch function**. Disassembly confirms it ignores
r3, r4, r5 entirely — it immediately calls `sub_8212DA18()` (task system init)
followed by `sub_82132A08()` (the main game loop). The entire cmdline parsing path
in xstart is dead code.

**The initial midasm hook enabling cmdline parsing was therefore also dead code
and has been removed.**

### `sub_82254B18` — Command String Source

Returns the constant `65966` (`0x101BE`) — a guest memory address where the
command string would be stored. On retail, this memory is uninitialized (empty).

### Retail Default

On retail Xbox 360, `0x82426728` is always 0, so the parser never runs.
Even if it did, the parsed arguments are discarded by `sub_82132B28`.

---

## 7. Dead Debug Artifacts

### Orphaned "rDebug Save" Strings

| Address     | String           | Xrefs |
|-------------|------------------|-------|
| `0x822f4fb4` | `"rDebug Save"` | 0     |
| `0x822f5c5d` | `"rDebug Save"` | 0     |

Both strings are dead — no code references them. They suggest a debug save
feature was removed or disabled before release but the strings were left in.

### DbgPrint Import

`DbgPrint` is imported (`__imp__DbgPrint` at `0x822E7794`) and called in
`xstart` after the command-line dispatch. In the recompiled port, the SDK
implements this as a logging output. It is functional but gated behind
the cmdline parser (which requires `0x824A6728 != 0`).

### Unused Hook Stubs

Two hooks exist in `src/eternalsonata_hooks.cpp` but are not wired to any
config entries:

- **`EternalsonataSkipSubObjectRelease()`** — no-op, intended for `sub_82294600`
- **`EternalsonataGuardNullResourceEntry(PPCRegister& r31)`** — returns `r31.u32 == 0`,
  intended for `sub_82160DC8`

These may be leftover from an earlier development approach or intended for
future use. They are currently dead code.

---

## 8. Init Callback Tables

### Pre-Init Table: `0x822F0494..0x822F04A0`

Small table of function pointers called very early in the init sequence.

### Main Init Table: `0x822F0010..0x822F0490`

Massive table of ~357 function pointers. Each is called during the main
initialization phase. This is the primary init sequence that sets up
subsystems (graphics, audio, input, filesystem, etc.).

### Post-Init Linked List: `0x822F052C`

A linked list of post-init callbacks, called after the main table.

### Callback Table: `0x82440828..0x82440844`

8 function pointers (32 bytes). Used by `sub_82132B28` to dispatch
command-line arguments. Each callback receives `(argcount, argarray)`.

---

## 9. Dev Tool References

The binary contains references to Xbox 360 development tools:

| Reference | Address/Location | Meaning |
|-----------|-----------------|---------|
| `e:\xbperfview.cap` | String in data | XBPerfView capture file (GPU profiling) |
| `xe:\pix\crashdump.pix2` | String in data | PIX crash dump file (GPU debugging) |
| `DbgPrint` | Imported function | Kernel debug print (SDK provides logging) |
| GPU debug register names | Data section | Register names for GPU debugging |

These are standard Xbox 360 development artifacts and indicate the binary
was built from a development environment.

---

## 10. Collision `todo:` Stubs

12 functions with `todo:` markers at vtable addresses:

| Address Range | Count | Purpose |
|---------------|-------|---------|
| `0x82083a20..0x82084018` | 12 | Collision system stubs |

These are placeholder implementations in the collision detection system,
suggesting incomplete or deferred features.

---

## 11. Implemented Hooks

### Summary

| Hook | Address | Type | Purpose | Status |
|------|---------|------|---------|--------|
| `sub_82254060` override | `sub_82254060` | `REX_HOOK_RAW` | Return 0 to bypass devkit gate | **Active** |
| `sub_821D50A8` override | `sub_821D50A8` | `REX_HOOK_RAW` | Rewrite `<wv>` waits to `<w>` (skippable voice lines) | **Active** |
| `EternalsonataSkipSubObjectRelease` | `sub_82294600` | (dead code) | Skip virtual destructor call | Not wired |
| `EternalsonataGuardNullResourceEntry` | `sub_82160DC8` | (dead code) | Guard null dereference | Not wired |

`sub_82254060` is the only hook required to boot; its effect is invisible by
design (without it the process self-terminates). `sub_821D50A8` is the only
user-visible one.

### Removed

| Hook | Address | Type | Reason |
|------|---------|------|--------|
| `EternalsonataEnableCmdlineParse` | `0x822542B0` | `[[midasm_hook]]` | `sub_82132B28` ignores all args — dead code path |
| `sub_822DFA88` override | `sub_822DFA88` | `REX_HOOK_RAW` | Console state bytes are never read; vtable has no render/input method |
| `sub_822E5BE8` override | `sub_822E5BE8` | `REX_HOOK_RAW` | Same — ConsoleSetting is dormant in retail |

---

## 12. Hooking Architecture

### How Hooks Work (ReXGlue SDK)

1. **Function Override** (`REX_HOOK_RAW`):
   - Generated code creates a weak alias via `DEFINE_REX_FUNC(name)`
   - Your strong `REX_HOOK_RAW(name)` overrides it at link time
   - Full access to `PPCContext& ctx` and `uint8_t* base`

2. **Mid-ASM Hook** (`[[midasm_hook]]` in TOML):
   - Codegen injects a call to your C++ function at a specific PPC instruction
   - Register list determines which `PPCRegister&` arguments are passed
   - Can fire before or after the instruction (`after_instruction`)
   - Supports conditional control flow (`return_on_true`, `jump_address`, etc.)

3. **Function Replacement** (`[functions]` in TOML):
   - Renames the function symbol in generated code
   - Does NOT create hooks — just affects naming/sizing during codegen

### Key Types

- `PPCContext` — Full PPC register file (r0-r31, f0-f31, cr, lr, ctr, xer, fpscr, etc.)
- `PPCRegister` — Single register with `.u32`, `.u64`, `.s32`, `.s64`, `.f32`, `.f64` accessors
- `uint8_t* base` — Pointer to guest memory base (guest addr `X` → host `base + (X - 0x82000000)`)

### Memory Macros

```cpp
REX_LOAD_U32(guest_addr)          // Read u32 from guest memory
REX_STORE_U32(guest_addr, value)  // Write u32 to guest memory
```

Guest addresses are translated through `base + offset + REX_PHYS_HOST_OFFSET(addr)`.

### Config File

- `eternalsonata_config.toml` — Main project config
- `eternalsonata_manifest.toml` — Codegen manifest (references the config)

### Build Pipeline

```bash
python scripts/build.py     # runs codegen + cmake build
./eternalsonata.exe         # runs the built binary
```

---

## 13. IDA Session Notes

### Session Info

- **Session ID**: `b75ab2d6`
- **Input**: `C:\Users\m.andronaco\dev\EternalSonataReprise\assets\default.xex`
- **Backend**: Headless (idalib worker)
- **Status**: Active

### Key Functions Analyzed

| Function | Address | Purpose |
|----------|---------|---------|
| `start` (xstart) | `0x82254248` | Title entry point |
| `sub_82254060` | `0x82254060` | Devkit privilege gate |
| `sub_822DFA88` | `0x822DFA88` | Debug console init |
| `sub_822E5BE8` | `0x822E5BE8` | ConsoleSetting init |
| `sub_82132B28` | `0x82132B28` | Main game function (init→loop→cleanup) |
| `sub_82132A08` | `0x82132A08` | Main game loop (task dispatch) |
| `sub_8212DA18` | `0x8212DA18` | Task system init |
| `sub_822D0190` | `0x822D0190` | Kernel/threading init (TLS, main thread) |
| `sub_82254C80` | `0x82254C80` | Pre-init callback runner (linked list at `0x822F052C`) |
| `sub_822E6440` | `0x822E6440` | Console destructor (NOT update) |
| `sub_822E71B8` | `0x822E71B8` | ConsoleSetting destructor |
| `sub_822CCAD0` | `0x822CCAD0` | Dynamic array push (registers shutdown callbacks) |
| `sub_820E64F0` | `0x820E64F0` | Base object init (sets vtable, copies name) |
| `sub_8212DDA8` | `0x8212DDA8` | Linked-list unlink (removes from task system) |
| `sub_82254B18` | `0x82254B18` | Returns cmd string addr (`0x101BE`) |

### Key Globals

| Address | Name | Description |
|---------|------|-------------|
| `0x8244C140` | Debug console | Console object with vtable |
| `0x8244C188` | Command buffer | 7,245-byte command buffer |
| `0x8244DDE0` | Console mode | -1 = disabled (never read after init) |
| `0x8244DDE4` | Console flag | 0x80000000 (never read after init) |
| `0x8244DDE8` | Console active | 1 = initialized (never read after init) |
| `0x82426728` | Cmdline gate | 0 = retail (always 0, never written) |
| `0x82565850` | ConsoleSetting | Settings console object |
| `0x82000698` | Language table | Language → region mapping |
| `0x82440828` | Task linked lists | 7 entries, iterated by init/loop/cleanup |
| `0x822F052C` | Shutdown callbacks | Linked list of registered shutdown handlers |
| `0x82082DDC` | Console vtable | Destructor + nullsub (2 methods) |
| `0x82082B60` | Base vtable | Shared base class vtable |

---

## 14. Options Screen (native settings rows)

Groundwork for exposing SDK cvars (starting with `fullscreen`) as native rows in
the game's own Options screen, rather than only in the ImGui overlay
(`src/settings.cpp`).

### Corrections to earlier notes

An earlier pass recorded a set of conclusions that are **wrong**. They are listed
here so they are not re-derived:

- **`sub_821D5CC0` is NOT the Options screen handler.** It is the text/glyph
  layout and rendering routine: it walks a character buffer (`v132`),
  special-cases char 32 (space) for word wrap, and emits per-glyph quads into an
  array at `a1 + 20140` with a 28-byte stride. The claimed "screen row list of
  triples at `+0x110`" and "8-entry registry table at `+0x88D0`" do not exist —
  those offsets were being read out of a glyph buffer.
- Do **not** call guest functions (e.g. `sub_82176CD8`) re-entrantly from inside
  a draw hook via `rex::CallFrame`. The reverted probe set did this and the game
  crashed on startup.

### Verified findings

- **`sub_821C9FE0` is a 67-case state dispatcher** on `*(a1 + 4)`, reached via
  `bctr`. Hex-Rays does not resolve the jump table; recover it manually:
  - offsets: `word_820824A0`, 67 × big-endian `u16`
  - target: `0x821CA078 + word_820824A0[state - 1]`
  - `0x821CC4B0` is the default/exit target (states 38, 45, 55).
  - **…but it is never called.** A hook on it did not fire once across a full
    boot + title + menu session (2026-08-05). Like `sub_8223FB78` (§ frame cap),
    it is dead code in this build. It is *not* "the options dispatcher" —
    do not build on it. This also explains why the earlier probe set logged no
    settings ticks.
- **BTX string ids are the registry keys.** `sub_82176CD8(registry, sid)` is
  called with the BTX string id in `r4`; a log-only hook on it works and is
  safe. Boot resolves sids 0..36; the rest of the Options range (37..54)
  resolves later, when the menu is first opened.
- **`dword_824CF500`** is the text/task registry; `sub_82176CD8(registry, key)`
  resolves a numeric message id to a task. `dword_824D0440` is the config base.
- **`sub_821A9F00` is a generic accessor** (60+ call sites across the whole UI),
  not Options-specific. It is not a useful anchor for locating the screen.
- **Options row labels** live in the packed UI message blob as consecutive
  entries in the English block:

  | Address | Label |
  |---|---|
  | `0x8203316C` | `Item Set` |
  | `0x8203317A` | `Attack Button` |
  | `0x82033188` | `Audio Output` |
  | `0x82033195` | `Volume` |
  | `0x8203319C` | `Piano Music` |
  | `0x820331A8` | `Music` |
  | `0x820331AE` | `Battle Camera` |
  | `0x820331BC` | `Vibration` |
  | `0x820331C6` | `Voice` |

  Value strings for these rows sit nearby (e.g. `Stereo` `0x820334AD`,
  `5.1ch Surround` `0x820334B9`).

- These strings have **no xrefs and no absolute pointer table** — the blob is a
  contiguous run of NUL-terminated strings (character names → `Main Menu`
  `0x820330FA` → `Items` `0x82033104` → …), addressed by *ordinal message id*,
  not by pointer. There are ~9 language copies (`Option` matches at
  `0x820331DD`, `0x8203377F`, `0x8203405E`, …).

### How screens are actually built (runtime-verified, 2026-08-05)

Static search failed here; all of the below came from a safe log-only probe.

- **`sub_8223B780(blob, string_id) -> char*`** is the BTX lookup (see
  `scripts/btx.py`). Hooking it and logging the *returned string* is the
  reliable way to identify a screen — far better than guessing from ids.
- **Ids are language-independent.** The running build is Italian
  (`off_822FF578[dword_8243D370]`), and id 35 resolved to `Pulsante Attacca`,
  37 `Volume`, 39 `Musica`, 40 `Fotoc. battaglia`, 43/44 `Giapponese`/`Inglese`
  — matching the USA block decoded statically. So the id table holds for any
  language.
- **Multiple BTX blobs exist.** The xex blob at `0x82031A00` holds the menu and
  Options text; the `.e` files carry their own (ids > 210 come from those).
  Dedup probes per `(blob, id)`, never per id.
- **The Options screen is real and confirmed**: ids 197 `Opzioni`, 201
  `Sottotitoli`, 54 `Conferma e torna al menu`, 144 `Effetti sonori`. Note the
  live row set is *not* what the USA string order suggests — there is a
  Subtitles row and **no Vibration row**.

#### The display-list mechanism

- **`sub_821F2F38(a1, list, ...)`** is a generic UI display-list interpreter,
  not a screen. The same call site (`0x821F4048`) draws the Menu, Player
  Controls and Options screens. `list` (r4, kept in r30) is a **variable-length
  record stream**: the function advances r30 by 8 / 0xC / 0x10 / 0x14 / 0x18 /
  0x1C depending on record type. For text records, `list+4` is the BTX id
  (`lwz r4, 4(r30)` at `0x821F3FC8` feeding `sub_8223B780`).
- **`sub_821EC050`** is the screen driver. It calls the interpreter from
  `0x821ECC38`, passing a different **static** display list per screen —
  observed `0x8205E880` and `0x8205F108` (id 65 = `Cambia pagina`). The lists
  live in the `0x8205xxxx` data region.

This is the mechanism a new row has to use: extend (a copy of) the Options
screen's display list with one more text record and point the interpreter at it.

#### Text record format (decoded 2026-08-05)

The Options display list lives at `0x8205E880`; its rows run from the title
record at `0x8205E9AC` to at least `0x8205EF64`, ending before the next list at
`0x8205F108`. A **text label record is 0x28 bytes**:

| Offset | Type | Meaning | Volume | Musica | Effetti sonori |
|---|---|---|---|---|---|
| `+0x00` | u32 | record type — `0xC8` = text label | 0xC8 | 0xC8 | 0xC8 |
| `+0x04` | u32 | **BTX string id** | 37 | 39 | 144 |
| `+0x08` | s32 | X | 120 | 490 | 490 |
| `+0x0C` | s32 | Y | 135 | 135 | 185 |
| `+0x10` | s32 | width | 300 | 300 | 300 |
| `+0x14` | s32 | height | 48 | 48 | 48 |
| `+0x18` | u32 | (0) | 0 | 0 | 0 |
| `+0x1C` | u32 | record size (0x28) | 40 | 40 | 40 |
| `+0x20` | s32 | (-1) | -1 | -1 | -1 |
| `+0x24` | u32 | (1) | 1 | 1 | 1 |

Verified record addresses (runtime): title id 197 `0x8205E9AC`, Battle Camera
id 40 `0x8205EA58`, Volume id 37 `0x8205EB88`, Music id 39 `0x8205EBB0`, Sound
Effects id 144 `0x8205EBE8`, Subtitles id 201 `0x8205EF64`.

Page-1 rows form a grid at X = 120 / 490 / 690, ending with the Voice row
(id 42, Y=335) whose values are `Inglese` (id 44, X=490) and `Giapponese`
(id 43, X=690) — confirmed in-game as the last row. Immediately after
`Giapponese` the stream switches to **type `0x64` records, 0x1C bytes**
(`{0x64, x, 0x32, y, 0x3E8, 0x3E8, -1}`), which matches the interpreter's
`addi r30, r30, 0x1C`. Between text groups sit 0x10-byte records of type
`0x258`.

So record **type is at `+0x00`** and length is type-dependent: `0xC8` → 0x28,
`0x64` → 0x1C, `0x258` → 0x10.

Two hypotheses that were tested and are **false** — do not re-derive them:
- `+0x24` is *not* a page index: `Sottotitoli` (id 201, a different page from
  the Voice row) also has `+0x24 = 1`.
- `+0x1C` holding `0x28` is not a general size field; it only happens to equal
  the record length for `0xC8` records (a `0x64` record is only 0x1C long, so
  `+0x1C` lies outside it).

Probe trick that produced this: `r30` is callee-saved, so inside a
`sub_8223B780` hook whose `lr` is in `sub_821F2F38`, `ctx.r30` still holds the
record pointer the interpreter is walking.

#### List bounds and the implemented row

- Dispatch: `type = index * 100`, `index = type / 100` (`li r10, 100; divw`),
  bounds-checked `> 0x1F`, jump table `word_82082428` (32 entries).
- **Terminator: a record whose type field is `0xFFFF`** (checked at
  `0x821F3150`; `0xFFFF / 100 = 655` also falls out of the jump table range).
- The Options list runs `0x8205E880` … terminator at `0x8205F0E8`, i.e. **0x868
  bytes** of records. There is no slack before the next structure, so the list
  must be *copied* to be extended, not patched in place.

`src/eternalsonata_hooks.cpp` implements a native **Fullscreen** row on this:
copy the list into `SystemHeapAlloc` memory, append two type-200 records
(label X=120, value X=490, Y=385) plus a fresh `0xFFFF`, swap the pointer in a
`sub_821F2F38` hook when the incoming list is the Options one, and answer two
synthetic string ids (900/901) from a `sub_8223B780` hook. Verified in-game:
the row renders, no crash, no game data touched.

#### Screen build vs. per-frame draw

The display list is walked **once, at screen construction**, not per frame:
`sub_821EC050` turns records into text objects via `sub_82176398`, and those
objects are what draw each frame. Consequence: a value sampled while building
(like the fullscreen cvar) is fixed until the screen is reopened — confirmed
in-game, the row updates after a reload.

#### Selection is a separate structure (why the row is not selectable)

`sub_821F62B8` is the per-frame cursor update. It walks a **linked list of
selectable items** rooted at `dword_824400E8 + 392`:

| Offset | Meaning |
|---|---|
| `+0x00` | item id (compared against the current selection) |
| `+0x2C` | flag (`0xFF` ⇒ cursor parked at 0,0) |
| `+0x30` | next item |

The current selection id lives at `dword_824400E8 + 396`. Position comes from
`sub_82200298(menu, &x, &y)`, and the cursor is placed at `y - 50` (the row
pitch) via `sub_821E9918` / `sub_821E9A00`.

A row in the display list therefore *draws* but is not *selectable* — the two
systems are independent. Making the Fullscreen row usable needs a node added to
this list plus a handler for its id.

This *is* the Options cursor (an earlier note doubted it: the four nodes all
read `x=y=10000` because that is the parked sentinel used while the cursor is
hidden — real coordinates appear once it moves).

#### Group / sub-item layout (runtime-verified)

Each node is a **group** of rows; `+0x2C` is the highlighted row *within* the
group:

| Offset | Meaning |
|---|---|
| `+0x00` | group id |
| `+0x04` | sub-item block base (sub-items are 0x10 bytes each) |
| `+0x08` | sub-item pointer array |
| `+0x0C` | active count (mirrored at `+0x0D`) |
| `+0x14` | previous group id (up) |
| `+0x20` | next group id (down), `-1` on the last group |
| `+0x2C` | index of the highlighted sub-item |
| `+0x30` | next node |

`sub_82200298` reads the cursor position from `array[index] + 4 / + 8`. The
array is **pre-allocated with 10 slots**; unused ones are parked at the
sentinel `(10000 + i, 10000 + i)`, so adding a row needs no allocation — set a
spare slot's x/y and raise the count byte.

Options groups: **id 3** = 3 rows at screen y 275/325/375, **id 2** = 2 rows at
430/480 and is the last group (`+0x20 == -1`). Screen y = display y + 145
(Sottotitoli 285→430, Voce 335→480).

#### The value highlight — SOLVED (2026-08-05)

A two-option row (Sottotitoli, Voce, and our Fullscreen row) draws **both**
choices side by side — label at X=120, options at X=490 and X=690 — and the game
marks the active one with a brighter background bar that slides horizontally the
moment the value changes, with no screen rebuild.

Everything below was tested against a live Sottotitoli toggle and **ruled out**
before the answer was found; do not re-derive them:

| Hypothesis | Result |
|---|---|
| It is the row cursor moving | No — the cursor is vertical only, and the row's sub-item stays at `x=550` throughout |
| The sub-item coordinates change | No — `group 2 sub[0]` never moved while toggling |
| A second selection group drives it | No — group 1 exists but stays `count=0`, parked, on this screen |
| It is positioned via `sub_821E9918` / `sub_821E9A00` | No — hooking both, they are called **only** for the row cursor, never on a value toggle |
| It is a display-list record the game repositions | No — the interpreter runs **once per screen entry** (measured), so nothing in the list can animate |
| The value lives in the `0x824D0440` config struct | No — the setting lives in `0x8243FC04` (see below); that earlier 0x800-byte diff was simply looking in the wrong place |

**How it was found.** Guessing was the problem, so the search was made
exhaustive: a full-memory differ (`menu_scan` cvar, "Value-highlight hunt" in
`src/eternalsonata_hooks.cpp`) snapshots all committed guest memory on every
left press and again on every right press, and intersects across toggles. A
two-option row is idempotent per direction, so left is unambiguously state A and
right state B regardless of press order. 334 MiB collapsed to 724 stable
addresses in ~3 s, of which only these were not pad state:

```
0x8243FC04  byte +1: 1 -> 0                  the Subtitles setting itself
0xF008DBF0  float 530 -> 730                 the bar's X
0xF0271750..0xF0271790  530/730, 631/849, 101/119   its quad: x, right edge, width
```

An lldb write-watchpoint on the bar X (host `0x1F008EBF0`) then gave the
per-frame draw path (`sub_82177A08` → `sub_82177878` → `sub_821649E8`), and
IDA xrefs on the setting byte led to the real prize.

**`sub_82201620` is the Options row input handler.** It resolves everything:

- **Row identity is the 4th field of the `1502` selection record** (see the next
  section): `id = *(u8*)(group_node + 44)`. On the second group the handler adds
  10, so Sottotitoli is `10` and Voce is `11`.
- **Settings live in `dword_8243FC04`, one byte per row**: `BYTE1`
  (`0x8243FC05`) is Subtitles, `BYTE2` (`0x8243FC06`) is Voice. Page-1 rows use
  separate globals `dword_8243F364` / `dword_8243F368` / `dword_8243F36C`.
- **The bar object is a field of the screen object**, `dword_824400E4[709] + N`:
  `+120` for Subtitles, `+92` for Voice, `+84 / +88 / +92` for the page-1 rows.
  (`[709]` is the same `4 * (idx + 709)` slot `sub_821F2F38` allocates.)
- **`sub_82179F78(&dword_824CF500, bar_obj, &xyz, 0.0, 14.0, 14.0, 14.0)` moves
  the bar** — a float `{x, y, z}` vector by pointer. This is the call the whole
  hunt was looking for.
- **Geometry**: `x = base + 200 * option_index`, where `base` is
  language-dependent — `530` for `dword_8243D370` in 3..4, `480` otherwise (a
  seventh case reads an uninitialised local). `y` is a per-row constant: `155`,
  `205`, `255` for page-1 rows, **`415` for Subtitles, `465` for Voice** — i.e.
  display Y + 130. This matches the scanned `530 -> 730` exactly.
- `sub_82202358(n)` applies setting `n` after the change; `sub_821425D8(..., 5,
  0, 0)` plays the change SFX.

So a two-option row needs three things: a settings byte, a bar object, and a
case in this handler. Our Fullscreen row has the first (the cvar) and the third
(our own hook), and is missing **only the bar object**.

#### The bar is a registry *id*, not a pointer

Dumped live (`menu_scan`, `DumpHighlightBars` in `eternalsonata_hooks.cpp`).
`root = dword_824400E4`, `page = *(u8*)(root + 2833)`,
`screen = *(u32*)(root + 4 * (page + 709))` — the 1744-byte object
`sub_821F2F38` allocates. The four "bar" slots hold **small integers**:

```
screen+ 84 = 0x48   screen+ 88 = 0x49   screen+ 92 = 0x68   screen+120 = 0x7D
```

That matches `sub_82179F78`, which does nothing but pack its float args and
call `sub_821771F8(&dword_824CF500, id, params, 0, 0, -1)` — the second
argument is passed straight through as an **id into the `dword_824CF500`
registry**, the same registry `sub_82176CD8` and `sub_82178698` index. So a bar
is a registered task, and giving our row one means getting an id minted for it.

Ids are allocated sequentially per screen build and **accumulate across the
session** — a second Options entry shifted every id in the screen object by a
uniform +283. Any A/B on this dump has to normalise by that offset first.

The screen object is a set of id arrays with count bytes at exactly the offsets
`sub_821F2F38` zeroes (72, 380, 640, …); e.g. count at `+0x17C` (380) with its
id array at `+0x180` (384).

#### The bar, end to end (implemented)

A row's highlight bar needs three things, all now in place for the Fullscreen
row:

1. **A bar object**, created by a **type-110** record — the 100 family, subtype
   10. The stock Subtitles one is at list offset `0x6C4`:
   `{110, 234, 490, 285, 1000, 1000, -1}` = sprite 234, x 490 (the left
   option's column), y 285 (the row's display y), and a size scale in
   thousandths at `+0x10`/`+0x14`.
2. **The surrounding state block.** The bar is not a lone record; it sits
   between `{2101, 2100, 1}` at `0x6B8` and `{2}` at `0x6E0`. That pair sets
   and restores the draw state the type-100 family handler branches on (the
   `r27` test at `0x821F3610`). Emitted outside it, the bar draws visibly
   **darker** than the stock ones. Clone the whole `0x6B8..0x6E4` block.
3. **Placement**, which the display-list record does *not* decide. Two
   different calls in two different coordinate spaces:
   - **`sub_82200FE8` is the Options screen init**, and it places every bar
     with `sub_82178A88(&dword_824CF500, id, &{x,y,z}, 0, 0, -1)` — an instant
     set, no animation.
   - **`sub_82201620` moves it on a value change** with `sub_82179F78`.

   | slot | init x | init y | handler y |
   |---|---|---|---|
   | +84 | `base + 200*(1 - byte_8243FBFC)` | 895 | 155 |
   | +88 | `base + 200*dword_8243F368` | 945 | 205 |
   | +92 | `base + 200*(BYTE2(FC04) ^ 1)` | 1205 | 465 |
   | +120 | `base + 200*(1 - BYTE1(FC04))` | 1155 | 415 |

   The two spaces differ by a constant **740** on every row, and `base` is the
   language-dependent 530/480. The Fullscreen row continues the 50px pitch
   after Subtitles (415) and Voce (465): **515 handler, 1255 init**.

Do not try to derive the bar's position from the y authored in its record —
the record's y is in neither space, and chasing it is what produced a round of
"a few pixels off" guessing before `sub_82200FE8` was found.

Ordering matters as much as content: the id array at `screen+0x4C` is indexed
**positionally** (`sub_82201620` uses elements 2/3/4/11), so a bar record must
be appended *after* every stock object-creating record or the stock rows are
renumbered and their highlights move. Only the 100 family creates entries —
text (200) and 600 do not, which is why the row's three text records never
disturbed the indices.

Implemented in `src/eternalsonata_hooks.cpp`: the row's copy of the display
list appends a cloned bar block at 600x800 thousandths, `MoveFullscreenBar`
places it on entry with `sub_82178A88` and slides it on toggle with
`sub_82179F78`.

The one thing not readable from the game is the 7px vertical fixup. Dumping
the stock Subtitles bar object beside ours (`DumpHighlightBars`) showed them
identical but for y - differing by exactly the 100 of two row pitches, so
correct - and the scale at `+0x4C`/`+0x50`, 1.0/1.0 stock against our
0.60/0.80. The bar sprite is anchored at its top, so shrinking it lifts the
bottom edge and the centre rises by `(1 - scale) * height / 2`; 7px settles it,
close to the 5px a 50px-tall bar predicts, so the sprite is slightly taller
than the row pitch. The fixup is applied to all three y values (record, init
and move) so the opening and snapped positions stay in step.

Object layout, for reference: `+0x3C` x, `+0x40` y, `+0x4C`/`+0x50` x/y scale
as floats - which is what confirms `+0x10`/`+0x14` of the record are that scale
in thousandths.

#### Type-100 records do NOT mint a bar — and crash the screen

Tested directly: `fs_row_bar` appends `{100, x, 50, y, 1000, 1000, -1}` (the
exact shape the Options list already carries) to the Fullscreen row's copy of
the list. Result, after normalising the +283 id shift:

- **Every id array is byte-identical** with and without the record — no object
  was registered.
- The **count byte at `+0x17C` went 13 → 14** — something counted the record.
- The game **crashed a few seconds after entering Options**, consistent with
  per-frame code iterating 14 entries against an array that only has 13 valid
  ones.

So a bare type-100 subtype-0 record is not self-sufficient. Type 100 is a
family — the handler at `0x821F35DC` sub-dispatches on `type - 100` over 21
entries via `word_820823E0` — and its subtype-0 path at `0x821F3610` branches on
`r27` (loop state carried between records, `clrlwi. r11, r27, 24`) and reads
`record+0x18` as a handle. Whatever earlier record sets that state is missing at
our insertion point.

**Do not fire more record shapes at the game to find out.** Read the subtype-0
handler at `0x821F3610` (and what writes `r27` in the interpreter loop) first,
then place the record where its preconditions hold.

Once a bar id exists, driving it is already understood: call
`sub_82179F78(&dword_824CF500, id, {base + 200*i, 515, 0}, 0, 14, 14, 14)` from
the existing `sub_821F62B8` hook — the same per-frame context `sub_82201620`
calls it from, and our row's display Y of 385 puts its bar at Y 515.

#### Disproved here — do not retry

- **The cursor does not move between a row's two options.** Toggling
  Sottotitoli between `Sì` and `NO` leaves its sub-item at `x=550`
  throughout; it only changes when the screen tears down. Whatever marks the
  active choice is *not* the sub-item coordinates, and is still unidentified.
- **`<g>` is not interpreted as markup by these labels.** Prefixing a value
  string with it renders the three characters literally (which shows up as the
  text being shifted ~3 characters right). Menu strings such as
  `<g><m2>Opzioni` are stored with tags, but the Options row path does not
  expand them.
- **The `fullscreen` cvar is inert on its own.** `FlagEntry` has no change
  callback and nothing watches the value, so setting it does not change the
  window — `rex::ui::Window::SetFullscreen()` must be called as well (see
  `eternalsonata::SetFullscreenSetting` in `settings.cpp`).

#### Pad state

`sub_821281B8` polls 4 pad objects at `0x824BB418`, stride 464.
`sub_82128310` fills each: `+424` held, `+8` previous, **`+428` newly pressed**,
`+432` pressed-or-repeat, `+436` released. It folds the left stick into
synthetic bits alongside the d-pad — confirmed live: `0x1000` = A,
`0x10000`/`0x20000` = up/down, **`0x40000`/`0x80000` = left/right**. Matching
only the d-pad bits (`0x4`/`0x8`) misses stick input entirely.

#### The selectable-item list is declared in the display list

Open question 1 below is also answered. The groups are not built by code — they
are records in the same display list, sitting between the last row's text
records and the terminator (Options: `0x8205F008` … `0x8205F0E8`). All four
types divide to jump-table index 15:

| Type | Layout | Meaning |
|---|---|---|
| `1500` | `{1500, group_id}` | begin group |
| `1502` | `{1502, x, y, row_id}` | one selectable row, at screen x/y |
| `1501` | `{1501}` | end of the group's items |
| `1503` | `{1503, group_id, up, ?, ?, down}` | navigation links |

Decoded from the Options list, exactly matching what was measured at runtime:

```
1500 0    1502 550 165 1   1502 550 215 2   1501  1503 0 -5 -1 -1 -5
1500 3    1502 550 275 1   1502 550 325 2   1502 550 375 3  1501  1503 3 0 -1 -1 2
1500 2    1502 550 430 5   1502 550 480 4   1501  1503 2 3 -1 -1 -1
```

`x = 550` is the cursor column; the y values are the screen rows; the 4th field
is the **row id** `sub_82201620` switches on (Sottotitoli `5`, Voce `4`, +10 on
this group). So a new row should be declared with a `1502` record in the copied
list rather than by the runtime count-bump the hook currently does in
`sub_821F62B8` — that hack works, but it is patching around the real mechanism.

### Open work

1. ~~Find what builds the selectable-item list~~ — done, it is declared by
   `1500`/`1502`/`1501`/`1503` records in the display list (above). Migrate the
   runtime count-bump in `sub_821F62B8` to a `1502` record at `(550, 530)`.
2. ~~Hook confirm/left-right handling to flip the cvar~~ — done, implemented on
   the pad edge in the `sub_821F62B8` hook.
3. ~~Give the row a highlight bar~~ — done; see "The bar, end to end" above.
   The bar is a live object, so no screen rebuild is needed on toggle.
The native Fullscreen row is complete: it draws, is selectable, toggles the
cvar, and carries a highlight bar that starts on the active option and slides
between them.

---

*Last updated: 2026-08-05 — added §14; corrected the `sub_821D5CC0` misreading
and recovered the `sub_821C9FE0` jump table. Later the same day: solved the
value highlight (`sub_82201620` / `sub_82179F78`), found the settings globals at
`0x8243FC04`, decoded the `1500`/`1502` selection records, and implemented the
Fullscreen row's own highlight bar (type-110 record + `sub_82200FE8`'s init
placement).*

---

## 15. Profiling cvars

Two independent, unrelated instruments for asking "why is the frame slow." Both
default to off. Neither reads the other's data.

### `native_profile_zones` (`src/settings.cpp`)

Gates `ProfileZone` (`src/native_renderer_profile.h`), the manually placed
timers inside the native renderer's own code: present, draw, vertex upload,
texture bind, fence wait, and so on (full phase list in
`native_renderer_draw.cpp`'s `kPhaseNames`). Each zone reads the clock on
construction and destruction, which is cheap once but adds up: with millions of
draws and several nested zones each, this alone was measured as several
percent of frame time on its own, so it stays off unless someone wants the
per-phase breakdown and the CPU/GPU bound verdict that the swap summary prints.

Only tells you about time spent inside the native renderer's own C++. It has no
way to see guest code, kernel calls, or anything else running on the thread.

### `guest_profile` (`src/guest_profiler.cpp`)

Gates a separate stack-sampling profiler: a background thread periodically
suspends the render thread, reads its actual call stack via
`SuspendThread`/`GetThreadContext`, and symbolizes it. Also tracks wait/block
time by kind, and a handful of guest zones (`kZoneNames` in the same file,
covering the render task, animation update, and related guest functions).

Because it samples the real call stack, it sees everything running on that
thread: guest PPC code, kernel calls (`NtWaitForSingleObject`), even unrelated
injected code such as `NVENCODEAPI_Thunk` from a capture overlay. This is the
tool for "what is actually running," as opposed to `native_profile_zones`'s
"how long did our own code take."

Turn it on with the F3 overlay (Guest Profiler window) or log continuously with
`--guest_profile=true` / the `guest_profile` cvar. `guest_profile_top` caps how
many ranked rows print; `guest_profile_hz` sets the sampling rate.
