// eternalsonata - ReXGlue Recompiled Project
//
// The guest's render and sampler state, decoded.
//
// The obvious way to get this is to hook the nine SetRenderState_* setters and
// the three SetSamplerState_* ones the config names. That is the wrong shape,
// and not only because it is more code: the named setters do not cover the
// state a host pipeline needs. There is no named setter for blending, for the
// depth compare function, for depth writes or for the colour write mask, and
// there is no list of what the block is missing -- an API this title reaches
// through a path nobody has looked at would simply never be mirrored, silently.
//
// So this reads the *destination* instead. Every setter in the D3D block writes
// a Xenos context register shadow inside the device object and dirties a bit;
// the shadow is what the flush at 0x82266D08 hands the GPU, so it is the state
// the draw genuinely runs with, whichever setter last touched it and whether or
// not that setter has a name. The dirty-bit map in eternalsonata_config.toml
// gives the shadow bases, and two of them are anchored against setters that
// were decompiled: cull mode writes device+10440 (0x2205 PA_SU_SC_MODE_CNTL)
// and alpha ref writes device+10372 (0x210E RB_ALPHA_REF).
//
// Sampler state is the same argument from the other side. The three setters
// patch fields inside the texture fetch constant at device+1024+24*stage rather
// than writing a register, so the fetch constant is the destination, and the
// mirror already reads it for the texture itself. Every tfetch in this title
// asks for mag, min and mip "from the fetch constant" (filter 3) and aniso 7,
// which is exactly what makes reading it here sufficient.
//
// Field positions come from xenia's registers.h and xenos.h, which are accurate
// down to bit offsets; the enum values below are xenia's, not D3D9's.

#pragma once

#include <cstdint>

namespace eternalsonata {

// Xenos CompareFunction, used by both the depth test and the alpha test.
enum class GuestCompare : uint32_t {
  kNever = 0,
  kLess = 1,
  kEqual = 2,
  kLessEqual = 3,
  kGreater = 4,
  kNotEqual = 5,
  kGreaterEqual = 6,
  kAlways = 7,
};

// What a host pipeline state object needs, decoded from the register shadows.
// The raw dwords are kept as well, because they are what the pipeline cache
// keys on: comparing the register the hardware would have received is both
// cheaper and stricter than comparing every decoded field.
struct GuestRenderState {
  // RB_DEPTHCONTROL (0x2200, shadow device+10420).
  uint32_t depth_control = 0;
  bool depth_enabled = false;
  bool depth_write = false;
  bool stencil_enabled = false;
  GuestCompare depth_func = GuestCompare::kAlways;

  // PA_SU_SC_MODE_CNTL (0x2205, shadow device+10440).
  uint32_t mode_cntl = 0;
  bool cull_front = false;
  bool cull_back = false;
  bool front_is_clockwise = false;

  // RB_BLENDCONTROL0 (0x2201, shadow device+10424) and RB_COLOR_MASK (0x2104,
  // shadow device+10332). Only target 0 is decoded: the resolve traffic shows
  // this title only ever renders to colour target 0 and the depth stencil, and
  // the frame layer binds exactly one colour target for the same reason.
  uint32_t blend_control = 0;
  uint32_t color_mask = 0;
  bool blend_enabled = false;
  uint32_t write_mask = 0xF;  // RGBA bits for target 0

  // RB_COLORCONTROL (0x2202, shadow device+10428) and RB_ALPHA_REF (0x210E,
  // shadow device+10372). Alpha test is fixed function on the console and has
  // no host equivalent at all, so it is not part of a host pipeline: it is
  // handed to the pixel shader, which discards. See the alpha test cbuffer in
  // scripts/xenos_hlsl.py and its upload in native_renderer_draw.cpp.
  uint32_t color_control = 0;
  bool alpha_test_enabled = false;
  GuestCompare alpha_func = GuestCompare::kAlways;
  float alpha_ref = 0.0f;

  bool valid = false;
};

// Read the shadows out of the live device. `base` is the guest memory base that
// REX_LOAD_U32 needs in scope; `device` is the device object's guest address.
// The returned state is `valid == false` when there is no device yet, in which
// case the pipeline falls back to the defaults it used before this existed.
GuestRenderState ReadGuestRenderState(uint8_t* base, uint32_t device);

// Sampler state, out of a stage's texture fetch constant. Everything here is a
// field the three SetSamplerState_* setters write, plus the clamp modes, which
// no named setter writes but SetTexture does.
struct GuestSamplerState {
  // xenos::TextureFilter: 0 point, 1 linear, 2 basemap (mip only), 3 "use the
  // fetch constant", which cannot occur here because this *is* the fetch
  // constant.
  uint32_t min_filter = 0;
  uint32_t mag_filter = 0;
  uint32_t mip_filter = 0;

  // xenos::ClampMode, one per axis.
  uint32_t clamp_x = 0;
  uint32_t clamp_y = 0;
  uint32_t clamp_z = 0;

  // xenos::AnisoFilter: 0 disabled, 1..5 give 1x..16x, 7 "use the fetch
  // constant" which again cannot occur here.
  uint32_t aniso = 0;

  // A 10 bit signed field with 5 fractional bits, so this is the value already
  // divided by 32.
  float lod_bias = 0.0f;

  // xenos::BorderColor. Only meaningful under a clamp-to-border mode.
  uint32_t border_color = 0;
};

GuestSamplerState DecodeSamplerState(const uint32_t words[6]);

// A value that is equal exactly when two sampler states would produce the same
// host sampler, which is what the sampler cache is keyed on.
uint64_t GuestSamplerKey(const GuestSamplerState& state);

}  // namespace eternalsonata
