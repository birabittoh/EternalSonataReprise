#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include "generated/eternalsonata_init.h"

#include <rex/system/kernel_state.h>

#include "eternalsonata_hooks_internal.h"

// ---------------------------------------------------------------------------
// Console references in the shipped text
// ---------------------------------------------------------------------------
//
// The BTX blobs baked into the image (0x8202B8A8, 0x822FDD00 and friends) warn
// the player not to switch off the Xbox 360, which the save screens still show.
// Each language gets its own rewrite so the sentence stays grammatical.
//
// The blobs sit in read-only guest pages, so the strings cannot be edited in
// place; the fixed copies live on the guest heap and the BTX lookup hands them
// out instead (see the sub_8223B780 hook in eternalsonata_options.cpp).

namespace eternalsonata_hooks {
namespace {

struct ConsoleTextFix {
    const char* warning;  // where the console warning starts, per language
    const char* to;       // what to say instead, to the end of the string
};

// Neither half of the warning survives the port: there is no storage device to
// remove and no console to switch off, only the game to keep open. So the whole
// sentence is replaced, from the word each language opens it with, and whatever
// comes before it (the "Checking save files..." line) is kept.
//
// Latin-1, the encoding the blobs use. Each opener is unique to its language,
// and none of them appears in the line above the warning, so the first rule that
// matches is the right one.
constexpr ConsoleTextFix kConsoleTextFixes[] = {
    {"Please do not remove", "Please do not close the game."},
    {"Ne pas ", "Ne pas fermer le jeu."},
    {"Non rimuovere", "Non chiudere il gioco."},
    {"No retires", "No cierres el juego."},
    {"Bitte das ", "Bitte das Spiel nicht beenden."},
};

// Longest blob string we are willing to copy.
constexpr size_t kMaxBlobString = 1024;

std::mutex g_console_text_mutex;
// Looked-up string -> our copy, or 0 for "nothing to do". Keyed by address so
// each string is examined once; the blobs are static, so the answer never
// changes.
std::unordered_map<u32, u32> g_console_text;

// Our copy of `text`, on the guest heap, or 0 if it needs no fixing.
u32 FixedCopyOf(u8* base, std::string text) {
    bool fixed = false;
    for (const ConsoleTextFix& fix : kConsoleTextFixes) {
        const size_t pos = text.find(fix.warning);
        if (pos != std::string::npos) {
            text.replace(pos, std::string::npos, fix.to);
            fixed = true;
            break;
        }
    }
    if (!fixed) {
        REXLOG_WARN("[text] no rule for the Xbox 360 string \"{}\"", text);
        return 0;
    }
    auto* mem = rex::system::kernel_memory();
    const u32 copy = mem ? mem->SystemHeapAlloc(text.size() + 1, 0x20) : 0;
    if (!copy) {
        REXLOG_WARN("[text] guest allocation failed for \"{}\"", text);
        return 0;
    }
    for (size_t i = 0; i <= text.size(); ++i) {
        REX_STORE_U8(copy + i, static_cast<u8>(text[i]));
    }
    REXLOG_INFO("[text] rewrote \"{}\"", text);
    return copy;
}

}  // namespace

u32 ConsoleTextOverrideFor(u8* base, u32 text_address) {
    if (!text_address) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(g_console_text_mutex);
    const auto it = g_console_text.find(text_address);
    if (it != g_console_text.end()) {
        return it->second;
    }

    // Matching on the text rather than on the address: a blob string's start is
    // only known from its block's offset table, and the first string of a block
    // has no terminator in front of it to find it by.
    const char* const s = reinterpret_cast<const char*>(base + text_address);
    const size_t len = strnlen(s, kMaxBlobString);
    u32 copy = 0;
    if (len < kMaxBlobString && std::strstr(s, "Xbox 360")) {
        copy = FixedCopyOf(base, std::string(s, len));
    }
    g_console_text[text_address] = copy;
    return copy;
}

}  // namespace eternalsonata_hooks

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
// Storage-device textboxes
// ---------------------------------------------------------------------------
//
// sub_8223FB78 drives the storage-device screen: a1[99] is the next state,
// a1[100] selects state 7's message. Two states only narrate Xbox 360 storage
// selection and are skipped straight to their successor:
//
//   state 6                 "Please select a storage device."   -> 4, opens
//                                                                  the selector
//   state 7 with a1[100]==4 "There is enough available space."  -> 1, commits
//                                                                  the device
//
// Other a1[100] values are real errors ("not signed in", "insufficient
// space") and are left alone.
//
// Rewriting the state in the driver rather than in sub_8223FC88's entry action
// is required: the driver assigns a1[98] = a1[99] right after the action, so a
// change made there is swallowed and the successor never starts.

static constexpr u32 kStorageStateNext = 99 * 4;
static constexpr u32 kStorageMessageId = 100 * 4;

REX_EXTERN(__imp__sub_8223FB78);
REX_HOOK_RAW(sub_8223FB78) {
    const u32 a1 = ctx.r3.u32;
    if (a1) {
        const u32 state = REX_LOAD_U32(a1 + kStorageStateNext);
        if (state == 6) {
            REX_STORE_U32(a1 + kStorageStateNext, 4);
        } else if (state == 7 && REX_LOAD_U32(a1 + kStorageMessageId) == 4) {
            REX_STORE_U32(a1 + kStorageStateNext, 1);
        }
    }

    __imp__sub_8223FB78(ctx, base);
}
