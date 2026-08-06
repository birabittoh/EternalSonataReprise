#include "generated/eternalsonata_init.h"

// ---------------------------------------------------------------------------
// Debug hooks
// ---------------------------------------------------------------------------

// sub_82254060 is the devkit-privilege gate called early in xstart (the title
// entry point).  It probes XexCheckExecutablePrivilege(0xA), XGetAVPack, and
// two ExGetXConfigSetting calls; if any check indicates a non-dev retail
// environment it returns 1, which makes xstart call XamLoaderTerminateTitle
// and kill the process.  In the recompiled port we are always "dev-capable"
// so the simplest fix is to override the whole function and return 0 (pass).
REX_EXTERN(__imp__sub_82254060);
REX_HOOK_RAW(sub_82254060) { ctx.r3.u64 = 0; }

// The debug-console (sub_822DFA88) and ConsoleSetting (sub_822E5BE8) init
// hooks used to live here.  Both forced "console active" state bytes after
// the original init ran, and both were removed as dead code: the retail build
// keeps only the allocation and teardown of those objects.  Their state bytes
// (dword_8244DDE0, byte_8244DDE8, ...) have zero reads after init, the command
// buffer at 0x8244C188 is only ever zeroed, and the console vtable
// off_82082DDC holds just a destructor and a nullsub - no render, input, or
// command-dispatch method survives in the binary.  An on-screen console has to
// be built host-side.

// ---------------------------------------------------------------------------
// Skippable message waits
// ---------------------------------------------------------------------------

// sub_821D50A8 is the dialogue markup preprocessor.  It walks a raw text entry
// out of a .e file, expands the `<...>` tags into single-byte control codes in
// a scratch buffer at a1+19104, stores each tag's numeric argument into the
// parallel slot array at a2 + 8*(argidx+65) (argidx counter at a2+840), and
// finally copies the scratch buffer to a2+8 (or a1+35122) + *(u16*)(a2+420).
//
// The three "end of message" tags and the control codes they emit
// (jump table word_820821B8, base loc_821D5740, chars 'n'..'z'; the `w` case
// is at 0x821D5804):
//
//   <w>        -> 2    wait for player input, no timeout   (29473 uses)
//   <wNNNN>    -> 1    auto-advance after NNNN ms, arg=NNNN (26829 uses)
//   <wv>       -> 13   wait for the voice clip to finish    (568 uses)
//
// `<wv>` has no player-skip path at all, so a long voice line (e.g. the battle
// tutorial narration in btldata/script/tutorial/t0001.e) blocks for the full
// clip.  Rewriting the emitted code to 2 puts those messages on the ordinary,
// well-travelled "press a button to advance" path without touching the assets.
//
// This is safe with respect to the argument array: the <w> and <wv> paths both
// advance the arg index by exactly 1, and <w> never reads its slot, so no
// re-indexing is needed - only the control byte changes.

// Make <wv> (wait-for-voice) player-skippable.  This is the one that motivated
// the hook.
//
// Fallback if the code-2 path turns out not to draw an advance prompt during
// battle-tutorial narration: rewrite 13 -> 1 (kWaitTimed) instead.  The <wv>
// handler at 0x821D5858 already stores 0 into that message's argument slot, so
// a timed wait of 0 ms advances immediately rather than waiting for input.
static constexpr bool kSkippableVoiceWaits = true;

// Also convert <wNNNN> (timed auto-advance) into a player wait.  Off by
// default: it would make ~26k normally self-advancing messages - including
// non-dialogue things like title cards - demand a button press.
static constexpr bool kSkippableTimedWaits = false;

static constexpr u8 kWaitForInput = 2;
static constexpr u8 kWaitTimed = 1;
static constexpr u8 kWaitForVoice = 13;

// Guard against a missing terminator in a malformed entry.
static constexpr u32 kMaxMessageBytes = 8192;

REX_EXTERN(__imp__sub_821D50A8);
REX_HOOK_RAW(sub_821D50A8) {
    // The original clobbers r3/r4, so capture the arguments up front.
    const u32 a1 = ctx.r3.u32;
    const u32 a2 = ctx.r4.u32;

    __imp__sub_821D50A8(ctx, base);

    if (!a2 || (!kSkippableVoiceWaits && !kSkippableTimedWaits)) {
        return;
    }

    // Recompute the destination exactly as the tail of sub_821D50A8 does.
    const u32 dest = (REX_LOAD_U32(a2) == REX_LOAD_U32(a1 + 36148)) ? (a1 + 35122) : (a2 + 8);
    const u32 start = dest + REX_LOAD_U16(a2 + 420);

    for (u32 p = start; p < start + kMaxMessageBytes; ++p) {
        const u8 c = REX_LOAD_U8(p);
        if (!c) {
            break;
        }
        if ((kSkippableVoiceWaits && c == kWaitForVoice) ||
            (kSkippableTimedWaits && c == kWaitTimed)) {
            REX_STORE_U8(p, kWaitForInput);
        }
    }
}

// ---------------------------------------------------------------------------
// Dead lead, kept as a note: sub_8223FB78
// ---------------------------------------------------------------------------
//
// An earlier attempt hooked sub_8223FB78, believed to hold "the single FPS cap
// in the whole image" (an `elapsed_us >= 30000` gate). That was wrong: IDA
// reports exactly one xref to 0x8223FB78, at 0x820B2448, and that is a .pdata
// unwind entry, not a dispatch-table slot. Nothing in the image references the
// function as code or data, and a live breakpoint on it never hit. It is dead
// code in this build. Do not resurrect that hook.
