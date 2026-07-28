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
python scripts/run.py       # runs the built binary
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

*Last updated: 2026-07-29 — dormancy verdict re-verified independently; dead
console hooks removed from `src/eternalsonata_hooks.cpp`.*
