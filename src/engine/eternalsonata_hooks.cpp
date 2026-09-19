#include <algorithm>
#include <bit>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>

#include "generated/eternalsonata_init.h"

#include <rex/cvar.h>
#include <rex/system/kernel_state.h>

#include "eternalsonata_hooks_internal.h"
#include "native_renderer_frame.h"
#include "overworld_system.h"

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

// sub_82108180(camera, vertical_fov, near, far) is the only projection setup in
// the binary. It derives the projection matrix at camera+304, the stored field
// of view at camera+368 and the near plane extents at camera+380..392 from that
// one angle, so scaling the argument moves the rendered view and the frustum
// the cull test below uses together.
REXCVAR_DECLARE(double, camera_fov_scale);
REX_EXTERN(__imp__sub_82108180);
REX_HOOK_RAW(sub_82108180) {
    const double scale = std::clamp(REXCVAR_GET(camera_fov_scale), 0.5, 2.0);
    const double fov = ctx.f1.f64;
    // Radians, and the cameras seen so far sit at pi/2. Anything outside a
    // plausible angle is not the value this hook thinks it is.
    if (scale == 1.0 || fov <= 0.01 || fov >= 2.5) {
        __imp__sub_82108180(ctx, base);
        return;
    }

    const u32 camera = ctx.r3.u32;
    ctx.f1.f64 = fov * scale;
    __imp__sub_82108180(ctx, base);

    // sub_821078B0 re-runs this every frame with the angle stored back at
    // camera+368, so leaving the scaled one there multiplies it again each
    // frame. Put the guest's own angle back and let only the projection and
    // the extents derived from it carry the scale.
    if (camera) {
        REX_STORE_U32(camera + 368, std::bit_cast<u32>(static_cast<float>(fov)));
    }
}

// The renderer widens the world after the guest has already culled its models.
// Give the guest sphere test the same field of view for this call. A window
// wider than 16:9 expands horizontally and a taller one vertically, so both
// extent pairs have to follow their own axis.
REX_EXTERN(__imp__sub_82108878);
REX_HOOK_RAW(sub_82108878) {
    const u32 camera = ctx.r3.u32;
    float clip_x = 1.0f;
    float clip_y = 1.0f;
    eternalsonata::FrameWorldClipScale(&clip_x, &clip_y);

    // Top, bottom, right and left extents at the near plane. Bottom and left
    // are negative, so a plain divide widens them the right way.
    struct Extent {
        u32 offset;
        float scale;
    };
    const Extent extents[] = {
        {380, clip_y}, {384, clip_y}, {388, clip_x}, {392, clip_x},
    };

    u32 saved[std::size(extents)] = {};
    bool widened = false;
    if (camera) {
        for (size_t i = 0; i < std::size(extents); ++i) {
            saved[i] = REX_LOAD_U32(camera + extents[i].offset);
            if (extents[i].scale >= 1.0f || extents[i].scale <= 0.0f) {
                continue;
            }
            REX_STORE_U32(camera + extents[i].offset,
                          std::bit_cast<u32>(std::bit_cast<float>(saved[i]) /
                                             extents[i].scale));
            widened = true;
        }
    }

    __imp__sub_82108878(ctx, base);

    if (widened) {
        for (size_t i = 0; i < std::size(extents); ++i) {
            REX_STORE_U32(camera + extents[i].offset, saved[i]);
        }
    }
}

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
// Skippable voice waits
// ---------------------------------------------------------------------------

// A message ending in `<wv>` (control code 13, emitted by the markup
// preprocessor sub_821D50A8) waits for its voice clip and offers no way to cut
// it short. The consumer is the code-13 case of the layout pass sub_821D5CC0
// (jump table word_82082230, case at 0x821D69AC): on first arrival it parks the
// record in state 8 (a2+4) while sub_821431C0(dword_8243D89C, handle) says the
// clip at mgr+31372 is still playing, and advances once that returns 0.
//
// The advance button is already decoded for the `<w>` case: sub_821D49A0
// latches it into byte_8255D125 (mgr+31381) right before calling the layout
// pass. So when the record is parked on a voice and that flag is set, stop the
// clip the way the game's own skip path in sub_821D96F8 does
// (sub_82142EE8(mgr, handle, fade 0)) and answer "not playing" for that handle
// during this pass. A line the player does not touch still ends on its own.
//
// Only the captured handle is answered, so a `<vN>` later in the same pass
// starts a new clip that is waited on normally.
namespace {
constexpr u32 kTextManager = 0x82555690;
constexpr u32 kVoiceHandleOffset = 31372;
constexpr u32 kAdvancePressedOffset = 31381;
constexpr u32 kSoundManagerPtr = 0x8243D89C;
constexpr u32 kStateWaitingForVoice = 8;
u32 g_skipped_voice_handle = 0;
}  // namespace

REX_EXTERN(__imp__sub_821D5CC0);
REX_EXTERN(__imp__sub_82142EE8);
REX_HOOK_RAW(sub_821D5CC0) {
    const u32 mgr = ctx.r3.u32;
    const u32 record = ctx.r4.u32;
    g_skipped_voice_handle = 0;
    if (mgr == kTextManager && record && REX_LOAD_U32(record + 4) == kStateWaitingForVoice &&
        REX_LOAD_U8(mgr + kAdvancePressedOffset)) {
        const u32 handle = REX_LOAD_U32(mgr + kVoiceHandleOffset);
        if (handle) {
            ctx.r3.u32 = REX_LOAD_U32(kSoundManagerPtr);
            ctx.r4.u32 = handle;
            ctx.f1.f64 = 0.0;
            __imp__sub_82142EE8(ctx, base);
            ctx.r3.u32 = mgr;
            ctx.r4.u32 = record;
            g_skipped_voice_handle = handle;
        }
    }
    __imp__sub_821D5CC0(ctx, base);
    g_skipped_voice_handle = 0;
}

REX_EXTERN(__imp__sub_821431C0);
REX_HOOK_RAW(sub_821431C0) {
    if (g_skipped_voice_handle && ctx.r4.u32 == g_skipped_voice_handle) {
        ctx.r3.u32 = 0;
        return;
    }
    __imp__sub_821431C0(ctx, base);
}

// ---------------------------------------------------------------------------
// Text window content
// ---------------------------------------------------------------------------

// sub_821D3890 is SetText(mgr, window_id, text, tag) on the global text manager
// (dword_82555690).  It copies the raw markup into the window record at +8, or
// into the 1024 byte overflow buffer at 0x8255DFC2 for strings of 400 bytes or
// more.  Hooked rather than the preprocessor because it fires exactly once per
// "this window now shows this string".
REX_EXTERN(__imp__sub_821D3890);
REX_HOOK_RAW(sub_821D3890) {
    const u32 window = ctx.r4.u32;
    const u32 text = ctx.r5.u32;
    if (text) {
        eternalsonata::NotifyOverworldDialogue(
            window, reinterpret_cast<const char*>(base + text));
    }
    __imp__sub_821D3890(ctx, base);
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
