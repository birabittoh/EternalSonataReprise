// eternalsonata - ReXGlue Recompiled Project
//
// See native_renderer_state.h. Decoding only: nothing here touches the backend.

#include "native_renderer_state.h"

#include <cstring>

#include "generated/eternalsonata_init.h"
#include "native_renderer_d3d.h"

namespace eternalsonata {
namespace {

// The context register shadows, from the dirty state map in
// eternalsonata_config.toml. Each block is contiguous and ascending in register
// order, so a register's shadow offset is base + 4 * (reg - first_reg).
//
// Verified anchors, both from decompiled setters: PA_SU_SC_MODE_CNTL (0x2205)
// is device+10440, which is 10420 + 4*5, and RB_ALPHA_REF (0x210E) is
// device+10372, which is 10316 + 4*14.
constexpr uint32_t kShadow2200 = 10420;  // 0x2200..0x220B
constexpr uint32_t kShadow2100 = 10316;  // 0x2100..0x2114
constexpr uint32_t kShadow2180 = 10400;  // 0x2180..0x2184
constexpr uint32_t kShadow2280 = 10468;  // 0x2280..0x2294

constexpr uint32_t Reg2200(uint32_t reg) { return kShadow2200 + 4 * (reg - 0x2200); }
constexpr uint32_t Reg2100(uint32_t reg) { return kShadow2100 + 4 * (reg - 0x2100); }
constexpr uint32_t Reg2180(uint32_t reg) { return kShadow2180 + 4 * (reg - 0x2180); }
constexpr uint32_t Reg2280(uint32_t reg) { return kShadow2280 + 4 * (reg - 0x2280); }

}  // namespace

GuestRenderState ReadGuestRenderState(uint8_t* base, uint32_t device) {
  GuestRenderState state;
  if (base == nullptr || device == 0)
    return state;

  state.depth_control = REX_LOAD_U32(device + Reg2200(0x2200));  // RB_DEPTHCONTROL
  state.blend_control = REX_LOAD_U32(device + Reg2200(0x2201));  // RB_BLENDCONTROL0
  state.color_control = REX_LOAD_U32(device + Reg2200(0x2202));  // RB_COLORCONTROL
  state.mode_cntl = REX_LOAD_U32(device + Reg2200(0x2205));      // PA_SU_SC_MODE_CNTL
  state.color_mask = REX_LOAD_U32(device + Reg2100(0x2104));     // RB_COLOR_MASK

  const uint32_t alpha_ref_bits = REX_LOAD_U32(device + Reg2100(0x210E));  // RB_ALPHA_REF
  std::memcpy(&state.alpha_ref, &alpha_ref_bits, sizeof(float));

  // RB_DEPTHCONTROL: stencil_enable +0, z_enable +1, z_write_enable +2,
  // zfunc +4 (3 bits).
  state.stencil_enabled = (state.depth_control & 1u) != 0;
  state.depth_enabled = ((state.depth_control >> 1) & 1u) != 0;
  state.depth_write = ((state.depth_control >> 2) & 1u) != 0;
  state.depth_func = GuestCompare((state.depth_control >> 4) & 7u);

  // RB_DEPTHCONTROL again, the stencil half: backface_enable +7, then per face
  // func +8, fail +11, zpass +14, zfail +17, and the back face's four at +20.
  state.stencil_backface_enabled = ((state.depth_control >> 7) & 1u) != 0;
  state.stencil_func = GuestCompare((state.depth_control >> 8) & 7u);
  state.stencil_fail_op = GuestStencilOp((state.depth_control >> 11) & 7u);
  state.stencil_zpass_op = GuestStencilOp((state.depth_control >> 14) & 7u);
  state.stencil_zfail_op = GuestStencilOp((state.depth_control >> 17) & 7u);
  if (state.stencil_backface_enabled) {
    state.stencil_func_bf = GuestCompare((state.depth_control >> 20) & 7u);
    state.stencil_fail_op_bf = GuestStencilOp((state.depth_control >> 23) & 7u);
    state.stencil_zpass_op_bf = GuestStencilOp((state.depth_control >> 26) & 7u);
    state.stencil_zfail_op_bf = GuestStencilOp((state.depth_control >> 29) & 7u);
  } else {
    state.stencil_func_bf = state.stencil_func;
    state.stencil_fail_op_bf = state.stencil_fail_op;
    state.stencil_zpass_op_bf = state.stencil_zpass_op;
    state.stencil_zfail_op_bf = state.stencil_zfail_op;
  }

  // RB_STENCILREFMASK: ref +0, read mask +8, write mask +16, a byte each.
  state.stencil_ref_mask = REX_LOAD_U32(device + Reg2100(0x210D));
  state.stencil_ref = state.stencil_ref_mask & 0xFFu;
  state.stencil_read_mask = (state.stencil_ref_mask >> 8) & 0xFFu;
  state.stencil_write_mask = (state.stencil_ref_mask >> 16) & 0xFFu;

  // PA_SU_SC_MODE_CNTL: cull_front +0, cull_back +1, face +2 where 0 means
  // front is counter clockwise and 1 means clockwise.
  state.cull_front = (state.mode_cntl & 1u) != 0;
  state.cull_back = ((state.mode_cntl >> 1) & 1u) != 0;
  state.front_is_clockwise = ((state.mode_cntl >> 2) & 1u) != 0;

  // RB_BLENDCONTROL: color_srcblend +0 (5 bits), color_comb_fcn +5 (3),
  // color_destblend +8 (5), then the alpha trio at +16, +21 and +24. There is
  // no enable bit: the hardware always blends, and "off" is spelled as
  // ONE * src + ZERO * dst with an ADD on both halves. Testing for that spelling
  // is how xenia's own pipeline cache decides whether to enable the host blend
  // unit, and it is also exactly Plume's RenderBlendDesc::Copy().
  const uint32_t src = state.blend_control & 0x1Fu;
  const uint32_t op = (state.blend_control >> 5) & 7u;
  const uint32_t dst = (state.blend_control >> 8) & 0x1Fu;
  const uint32_t src_a = (state.blend_control >> 16) & 0x1Fu;
  const uint32_t op_a = (state.blend_control >> 21) & 7u;
  const uint32_t dst_a = (state.blend_control >> 24) & 0x1Fu;
  state.blend_enabled = !(src == 1 && dst == 0 && op == 0 && src_a == 1 && dst_a == 0 && op_a == 0);

  // RB_COLOR_MASK: four bits per target, RGBA from bit 0.
  state.write_mask = state.color_mask & 0xFu;

  // RB_COLORCONTROL: alpha_func +0 (3 bits), alpha_test_enable +3.
  state.alpha_func = GuestCompare(state.color_control & 7u);
  state.alpha_test_enabled = ((state.color_control >> 3) & 1u) != 0;

  // SQ_PROGRAM_CNTL: param_gen +18. SQ_CONTEXT_MISC: param_gen_pos +8, a byte.
  const uint32_t program_cntl = REX_LOAD_U32(device + Reg2180(0x2180));
  const uint32_t context_misc = REX_LOAD_U32(device + Reg2180(0x2181));
  state.param_gen_enabled = ((program_cntl >> 18) & 1u) != 0;
  state.param_gen_pos = (context_misc >> 8) & 0xFFu;

  // PA_SU_POINT_SIZE: height +0, width +16, each a half extent in 12.4 fixed
  // point. Doubled back to a diameter so the geometry shader's one multiply by
  // the reciprocal viewport extent produces a clip space radius.
  const uint32_t point_size = REX_LOAD_U32(device + Reg2280(0x2280));
  state.point_diameter_y = float(point_size & 0xFFFFu) * (2.0f / 16.0f);
  state.point_diameter_x = float((point_size >> 16) & 0xFFFFu) * (2.0f / 16.0f);

  state.valid = true;
  return state;
}

GuestSamplerState DecodeSamplerState(const uint32_t words[6]) {
  GuestSamplerState state;

  // dword_0: clamp_x +10, clamp_y +13, clamp_z +16, three bits each.
  state.clamp_x = (words[0] >> 10) & 7u;
  state.clamp_y = (words[0] >> 13) & 7u;
  state.clamp_z = (words[0] >> 16) & 7u;

  // dword_3: mag_filter +19, min_filter +21, mip_filter +23, two bits each,
  // aniso_filter +25, three bits. These are the fields the min and mag filter
  // setters read-modify-write, which is the cross check on the offsets.
  state.mag_filter = (words[3] >> 19) & 3u;
  state.min_filter = (words[3] >> 21) & 3u;
  state.mip_filter = (words[3] >> 23) & 3u;
  state.aniso = (words[3] >> 25) & 7u;

  // dword_4: lod_bias +12, a 10 bit signed field with 5 fractional bits, which
  // is the field 0x8225AF40 writes as the argument times 32.
  const uint32_t raw_bias = (words[4] >> 12) & 0x3FFu;
  const int32_t bias = (raw_bias & 0x200u) != 0 ? int32_t(raw_bias) - 1024 : int32_t(raw_bias);
  state.lod_bias = float(bias) / 32.0f;

  // dword_5: border_color +0, two bits.
  state.border_color = words[5] & 3u;

  return state;
}

uint64_t GuestSamplerKey(const GuestSamplerState& state) {
  // The lod bias is quantised by the hardware field it came out of, so the raw
  // 10 bit value is an exact key rather than a float comparison.
  const uint64_t bias = uint64_t(int64_t(state.lod_bias * 32.0f) & 0x3FF);
  return uint64_t(state.min_filter) | (uint64_t(state.mag_filter) << 2) |
         (uint64_t(state.mip_filter) << 4) | (uint64_t(state.clamp_x) << 6) |
         (uint64_t(state.clamp_y) << 9) | (uint64_t(state.clamp_z) << 12) |
         (uint64_t(state.aniso) << 15) | (uint64_t(state.border_color) << 18) | (bias << 20);
}

}  // namespace eternalsonata
