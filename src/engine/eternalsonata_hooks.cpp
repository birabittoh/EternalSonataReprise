#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <iterator>

#include "generated/eternalsonata_init.h"

#include <rex/cvar.h>
#include <rex/system/kernel_state.h>

#include "eternalsonata_hooks_internal.h"
#include "native_renderer_frame.h"
#include "overworld_system.h"

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

// ---------------------------------------------------------------------------
// Mirror plane
// ---------------------------------------------------------------------------

// sub_82125638 renders a mirror: it reflects the camera about the plane through
// the mirror mesh's first vertex, with the vertex's packed normal, both read in
// object space. The mirror node's world matrix is never applied, which holds
// for the 360 maps (identity) and breaks a rotated mirror such as PS3 bqj16.
// The wrapper finds the mirror model's matrix with the same scan the function
// does, and the sub_820C0C60 call that normalizes the plane normal moves both
// to world space.
namespace {
constexpr u32 kMirrorNormalizeReturn = 0x82125810;
bool g_mirror_world_valid = false;
float g_mirror_world[12] = {};

float LoadF32(uint8_t* base, u32 address) {
    return std::bit_cast<float>(REX_LOAD_U32(address));
}

bool FindMirrorWorld(uint8_t* base) {
    const u32 object_count = REX_LOAD_U32(0x824BB398);
    const u32 objects = REX_LOAD_U32(0x824BB3B8);
    for (u32 i = 0; i < object_count && objects; ++i) {
        const u32 slot = objects + i * 16;
        const u32 object = REX_LOAD_U32(slot + 4);
        if (REX_LOAD_U32(slot) || !object) {
            continue;
        }
        const u32 entries = REX_LOAD_U32(object + 0x564);
        const int32_t entry_count = static_cast<int32_t>(REX_LOAD_U32(object + 0x568));
        for (int32_t j = 0; j < entry_count && entries; ++j) {
            const u32 entry = entries + j * 64;
            if (REX_LOAD_U32(entry + 0x14) & 1) {
                continue;
            }
            const u32 model = REX_LOAD_U32(entry);
            const u32 mesh = REX_LOAD_U32(entry + 4);
            if (!model || !mesh) {
                continue;
            }
            const u32 flags =
                REX_LOAD_U32(REX_LOAD_U32(mesh) + 0x2C) | REX_LOAD_U32(model + 0x620);
            if (!(flags & 0x20000000)) {
                continue;
            }
            // Only a static mirror, whose world matrix is the model's own (row
            // vectors, translation in row 3). A skinned one keeps the old plane.
            if (REX_LOAD_U32(model + 404) && REX_LOAD_U32(mesh + 24)) {
                return false;
            }
            for (u32 k = 0; k < 4; ++k) {
                for (u32 i = 0; i < 3; ++i) {
                    g_mirror_world[i * 4 + k] = LoadF32(base, model + 336 + k * 16 + i * 4);
                }
            }
            return true;
        }
    }
    return false;
}
}  // namespace

REX_EXTERN(__imp__sub_82125638);
REX_HOOK_RAW(sub_82125638) {
    g_mirror_world_valid = FindMirrorWorld(base);
    __imp__sub_82125638(ctx, base);
    g_mirror_world_valid = false;
}

// Rows are (right, up, forward, translation) so row i dotted with (p, 1) is
// world axis i. f29..f27 hold the vertex position and r3 points at the normal.
REX_EXTERN(__imp__sub_820C0C60);
REX_HOOK_RAW(sub_820C0C60) {
    if (g_mirror_world_valid && ctx.lr == kMirrorNormalizeReturn) {
        g_mirror_world_valid = false;
        const float* m = g_mirror_world;
        const float p[3] = {static_cast<float>(ctx.f29.f64), static_cast<float>(ctx.f28.f64),
                            static_cast<float>(ctx.f27.f64)};
        const u32 normal = ctx.r3.u32;
        const float n[3] = {LoadF32(base, normal), LoadF32(base, normal + 4), LoadF32(base, normal + 8)};
        double* position[3] = {&ctx.f29.f64, &ctx.f28.f64, &ctx.f27.f64};
        for (int i = 0; i < 3; ++i) {
            const float* row = m + i * 4;
            *position[i] = row[0] * p[0] + row[1] * p[1] + row[2] * p[2] + row[3];
            const float world_n = row[0] * n[0] + row[1] * n[1] + row[2] * n[2];
            REX_STORE_U32(normal + i * 4, std::bit_cast<u32>(world_n));
        }
    }
    __imp__sub_820C0C60(ctx, base);
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
