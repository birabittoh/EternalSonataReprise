// eternalsonata - ReXGlue Recompiled Project
//
// See native_renderer_frame.h.

#include "native_renderer_frame.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "native_renderer_plume_internal.h"
#include "native_renderer_readback.h"

#ifdef _WIN32
#include "shaders/blitVert.hlsl.dxil.h"
#include "shaders/blitFrag.hlsl.dxil.h"
#endif
#include "shaders/blitVert.hlsl.spirv.h"
#include "shaders/blitFrag.hlsl.spirv.h"

#ifdef _WIN32
#include <windows.h>

#include <renderdoc_app.h>
#endif

namespace eternalsonata {
namespace {

using namespace plume;

// Colour targets are created in the swap chain's format so the present blit is
// a straight copy rather than a conversion pass. The guest's own formats are
// recorded in the key but not yet honoured; every colour surface this title
// creates is an 8888 variant, and the ones that are not will show up as a
// mismatch in the log rather than silently rendering wrong.
constexpr RenderFormat kColorFormat = RenderFormat::B8G8R8A8_UNORM;
constexpr RenderFormat kDepthFormat = RenderFormat::D32_FLOAT;

// A host image standing in for a region of EDRAM.
//
// Keyed by (base tile, size, multisampling, colour or depth) rather than by the
// surface object's address, because the guest creates and destroys surface
// objects freely over the same tiles and the image behind them is the same one.
struct GuestTarget {
  uint32_t base_tile = 0;
  uint32_t tiles = 0;  // EDRAM footprint, so two targets can be asked to overlap
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t msaa = 0;
  uint32_t guest_format = 0;
  bool depth = false;

  // What the host image is actually sized to, which is not the guest surface.
  //
  // The guest renders 720p through an EDRAM surface of 1280x384 and has the GPU
  // replay the command buffer once per band, shifting each band's geometry with
  // PA_SC_WINDOW_OFFSET. We see each draw once and execute it once, so there is
  // no replay to hang the second band off: the host renders the whole screen in
  // one pass and each band's resolve takes its own rows out of it. That only
  // works if the image is the size of the screen rather than of the band, which
  // is what BeginTiling's extent at device+13044/13048 gives.
  //
  // Part of the target's identity, so an extent that grows produces a new target
  // rather than a resize of one the previous frame may still be reading.
  uint32_t host_width = 0;
  uint32_t host_height = 0;

  std::unique_ptr<RenderTexture> texture;
  RenderTextureLayout layout = RenderTextureLayout::UNKNOWN;
};

// Framebuffers are per (colour, depth) pair, which is the granularity Plume
// binds at. There are only a handful of pairs in a frame, so a linear scan is
// cheaper than anything with a hash in it.
struct FramebufferEntry {
  const RenderTexture* color = nullptr;
  const RenderTexture* depth = nullptr;
  std::unique_ptr<RenderFramebuffer> framebuffer;
};

// A host image standing in for a *resolved* surface, i.e. the main memory
// texture a resolve wrote into. Keyed by guest base address, because that is
// what the game's own texture fetch constants carry and therefore how a later
// bind recognises this as something the frame produced rather than an asset.
struct ResolvedTexture {
  uint32_t address = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  std::unique_ptr<RenderTexture> texture;
  RenderTextureLayout layout = RenderTextureLayout::UNKNOWN;

  // The guest's own copy of this image, for the guest's own CPU to read. See
  // native_renderer_readback.h: the copy into this buffer is recorded into the
  // frame's command list and is therefore a frame behind by the time anything
  // can read it, which is the same delay the SDK's `readback_resolve=fast`
  // runs with. Persistently mapped, because the alternative is a map/unmap per
  // frame for a buffer whose contents are read from a fault handler.
  std::unique_ptr<RenderBuffer> readback;
  uint8_t* readback_mapped = nullptr;
  uint32_t readback_row_bytes = 0;

  // Whether a copy into it has been recorded in an earlier frame, i.e. whether
  // what it holds is an image rather than whatever the allocation came with.
  bool readback_written = false;
  uint64_t published_frame = ~0ull;
};

// Binds of an address that is a known resolve destination but at an extent that
// is not the one resolved there, i.e. memory that has stopped being a render
// target. Counted because it is the discriminator between the two, and a large
// number would mean the rule is too strict rather than that the game is
// recycling buffers.
uint64_t g_resolve_extent_mismatch = 0;

// Bumped once per frame, at the point the frame's command list is opened. Only
// ever compared for equality; the readback path uses it to offer a destination
// to the guest once a frame rather than once a resolve, since this title
// resolves the screen a band at a time.
uint64_t g_frame = 0;

std::vector<std::unique_ptr<GuestTarget>> g_targets;
std::vector<std::unique_ptr<ResolvedTexture>> g_resolved;
std::vector<FramebufferEntry> g_framebuffers;

// What the guest currently has bound, kept as the *surfaces* rather than as
// resolved host targets.
//
// Resolving them lazily is not a style choice. A host target's size depends on
// the tiling extent, which BeginTiling does not establish until some way into
// the first frame, so a target acquired before it and one acquired after it
// have different heights. Binding colour and depth at different moments would
// then pair a full screen colour attachment with a band-tall depth one, and
// Plume takes a framebuffer's size from the colour attachment alone without
// ever checking that the depth attachment agrees -- so it builds, and the first
// draw runs off the end of the depth buffer. Acquiring both at the point of use
// means they are always resolved against the same extent.
Surface g_bound_color_surface[d3d::kColorSurfaceCount];
bool g_bound_color_valid[d3d::kColorSurfaceCount] = {};
Surface g_bound_depth_surface;
bool g_bound_depth_valid = false;

GuestTarget* AcquireTarget(const Surface& surface, bool depth);

// See "The ripple probe" at the end of this namespace. Declared here because
// the first thing it watches is a target being created.
void RippleNoteTargetCreated(const GuestTarget* target);

GuestTarget* BoundColorTarget(uint32_t index) {
  if (index >= d3d::kColorSurfaceCount || !g_bound_color_valid[index])
    return nullptr;
  return AcquireTarget(g_bound_color_surface[index], false);
}

GuestTarget* BoundDepthTarget() {
  return g_bound_depth_valid ? AcquireTarget(g_bound_depth_surface, true) : nullptr;
}

// The framebuffer the command list currently has bound, so a run of draws into
// the same target does not re-set it. This is not just a saving: Plume's Vulkan
// backend ends the active render pass on every setFramebuffer, so setting the
// same one per draw would start and end a pass per draw.
//
// Command list state does not survive a begin(), which is why this is reset
// from there rather than at the present: the frame can also be flushed without
// presenting.
const RenderFramebuffer* g_bound_framebuffer = nullptr;

void BindFramebuffer(RenderCommandList* commands, RenderFramebuffer* framebuffer) {
  if (g_bound_framebuffer == framebuffer)
    return;
  commands->setFramebuffer(framebuffer);
  g_bound_framebuffer = framebuffer;
}

// Where the frame's image ends up: the destination of the most recent colour
// resolve. Sticky across frames, because a frame that resolves nothing is still
// showing whatever the last one produced.
ResolvedTexture* g_present_texture = nullptr;

uint64_t g_clears_applied = 0;
uint64_t g_clears_dropped = 0;

// Clears repeated onto a second host image over the same EDRAM. Zero would mean
// the title never aliases its render targets, and that the aliasing path in
// FrameClear is dead code; it is not zero here.
uint64_t g_clears_aliased = 0;
uint64_t g_resolves_copied = 0;
uint64_t g_resolves_dropped = 0;

// Resolves taken through the old band-relative reading, i.e. against a host
// target that does not cover the screen. Non-zero means BeginTiling's extent was
// not known when the target was created, and the vertical duplication that
// motivated all this would be back for those.
uint64_t g_resolves_banded = 0;
uint64_t g_composites = 0;
uint64_t g_composites_skipped = 0;
uint64_t g_attachment_mismatch = 0;
uint32_t g_attachment_mismatch_reported = 0;
uint32_t g_resolve_rect_reported = 0;
uint32_t g_resolve_examples = 0;
uint32_t g_clear_examples = 0;

// Transition an image into `layout`, remembering what it is in now. The current
// layout is passed by reference rather than looked up because both kinds of
// image here carry their own, and Plume wants the barrier only when the layout
// actually changes.
void Transition(RenderCommandList* commands, RenderTexture* texture,
                RenderTextureLayout& current, RenderBarrierStages stages,
                RenderTextureLayout layout) {
  if (current == layout)
    return;
  commands->barriers(stages, RenderTextureBarrier(texture, layout));
  current = layout;
}

uint32_t AlignUp(uint32_t value, uint32_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

// D3D12 wants a placed footprint's rows 256 byte aligned, and Plume takes the
// row width in texels rather than bytes.
constexpr uint32_t kReadbackRowAlignment = 256;

// Keep the guest's own copy of a resolve destination up to date.
//
// Two things happen here, in this order and for that reason. First the buffer's
// current contents -- which the GPU filled during a previous frame, since the
// present waits on its fence -- are offered to the readback layer, which decides
// whether the guest ever actually reads them and writes guest memory if so.
// Then this frame's copy is recorded over them.
//
// `box` is the region of the destination image the resolve just wrote, so a
// title that resolves the screen in bands accumulates the bands into one buffer
// exactly as it accumulates them into one texture.
void ReadbackRecordCopy(RenderCommandList* commands, ResolvedTexture* destination,
                        const TextureFetch& dest_fetch, uint8_t* memory_base,
                        const RenderBox& box) {
  if (!ReadbackEnabled() || destination->width == 0 || destination->height == 0)
    return;

  if (!destination->readback) {
    RenderDevice* device = PlumeDevice();
    if (device == nullptr)
      return;
    destination->readback_row_bytes = AlignUp(destination->width * 4, kReadbackRowAlignment);
    const uint64_t bytes = uint64_t(destination->readback_row_bytes) * destination->height;
    destination->readback = device->createBuffer(RenderBufferDesc::ReadbackBuffer(bytes));
    if (!destination->readback)
      return;
    destination->readback_mapped = static_cast<uint8_t*>(destination->readback->map());
    if (destination->readback_mapped == nullptr) {
      destination->readback.reset();
      return;
    }
  }

  // Published even on the very first resolve into this destination, when the
  // buffer holds nothing yet. That case is the save screenshot: a destination
  // resolved once and read by the guest in the same frame. Registering it here
  // is what arms its pages, and `pixels_ready` false is what tells the readback
  // layer that answering a read means making the GPU catch up first.
  if (destination->published_frame != g_frame) {
    destination->published_frame = g_frame;
    ReadbackPublish(memory_base, dest_fetch, destination->readback_mapped,
                    destination->readback_row_bytes, destination->height,
                    destination->readback_written, g_frame);
  }

  Transition(commands, destination->texture.get(), destination->layout, RenderBarrierStage::COPY,
             RenderTextureLayout::COPY_SOURCE);
  commands->copyTextureRegion(
      RenderTextureCopyLocation::PlacedFootprint(destination->readback.get(), kColorFormat,
                                                 destination->width, destination->height, 1,
                                                 destination->readback_row_bytes / 4),
      RenderTextureCopyLocation::Subresource(destination->texture.get()), uint32_t(box.left),
      uint32_t(box.top), 0, &box);
  destination->readback_written = true;
}

GuestTarget* AcquireTarget(const Surface& surface, bool depth) {
  // The host image covers the whole screen, not the band the EDRAM surface
  // holds; see GuestTarget::host_height. Before BeginTiling has ever run the
  // extent is zero and the surface is all there is to go on, which is correct
  // for a title that is not tiling.
  // Only a surface that is plausibly a *band of this tiling setup* is grown: the
  // widths have to agree and the surface has to be the shorter one. A target
  // that is neither, a small off screen one say, keeps its own size, which
  // matters because otherwise every render target in the title would be
  // inflated to the size of the screen.
  uint32_t extent_width = 0;
  uint32_t extent_height = 0;
  D3DTilingExtent(&extent_width, &extent_height);

  uint32_t host_width = surface.width;
  uint32_t host_height = surface.height;
  if (extent_width == surface.width && extent_height > surface.height)
    host_height = extent_height;

  for (auto& candidate : g_targets) {
    if (candidate->base_tile == surface.base_tile && candidate->width == surface.width &&
        candidate->height == surface.height && candidate->msaa == surface.msaa &&
        candidate->depth == depth && candidate->host_width == host_width &&
        candidate->host_height == host_height) {
      return candidate.get();
    }
  }

  RenderDevice* device = PlumeDevice();
  if (device == nullptr)
    return nullptr;

  auto target = std::make_unique<GuestTarget>();
  target->base_tile = surface.base_tile;
  target->tiles = surface.edram_tiles;
  target->width = surface.width;
  target->height = surface.height;
  target->msaa = surface.msaa;
  target->guest_format = surface.format;
  target->depth = depth;
  target->host_width = host_width;
  target->host_height = host_height;

  // Multisampling is recorded but the host image is single sampled for now.
  // Nothing draws yet, so the only thing this loses is edge quality on a target
  // that has no geometry in it; it becomes a real decision once draws land, and
  // it is logged so the choice is visible rather than assumed.
  target->texture =
      depth ? device->createTexture(
                  RenderTextureDesc::DepthTarget(host_width, host_height, kDepthFormat))
            : device->createTexture(
                  RenderTextureDesc::ColorTarget(host_width, host_height, kColorFormat));
  if (!target->texture)
    return nullptr;

  REXLOG_INFO(
      "native_renderer: host {} target for EDRAM tile {}: {}x{} for a {}x{} guest surface, msaa "
      "{} (guest format 0x{:X})",
      depth ? "depth" : "colour", surface.base_tile, host_width, host_height, surface.width,
      surface.height, surface.msaa, surface.format);

  g_targets.push_back(std::move(target));
  RippleNoteTargetCreated(g_targets.back().get());
  return g_targets.back().get();
}

RenderFramebuffer* AcquireFramebuffer(GuestTarget* color, GuestTarget* depth) {
  // Plume sizes a framebuffer from its colour attachment and never checks that
  // the depth attachment matches, so a mismatched pair builds happily and then
  // draws off the end of the smaller one. Refusing here turns that into a
  // dropped draw with a reason, which is the difference between a bug that is
  // findable and one that removes the device.
  if (color != nullptr && depth != nullptr &&
      (color->host_width != depth->host_width || color->host_height != depth->host_height)) {
    if (g_attachment_mismatch_reported < 8) {
      ++g_attachment_mismatch_reported;
      REXLOG_WARN(
          "native_renderer: colour target is {}x{} but depth is {}x{}; refusing the framebuffer "
          "rather than rendering past the end of one of them",
          color->host_width, color->host_height, depth->host_width, depth->host_height);
    }
    ++g_attachment_mismatch;
    depth = nullptr;
  }

  const RenderTexture* color_texture = color ? color->texture.get() : nullptr;
  const RenderTexture* depth_texture = depth ? depth->texture.get() : nullptr;
  if (color_texture == nullptr && depth_texture == nullptr)
    return nullptr;

  for (const auto& entry : g_framebuffers) {
    if (entry.color == color_texture && entry.depth == depth_texture)
      return entry.framebuffer.get();
  }

  RenderDevice* device = PlumeDevice();
  if (device == nullptr)
    return nullptr;

  RenderFramebufferDesc desc;
  desc.colorAttachments = color_texture ? &color_texture : nullptr;
  desc.colorAttachmentsCount = color_texture ? 1 : 0;
  desc.depthAttachment = depth_texture;

  FramebufferEntry entry;
  entry.color = color_texture;
  entry.depth = depth_texture;
  entry.framebuffer = device->createFramebuffer(desc);
  if (!entry.framebuffer)
    return nullptr;

  g_framebuffers.push_back(std::move(entry));
  return g_framebuffers.back().framebuffer.get();
}

// Do two targets stand for overlapping EDRAM?
//
// The console has one EDRAM, and the title uses the same tiles under two
// different surface descriptions in a single frame: a 1280x720 single sampled
// colour surface at tile 0 with its depth at tile 720, and a 1280x384 2x
// multisampled one, also at tile 0, whose depth is at tile 768. Both are the
// same bytes on hardware; here they are separate host images, because the size
// and sample count have to be part of a target's identity for anything else to
// work.
bool TargetsOverlap(const GuestTarget& a, const GuestTarget& b) {
  // A footprint of zero means the surface decode did not give one, in which case
  // the only honest answer is the exact same base.
  if (a.tiles == 0 || b.tiles == 0)
    return a.base_tile == b.base_tile;
  return a.base_tile < b.base_tile + b.tiles && b.base_tile < a.base_tile + a.tiles;
}

// Clear one (colour, depth) pair. Split out of FrameClear because the same
// clear has to be applied to more than one host image; see FrameClear.
bool ClearTargets(RenderCommandList* commands, GuestTarget* color, GuestTarget* depth,
                  uint32_t argb, bool clear_depth, bool clear_stencil, float z, uint32_t stencil,
                  uint32_t* out_width, uint32_t* out_height) {
  if (color) {
    Transition(commands, color->texture.get(), color->layout, RenderBarrierStage::GRAPHICS,
               RenderTextureLayout::COLOR_WRITE);
  }
  if (depth) {
    Transition(commands, depth->texture.get(), depth->layout, RenderBarrierStage::GRAPHICS,
               RenderTextureLayout::DEPTH_WRITE);
  }

  RenderFramebuffer* framebuffer = AcquireFramebuffer(color, depth);
  if (framebuffer == nullptr)
    return false;
  BindFramebuffer(commands, framebuffer);

  const uint32_t width = framebuffer->getWidth();
  const uint32_t height = framebuffer->getHeight();
  commands->setViewports(RenderViewport(0.0f, 0.0f, float(width), float(height)));
  commands->setScissors(RenderRect(0, 0, int32_t(width), int32_t(height)));

  if (color) {
    // D3DCOLOR is packed ARGB, most significant byte first.
    const float a = float((argb >> 24) & 0xFFu) / 255.0f;
    const float r = float((argb >> 16) & 0xFFu) / 255.0f;
    const float g = float((argb >> 8) & 0xFFu) / 255.0f;
    const float b = float(argb & 0xFFu) / 255.0f;
    commands->clearColor(0, RenderColor(r, g, b, a));
  }
  if (depth)
    commands->clearDepthStencil(clear_depth, clear_stencil, z, stencil);

  if (out_width != nullptr)
    *out_width = width;
  if (out_height != nullptr)
    *out_height = height;
  return true;
}

// --- The ripple probe ---
//
// The water's height field is a 64x64 ping-pong pair that the guest clears,
// renders into and resolves every frame, and the flicker is that field
// inverting with period 2. A frame capture found five host 64x64 images where
// there should be three, with the guest's clears landing on two that nothing
// ever samples, so the question this answers is whether one guest address keeps
// one host image: whether the clear, the draw target, the resolve destination
// and the later bind are the same image, and whether that image is the same one
// from frame to frame.
//
// Every event on a surface of exactly ES_RIPPLE_SIZE (default 64) square is
// recorded in the order it happens, and one line per frame is logged. Host
// textures get small ids on first sight, because what matters is whether an id
// alternates rather than what the pointer is; the pointer is logged once, when
// the id is handed out.
//
// Off unless ES_RIPPLE_PROBE is set; its value is how many frames to log
// (default 120), following the ES_DUMP_PS convention.
uint32_t RippleProbeFrames() {
  static const uint32_t frames = [] {
    const char* value = std::getenv("ES_RIPPLE_PROBE");
    if (value == nullptr)
      return 0u;
    const int parsed = std::atoi(value);
    return parsed > 0 ? uint32_t(parsed) : 120u;
  }();
  return frames;
}

uint32_t RippleProbeExtent() {
  static const uint32_t extent = [] {
    const char* value = std::getenv("ES_RIPPLE_SIZE");
    const int parsed = value != nullptr ? std::atoi(value) : 0;
    return parsed > 0 ? uint32_t(parsed) : 64u;
  }();
  return extent;
}

uint32_t g_ripple_frames_logged = 0;
std::string g_ripple_line;
uint32_t g_ripple_events = 0;
const GuestTarget* g_ripple_last_target = nullptr;
std::vector<const void*> g_ripple_texture_ids;

// A frame's worth of events. High enough that the ripple traffic never reaches
// it, low enough that a mistake in the filter cannot fill the log.
constexpr uint32_t kMaxRippleEvents = 64;

bool RippleProbeActive() { return g_ripple_frames_logged < RippleProbeFrames(); }

bool RippleProbeWatches(uint32_t width, uint32_t height) {
  const uint32_t extent = RippleProbeExtent();
  return width == extent && height == extent;
}

int RippleTextureId(const void* texture) {
  if (texture == nullptr)
    return -1;
  for (size_t i = 0; i < g_ripple_texture_ids.size(); ++i) {
    if (g_ripple_texture_ids[i] == texture)
      return int(i);
  }
  g_ripple_texture_ids.push_back(texture);
  const int id = int(g_ripple_texture_ids.size()) - 1;
  REXLOG_WARN("native_renderer: ripple probe host texture T{} is {:016X}", id, uintptr_t(texture));
  return id;
}

void RippleAppend(const char* text) {
  if (g_ripple_events >= kMaxRippleEvents)
    return;
  ++g_ripple_events;
  g_ripple_line += " | ";
  g_ripple_line += text;
}

void RippleNoteTargetCreated(const GuestTarget* target) {
  if (!RippleProbeActive() || target == nullptr)
    return;
  if (!RippleProbeWatches(target->host_width, target->host_height))
    return;
  char text[192];
  std::snprintf(text, sizeof(text), "new-%s-target tile=%u T%d %ux%u msaa=%u fmt=0x%X",
                target->depth ? "depth" : "colour", target->base_tile,
                RippleTextureId(target->texture.get()), target->width, target->height,
                target->msaa, target->guest_format);
  RippleAppend(text);
}

void RippleNoteClear(const GuestTarget* target, bool aliased, uint32_t argb) {
  if (!RippleProbeActive() || target == nullptr)
    return;
  if (!RippleProbeWatches(target->host_width, target->host_height))
    return;
  char text[192];
  std::snprintf(text, sizeof(text), "%s tile=%u T%d argb=%08X", aliased ? "alias-clear" : "clear",
                target->base_tile, RippleTextureId(target->texture.get()), argb);
  RippleAppend(text);
}

// Noted only when it changes, because this runs per draw.
void RippleNoteDrawTarget(const GuestTarget* target) {
  if (!RippleProbeActive() || target == nullptr || target == g_ripple_last_target)
    return;
  g_ripple_last_target = target;
  if (!RippleProbeWatches(target->host_width, target->host_height))
    return;
  char text[192];
  std::snprintf(text, sizeof(text), "draw-into tile=%u T%d", target->base_tile,
                RippleTextureId(target->texture.get()));
  RippleAppend(text);
}

void RippleNoteResolve(const GuestTarget* source, const ResolvedTexture* destination,
                       bool created) {
  if (!RippleProbeActive() || source == nullptr || destination == nullptr)
    return;
  if (!RippleProbeWatches(destination->width, destination->height))
    return;
  char text[192];
  std::snprintf(text, sizeof(text), "resolve tile=%u T%d -> 0x%08X T%d%s", source->base_tile,
                RippleTextureId(source->texture.get()), destination->address,
                RippleTextureId(destination->texture.get()), created ? " (new)" : "");
  RippleAppend(text);
}

// `destination` is null for a bind that found nothing, which is the interesting
// case: the address falls through to the texture mirror and is decoded out of
// guest memory as if it were an asset.
void RippleNoteBind(uint32_t address, uint32_t width, uint32_t height,
                    const ResolvedTexture* destination, bool extent_mismatch) {
  if (!RippleProbeActive())
    return;
  if (!RippleProbeWatches(width, height))
    return;
  char text[192];
  if (destination != nullptr) {
    std::snprintf(text, sizeof(text), "bind 0x%08X -> T%d", address,
                  RippleTextureId(destination->texture.get()));
  } else {
    std::snprintf(text, sizeof(text), "bind 0x%08X -> %s", address,
                  extent_mismatch ? "EXTENT MISMATCH" : "MISS");
  }
  RippleAppend(text);
}

// F11: capture two consecutive frames.
//
// RenderDoc's own F12 takes one frame per press, which cannot show an
// alternation: the ripple simulation's defect is a period 2 flip, so a single
// frame looks like a still image of noise and says nothing about what changes
// between frames. TriggerMultiFrameCapture writes N consecutive frames as N
// separate .rdc files with no input timing involved.
//
// The obvious call for this is TriggerMultiFrameCapture, which writes N
// consecutive frames as N separate files. It needs API 1.1.0 and the SDK ships
// a renderdoc_app.h that stops at 1.0.1, so this brackets the two frames with
// StartFrameCapture/EndFrameCapture instead: both are in 1.0.0, and the result
// is one .rdc holding two frames rather than two files. That suits a diff.
//
// This runs at the present, so the capture opens on the frame *after* the one
// the key was pressed during, and closes two presents later.
//
// RenderDoc injects renderdoc.dll into the process before it starts, so
// GetModuleHandle is enough and this never loads anything: outside RenderDoc
// the handle is null and the key does nothing.
void PollCaptureKey() {
#ifdef _WIN32
  static RENDERDOC_API_1_0_0* api = [] () -> RENDERDOC_API_1_0_0* {
    HMODULE module = GetModuleHandleA("renderdoc.dll");
    if (module == nullptr)
      return nullptr;
    auto get_api = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI"));
    if (get_api == nullptr)
      return nullptr;
    RENDERDOC_API_1_0_0* found = nullptr;
    if (get_api(eRENDERDOC_API_Version_1_0_0, reinterpret_cast<void**>(&found)) != 1)
      return nullptr;
    REXLOG_INFO("native_renderer: RenderDoc in-application API ready, F11 captures two frames");
    return found;
  }();
  if (api == nullptr)
    return;

  // Counts presents left in an open capture; 0 means no capture is running.
  static int frames_remaining = 0;
  if (frames_remaining > 0) {
    if (--frames_remaining == 0) {
      const uint32_t ok = api->EndFrameCapture(nullptr, nullptr);
      REXLOG_WARN("native_renderer: F11 capture closed ({})", ok ? "written" : "FAILED");
    }
    return;
  }

  // Edge triggered. GetAsyncKeyState's low bit is unreliable when something
  // else polls the same key, so the previous state is tracked here instead;
  // without this a single press would retrigger on every frame it is held.
  const bool down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
  static bool was_down = false;
  const bool pressed = down && !was_down;
  was_down = down;
  if (!pressed)
    return;

  api->StartFrameCapture(nullptr, nullptr);
  frames_remaining = 2;
  REXLOG_WARN("native_renderer: F11 pressed, capturing the next 2 frames");
#endif
}

void RippleProbeFlush() {
  if (!RippleProbeActive())
    return;
  ++g_ripple_frames_logged;
  REXLOG_WARN("native_renderer: ripple probe frame {}{}", g_ripple_frames_logged,
              g_ripple_line.empty() ? std::string(" | no 64x64 traffic") : g_ripple_line);
  g_ripple_line.clear();
  g_ripple_events = 0;
  g_ripple_last_target = nullptr;
}

}  // namespace

void FrameSetColorSurface(uint32_t index, const Surface* surface) {
  if (index >= d3d::kColorSurfaceCount)
    return;
  g_bound_color_valid[index] = surface != nullptr;
  if (surface != nullptr)
    g_bound_color_surface[index] = *surface;
}

void FrameSetDepthSurface(const Surface* surface) {
  g_bound_depth_valid = surface != nullptr;
  if (surface != nullptr)
    g_bound_depth_surface = *surface;
}

void FrameClear(uint32_t flags, uint32_t argb, float z, uint32_t stencil) {
  // D3DCLEAR_TARGET / _ZBUFFER / _STENCIL.
  const bool clear_color = (flags & 0x1u) != 0;
  const bool clear_depth = (flags & 0x2u) != 0;
  const bool clear_stencil = (flags & 0x4u) != 0;

  GuestTarget* color = clear_color ? BoundColorTarget(0) : nullptr;
  GuestTarget* depth = (clear_depth || clear_stencil) ? BoundDepthTarget() : nullptr;
  if (color == nullptr && depth == nullptr) {
    ++g_clears_dropped;
    return;
  }

  RenderCommandList* commands = PlumeGuestCommands();
  if (commands == nullptr) {
    ++g_clears_dropped;
    return;
  }

  uint32_t width = 0;
  uint32_t height = 0;
  if (!ClearTargets(commands, color, depth, argb, clear_depth, clear_stencil, z, stencil, &width,
                    &height)) {
    ++g_clears_dropped;
    return;
  }
  RippleNoteClear(color, false, argb);

  // And again for every other host image standing over the same EDRAM.
  //
  // A clear writes bytes, and on the console those bytes are visible through
  // whichever surface description is bound next. This title relies on that: the
  // 720p single sampled pair at tiles 0 and 720 is what BeginTiling's clear
  // reaches, while the scene is drawn through the 2x multisampled 1280x384 pair
  // at tiles 0 and 768. Without this the scene's depth buffer is never cleared,
  // holds zero, and every draw after the first fails LESS_EQUAL against it --
  // which is what left the second intro logo white while its geometry, shaders
  // and textures were all correct.
  //
  // Clearing the whole aliasing image rather than the intersection is coarser
  // than the hardware: the two footprints here overlap by 87% and the remainder
  // is not addressed by the surface that is about to be drawn through. A partial
  // clear would need a real EDRAM model, which is exactly what this renderer is
  // built to avoid.
  for (auto& candidate : g_targets) {
    GuestTarget* other = candidate.get();
    const bool alias_color =
        color != nullptr && other != color && !other->depth && TargetsOverlap(*other, *color);
    const bool alias_depth =
        depth != nullptr && other != depth && other->depth && TargetsOverlap(*other, *depth);
    if (!alias_color && !alias_depth)
      continue;
    if (ClearTargets(commands, alias_color ? other : nullptr, alias_depth ? other : nullptr, argb,
                     clear_depth, clear_stencil, z, stencil, nullptr, nullptr)) {
      ++g_clears_aliased;
      RippleNoteClear(alias_color ? other : nullptr, true, argb);
    }
  }

  // The colours themselves, because with no draws yet the clear *is* the frame:
  // a black screen is the correct output if this is what the guest asks for,
  // and the only way to tell that apart from the copy not working is to look at
  // the value.
  if (g_clear_examples < 12) {
    ++g_clear_examples;
    REXLOG_INFO(
        "native_renderer: clear flags 0x{:X} colour 0x{:08X} z {} on EDRAM tile {} ({}x{})",
        flags, argb, z, color ? color->base_tile : (depth ? depth->base_tile : 0),
        width, height);
  }

  ++g_clears_applied;
}

void FrameResolve(uint32_t source, uint8_t* memory_base, const TextureFetch& dest_fetch,
                  int32_t src_x1, int32_t src_y1, int32_t src_x2, int32_t src_y2, int32_t dest_x,
                  int32_t dest_y) {
  const uint32_t dest_address = dest_fetch.base_address;
  const uint32_t dest_width = dest_fetch.width;
  const uint32_t dest_height = dest_fetch.height;

  // 4 is the depth stencil.
  const bool is_depth = source == d3d::kColorSurfaceCount;
  if (source > d3d::kColorSurfaceCount)
    return;
  GuestTarget* target = is_depth ? BoundDepthTarget() : BoundColorTarget(source);
  if (target == nullptr || dest_address == 0 || dest_width == 0 || dest_height == 0) {
    ++g_resolves_dropped;
    return;
  }

  // The rectangle is in screen space, not in the guest surface's own space,
  // because this title renders 720p through EDRAM in horizontal bands: the
  // colour surface holds 1280x384 and the second band's resolve asks for rows
  // 352..720.
  //
  // On the console those rows are inside the surface, because the GPU replayed
  // the draws with PA_SC_WINDOW_OFFSET shifting the band's geometry down into
  // it, so the band's origin in the surface is the rectangle minus the
  // destination point. That subtraction is precisely the window offset, and it
  // is what this used to do.
  //
  // The host does not replay and does not apply the window offset: it renders
  // the whole screen once, into a target sized to the tiling extent rather than
  // to the band (see GuestTarget::host_height). So the rectangle is already in
  // the target's space and the subtraction has nothing to undo -- doing it
  // anyway is what put both bands at the top of the image and made the screen
  // a vertically duplicated pair.
  //
  // The old reading is kept as the fallback for a target that is not full
  // screen, which is what a title that never calls BeginTiling would produce,
  // and the two agree there anyway since such a resolve has a zero destination
  // point.
  //
  // A resolve with no rectangle passes 0x7FFFFFFF and means "the whole
  // surface", so it is not evidence that the target is too small.
  const bool unbounded_rect = src_x2 == 0x7FFFFFFF;
  const bool host_covers_screen =
      unbounded_rect ||
      (int32_t(target->host_height) >= src_y2 && int32_t(target->host_width) >= src_x2);
  const int32_t local_x = host_covers_screen ? src_x1 : src_x1 - dest_x;
  const int32_t local_y = host_covers_screen ? src_y1 : src_y1 - dest_y;
  if (!host_covers_screen)
    ++g_resolves_banded;
  int32_t x1 = local_x < 0 ? 0 : local_x;
  int32_t y1 = local_y < 0 ? 0 : local_y;
  int32_t x2 = x1 + (src_x2 - src_x1);
  int32_t y2 = y1 + (src_y2 - src_y1);

  // Clamp against the source surface and against what is left of the
  // destination from the point the copy starts at.
  const int32_t source_right = int32_t(target->host_width);
  const int32_t source_bottom = int32_t(target->host_height);
  const int32_t dest_right = int32_t(dest_width) - (dest_x < 0 ? 0 : dest_x) + x1;
  const int32_t dest_bottom = int32_t(dest_height) - (dest_y < 0 ? 0 : dest_y) + y1;
  const int32_t limit_right = source_right < dest_right ? source_right : dest_right;
  const int32_t limit_bottom = source_bottom < dest_bottom ? source_bottom : dest_bottom;
  const bool clamped = x2 > limit_right || y2 > limit_bottom;
  if (x2 > limit_right)
    x2 = limit_right;
  if (y2 > limit_bottom)
    y2 = limit_bottom;

  if (x2 <= x1 || y2 <= y1) {
    ++g_resolves_dropped;
    if (g_resolve_rect_reported < 8) {
      ++g_resolve_rect_reported;
      REXLOG_WARN(
          "native_renderer: resolve rectangle ({},{})..({},{}) to ({},{}) is empty against a "
          "{}x{} source. Either the rect is not the third argument or D3DRECT is not four "
          "int32s.",
          src_x1, src_y1, src_x2, src_y2, dest_x, dest_y, target->host_width,
          target->host_height);
    }
    return;
  }
  // A resolve with no rectangle means the whole surface, so being cut down to
  // the surface is the point rather than a surprise; only a rectangle the guest
  // actually asked for is worth a warning.
  if (clamped && !unbounded_rect && g_resolve_rect_reported < 8) {
    ++g_resolve_rect_reported;
    REXLOG_WARN(
        "native_renderer: resolve rectangle ({},{})..({},{}) to ({},{}) does not fit its {}x{} "
        "source or its {}x{} destination and was clamped",
        src_x1, src_y1, src_x2, src_y2, dest_x, dest_y, target->host_width, target->host_height,
        dest_width, dest_height);
  }

  ResolvedTexture* destination = nullptr;
  bool destination_created = false;
  for (auto& candidate : g_resolved) {
    if (candidate->address == dest_address) {
      destination = candidate.get();
      break;
    }
  }

  RenderDevice* device = PlumeDevice();
  if (destination == nullptr && device != nullptr) {
    auto created = std::make_unique<ResolvedTexture>();
    created->address = dest_address;
    created->width = dest_width;
    created->height = dest_height;
    // A colour target rather than a plain texture: the copy needs it as a copy
    // destination now, and a later draw needs to sample it, but the guest also
    // resolves into textures it goes on to render into. Depth has no such case
    // -- nothing renders into a resolved depth mask -- so a plain texture is
    // enough, in the R32_FLOAT view of the same bits D32_FLOAT holds.
    created->texture =
        is_depth ? device->createTexture(
                       RenderTextureDesc::Texture2D(dest_width, dest_height, 1, RenderFormat::R32_FLOAT))
                 : device->createTexture(
                       RenderTextureDesc::ColorTarget(dest_width, dest_height, kColorFormat));
    if (created->texture) {
      REXLOG_INFO("native_renderer: resolve destination 0x{:08X} is {}x{}", dest_address,
                  dest_width, dest_height);
      g_resolved.push_back(std::move(created));
      destination = g_resolved.back().get();
      destination_created = true;
    }
  }
  if (destination == nullptr) {
    ++g_resolves_dropped;
    return;
  }

  RenderCommandList* commands = PlumeGuestCommands();
  if (commands == nullptr) {
    ++g_resolves_dropped;
    return;
  }

  Transition(commands, target->texture.get(), target->layout, RenderBarrierStage::COPY,
             RenderTextureLayout::COPY_SOURCE);
  Transition(commands, destination->texture.get(), destination->layout, RenderBarrierStage::COPY,
             RenderTextureLayout::COPY_DEST);

  // Where the copy lands, carrying over however far the source origin had to be
  // clamped so the two stay in step.
  const int32_t place_x = (dest_x < 0 ? 0 : dest_x) + (x1 - local_x);
  const int32_t place_y = (dest_y < 0 ? 0 : dest_y) + (y1 - local_y);

  const RenderBox box(x1, y1, x2, y2);
  commands->copyTextureRegion(RenderTextureCopyLocation::Subresource(destination->texture.get()),
                              RenderTextureCopyLocation::Subresource(target->texture.get()),
                              uint32_t(place_x), uint32_t(place_y), 0, &box);

  // And the same region again, out to a buffer the guest's own CPU can be given
  // if it ever asks. The region is the one just written, in the destination
  // image's coordinates rather than the source surface's. Not for depth: see
  // the comment above on why a depth resolve never sets this up.
  if (!is_depth) {
    const RenderBox readback_box(place_x, place_y, place_x + (x2 - x1), place_y + (y2 - y1));
    ReadbackRecordCopy(commands, destination, dest_fetch, memory_base, readback_box);
  }

  // Leave the destination in the layout a draw will sample it in.
  //
  // A resolve destination exists to be sampled: that is what
  // FrameResolveTextureByAddress hands it out for. The resolve itself needs it
  // as COPY_DEST and the readback copy then needs it as COPY_SOURCE, so without
  // this it is still in a copy layout when the next draw reads it. The
  // descriptor set does say SHADER_READ, but a descriptor declares what a
  // shader expects; only a barrier changes the resource state, and nothing
  // between here and the draw emits one.
  //
  // The symptom is asymmetric in a way worth recording, because it is what made
  // it hard to see: the present blit transitions its own source explicitly, so
  // the image on screen was always correct, while anything sampled back out of
  // a resolve in the same frame was read in the wrong state.
  Transition(commands, destination->texture.get(), destination->layout,
             RenderBarrierStage::GRAPHICS, RenderTextureLayout::SHADER_READ);

  // Small destinations are logged well past the general cap: those are the
  // thumbnails and the off screen effect buffers, and they are what the readback
  // path has to lay back out in guest memory byte for byte.
  if (g_resolve_examples < 12 || (dest_width <= 512 && g_resolve_examples < 64)) {
    ++g_resolve_examples;
    REXLOG_INFO(
        "native_renderer: resolved EDRAM tile {} ({},{})..({},{}) into 0x{:08X} at ({},{}) | "
        "requested ({},{})..({},{}) to ({},{}) | destination {}x{} pitch {} {} endian {} | source "
        "host {}x{}",
        target->base_tile, x1, y1, x2, y2, dest_address, place_x, place_y, src_x1, src_y1, src_x2,
        src_y2, dest_x, dest_y, dest_width, dest_height, dest_fetch.pitch,
        dest_fetch.tiled ? "tiled" : "linear", dest_fetch.endianness, target->host_width,
        target->host_height);
  }

  RippleNoteResolve(target, destination, destination_created);

  ++g_resolves_copied;
  // The present blit always wants the last colour resolve; a depth resolve
  // must not steal that slot from it.
  if (!is_depth)
    g_present_texture = destination;
}

void* FrameResolveTextureByAddress(uint32_t address, uint32_t width, uint32_t height) {
  if (address == 0)
    return nullptr;
  for (auto& candidate : g_resolved) {
    if (candidate->address != address)
      continue;

    // The extent has to agree, and this is not a formality. An address that was
    // once a resolve destination would otherwise be treated as one forever, so
    // if the guest frees that buffer and loads an ordinary texture into it, the
    // mirror would keep handing back the render target the scene before last
    // drew. A disagreement about the extent is the cheapest evidence that the
    // memory is no longer the image this owns; the caller falls through to
    // decoding it as an asset, which is what it now is.
    if (candidate->width != width || candidate->height != height) {
      ++g_resolve_extent_mismatch;
      RippleNoteBind(address, width, height, nullptr, true);
      return nullptr;
    }
    RippleNoteBind(address, width, height, candidate.get(), false);
    return candidate->texture.get();
  }
  RippleNoteBind(address, width, height, nullptr, false);
  return nullptr;
}

uint64_t FrameIndex() { return g_frame; }

void LogFrameSummary() {
  REXLOG_INFO(
      "native_renderer: frame targets={} framebuffers={} resolve destinations={} | clears "
      "applied={} dropped={} aliased={} | resolves copied={} dropped={} banded={} extent mismatch={} | "
      "attachment mismatch={} | "
      "composites={} skipped={} | presenting 0x{:08X} ({}x{})",
      g_targets.size(), g_framebuffers.size(), g_resolved.size(), g_clears_applied,
      g_clears_dropped, g_clears_aliased, g_resolves_copied, g_resolves_dropped, g_resolves_banded,
      g_resolve_extent_mismatch, g_attachment_mismatch, g_composites, g_composites_skipped,
      g_present_texture ? g_present_texture->address : 0,
      g_present_texture ? g_present_texture->width : 0,
      g_present_texture ? g_present_texture->height : 0);
}

RenderFramebuffer* FrameBindDrawTargets(RenderCommandList* commands, uint32_t* width,
                                        uint32_t* height) {
  // Colour target 0 and the depth stencil. The guest binds targets 1..3 in this
  // title only for the resolve source selector to name, and the pixel shaders
  // export a single colour (e0, all 128 of them), so one attachment is the whole
  // story rather than a simplification.
  GuestTarget* color = BoundColorTarget(0);
  GuestTarget* depth = BoundDepthTarget();
  if (color == nullptr && depth == nullptr)
    return nullptr;

  if (color) {
    Transition(commands, color->texture.get(), color->layout, RenderBarrierStage::GRAPHICS,
               RenderTextureLayout::COLOR_WRITE);
  }
  if (depth) {
    Transition(commands, depth->texture.get(), depth->layout, RenderBarrierStage::GRAPHICS,
               RenderTextureLayout::DEPTH_WRITE);
  }

  RenderFramebuffer* framebuffer = AcquireFramebuffer(color, depth);
  if (framebuffer == nullptr)
    return nullptr;

  BindFramebuffer(commands, framebuffer);
  RippleNoteDrawTarget(color);
  if (width)
    *width = framebuffer->getWidth();
  if (height)
    *height = framebuffer->getHeight();
  return framebuffer;
}

const void* FrameCurrentColorTexture() {
  GuestTarget* color = BoundColorTarget(0);
  return color != nullptr ? static_cast<const void*>(color->texture.get()) : nullptr;
}

// Not the place to count frames: the readback path can flush the frame's work
// mid frame, and the fresh command list that follows would look like a new one.
void FrameNotifyCommandListBegun() { g_bound_framebuffer = nullptr; }

// --- The present probe ---
//
// A full-screen flicker that persists on a still main menu is not a material
// bug: it is the frame as a whole differing between one present and the next.
// The per-swap summary samples this far too rarely to see an alternation, so
// this logs the identity of the presented image every frame, plus how much work
// went into the frame that produced it. What to look for is any column that
// ping-pongs between exactly two values while the scene is static.
//
// Off unless `ES_PRESENT_PROBE` is set; its value is how many frames to log
// (default 120), following the ES_DUMP_PS convention.
uint32_t PresentProbeFrames() {
  static const uint32_t frames = [] {
    const char* value = std::getenv("ES_PRESENT_PROBE");
    if (value == nullptr)
      return 0u;
    const int parsed = std::atoi(value);
    return parsed > 0 ? uint32_t(parsed) : 120u;
  }();
  return frames;
}
uint32_t g_present_probe_logged = 0;
uint64_t g_present_probe_last_resolves = 0;
uint64_t g_present_probe_last_clears = 0;

// --- The present blit ---
//
// The guest renders at a fixed 1280x720 and the window is any size at all, so
// the present is a textured draw rather than a copy: a copy cannot scale, and
// what it did instead was present the overlapping corner of the two, which read
// as the image sitting in the top left of a resized window.
//
// The scaling lives entirely in the viewport the draw is issued under, so
// stretching and letterboxing differ only in that rectangle. `present_letterbox`
// (the SDK's own cvar, default true) picks between them.
struct BlitResources {
  std::unique_ptr<RenderShader> vertex_shader;
  std::unique_ptr<RenderShader> pixel_shader;
  std::unique_ptr<RenderPipelineLayout> pipeline_layout;
  std::unique_ptr<RenderPipeline> pipeline;
  std::unique_ptr<RenderSampler> sampler;
  std::unique_ptr<RenderDescriptorSet> descriptor_set;

  // What the set currently points at. Rewriting a descriptor set that a
  // recorded draw still reads is trap 2 in the handoff; it is safe here only
  // because the present fence waits before the next frame records anything.
  const RenderTexture* bound_texture = nullptr;

  bool initialized = false;
  bool failed = false;
};

BlitResources g_blit;

bool EnsureBlitResources(RenderDevice* device) {
  if (g_blit.initialized)
    return true;
  if (g_blit.failed || device == nullptr)
    return false;

  RenderDescriptorRange ranges[] = {
      RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, 1),
      RenderDescriptorRange(RenderDescriptorRangeType::SAMPLER, 1, 1),
  };
  RenderDescriptorSetDesc descriptor_set_desc(ranges, 2);

  RenderPipelineLayoutDesc layout_desc;
  layout_desc.descriptorSetDescs = &descriptor_set_desc;
  layout_desc.descriptorSetDescsCount = 1;
  layout_desc.allowInputLayout = false;
  g_blit.pipeline_layout = device->createPipelineLayout(layout_desc);

  const RenderShaderFormat shader_format = PlumeShaderFormat();
#ifdef _WIN32
  if (shader_format == RenderShaderFormat::DXIL) {
    g_blit.vertex_shader =
        device->createShader(blitVertBlobDXIL, sizeof(blitVertBlobDXIL), "VSMain", shader_format);
    g_blit.pixel_shader =
        device->createShader(blitFragBlobDXIL, sizeof(blitFragBlobDXIL), "PSMain", shader_format);
  } else
#endif
      if (shader_format == RenderShaderFormat::SPIRV) {
    g_blit.vertex_shader =
        device->createShader(blitVertBlobSPIRV, sizeof(blitVertBlobSPIRV), "VSMain", shader_format);
    g_blit.pixel_shader =
        device->createShader(blitFragBlobSPIRV, sizeof(blitFragBlobSPIRV), "PSMain", shader_format);
  }

  RenderSamplerDesc sampler_desc;
  sampler_desc.minFilter = RenderFilter::LINEAR;
  sampler_desc.magFilter = RenderFilter::LINEAR;
  sampler_desc.mipmapMode = RenderMipmapMode::NEAREST;
  sampler_desc.addressU = RenderTextureAddressMode::CLAMP;
  sampler_desc.addressV = RenderTextureAddressMode::CLAMP;
  sampler_desc.addressW = RenderTextureAddressMode::CLAMP;
  g_blit.sampler = device->createSampler(sampler_desc);

  if (g_blit.vertex_shader && g_blit.pixel_shader && g_blit.pipeline_layout) {
    RenderGraphicsPipelineDesc pipeline_desc;
    pipeline_desc.pipelineLayout = g_blit.pipeline_layout.get();
    pipeline_desc.vertexShader = g_blit.vertex_shader.get();
    pipeline_desc.pixelShader = g_blit.pixel_shader.get();
    // The presented image's format, not the guest targets' one. They are the
    // same everywhere except Android; see PlumeSwapChainFormat.
    pipeline_desc.renderTargetFormat[0] = PlumeSwapChainFormat();
    pipeline_desc.renderTargetCount = 1;
    pipeline_desc.cullMode = RenderCullMode::NONE;
    pipeline_desc.depthEnabled = false;
    pipeline_desc.depthWriteEnabled = false;
    pipeline_desc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
    g_blit.pipeline = device->createGraphicsPipeline(pipeline_desc);
  }

  g_blit.descriptor_set = device->createDescriptorSet(descriptor_set_desc);

  if (!g_blit.pipeline || !g_blit.sampler || !g_blit.descriptor_set) {
    REXLOG_ERROR(
        "native_renderer: could not create the present blit pipeline, so the guest's image "
        "cannot be presented");
    g_blit.failed = true;
    return false;
  }

  g_blit.initialized = true;
  return true;
}

// The SDK's presenter cvar, read every present so the setting takes effect
// without a restart. Anything other than an explicit "false" letterboxes, which
// is both the cvar's own default and the safer answer if it ever goes missing.
bool PresentLetterbox() { return rex::cvar::GetFlagByName("present_letterbox") != "false"; }

bool FramePreparePresent(RenderCommandList* commands) {
  ++g_frame;
  ResolvedTexture* source = g_present_texture;
  PollCaptureKey();
  RippleProbeFlush();
  if (g_present_probe_logged < PresentProbeFrames()) {
    ++g_present_probe_logged;
    REXLOG_WARN(
        "native_renderer: present probe frame {} source=0x{:08X} texture={:016X} {}x{} | this "
        "frame: resolves={} clears={} | targets={} resolved={}",
        g_present_probe_logged, source ? source->address : 0,
        source ? uintptr_t(source->texture.get()) : 0, source ? source->width : 0,
        source ? source->height : 0, g_resolves_copied - g_present_probe_last_resolves,
        g_clears_applied - g_present_probe_last_clears, g_targets.size(), g_resolved.size());
    g_present_probe_last_resolves = g_resolves_copied;
    g_present_probe_last_clears = g_clears_applied;
  }
  if (source == nullptr || !source->texture) {
    ++g_composites_skipped;
    return false;
  }

  if (source->width == 0 || source->height == 0) {
    ++g_composites_skipped;
    return false;
  }
  if (!EnsureBlitResources(PlumeDevice())) {
    ++g_composites_skipped;
    return false;
  }

  // The draw reads it, so it is transitioned here rather than in the pass: the
  // pass has the framebuffer bound, and Plume's Vulkan backend ends the render
  // pass around a barrier issued inside one.
  Transition(commands, source->texture.get(), source->layout, RenderBarrierStage::GRAPHICS,
             RenderTextureLayout::SHADER_READ);

  if (g_blit.bound_texture != source->texture.get()) {
    g_blit.bound_texture = source->texture.get();
    g_blit.descriptor_set->setTexture(0, source->texture.get(), RenderTextureLayout::SHADER_READ);
    g_blit.descriptor_set->setSampler(1, g_blit.sampler.get());
  }
  return true;
}

void FramePresentGuestImage(RenderCommandList* commands, uint32_t width, uint32_t height) {
  ResolvedTexture* source = g_present_texture;
  if (source == nullptr || !source->texture || !g_blit.initialized || width == 0 || height == 0)
    return;

  // Where the guest's image lands in the window. Stretching is the whole target;
  // letterboxing is the largest rectangle of the source's aspect ratio that fits
  // inside it, centred, with the bars left to the caller's clear.
  float x = 0.0f;
  float y = 0.0f;
  float w = float(width);
  float h = float(height);
  if (PresentLetterbox()) {
    const float scale = std::min(w / float(source->width), h / float(source->height));
    w = float(source->width) * scale;
    h = float(source->height) * scale;
    x = (float(width) - w) * 0.5f;
    y = (float(height) - h) * 0.5f;
  }

  commands->setViewports(RenderViewport(x, y, w, h));
  commands->setScissors(RenderRect(int32_t(x), int32_t(y), int32_t(x + w), int32_t(y + h)));
  commands->setPipeline(g_blit.pipeline.get());
  commands->setGraphicsPipelineLayout(g_blit.pipeline_layout.get());
  commands->setGraphicsDescriptorSet(g_blit.descriptor_set.get(), 0);
  commands->drawInstanced(3, 1, 0, 0);

  // Whatever draws next (the overlay) expects the whole window, and this is the
  // only place that narrows it.
  commands->setViewports(RenderViewport(0.0f, 0.0f, float(width), float(height)));
  commands->setScissors(RenderRect(0, 0, int32_t(width), int32_t(height)));

  ++g_composites;
}

RenderFormat FrameColorFormat() { return kColorFormat; }
RenderFormat FrameDepthFormat() { return kDepthFormat; }

void ShutdownFrameTargets() {
  // Before the buffers go: the readback layer holds pointers into them and has
  // guest pages protected on their behalf, and leaving those protected would
  // fault the guest on memory nothing is watching any more.
  ReadbackForgetAll();
  for (auto& resolved : g_resolved) {
    if (resolved->readback && resolved->readback_mapped != nullptr) {
      resolved->readback->unmap();
      resolved->readback_mapped = nullptr;
    }
  }

  g_bound_framebuffer = nullptr;
  g_framebuffers.clear();
  g_targets.clear();
  g_resolved.clear();
  for (bool& bound : g_bound_color_valid)
    bound = false;
  g_bound_depth_valid = false;
  g_present_texture = nullptr;
  // The probe's ids are raw pointers into what was just destroyed, so a new
  // texture could land on an old address and inherit its id.
  g_ripple_texture_ids.clear();
  g_ripple_last_target = nullptr;
}

}  // namespace eternalsonata
