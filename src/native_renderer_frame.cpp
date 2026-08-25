// eternalsonata - ReXGlue Recompiled Project
//
// See native_renderer_frame.h.

#include "native_renderer_frame.h"

#include <memory>
#include <vector>

#include <rex/logging.h>

#include "native_renderer_plume_internal.h"

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
};

// Binds of an address that is a known resolve destination but at an extent that
// is not the one resolved there, i.e. memory that has stopped being a render
// target. Counted because it is the discriminator between the two, and a large
// number would mean the rule is too strict rather than that the game is
// recycling buffers.
uint64_t g_resolve_extent_mismatch = 0;

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
uint32_t g_size_mismatch_reported = 0;
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

void FrameResolve(uint32_t source, uint32_t dest_address, uint32_t dest_width,
                  uint32_t dest_height, int32_t src_x1, int32_t src_y1, int32_t src_x2,
                  int32_t src_y2, int32_t dest_x, int32_t dest_y) {
  // 4 is the depth stencil. Resolving depth out is a real thing this title does,
  // but it is not what the screen shows and the host depth format does not copy
  // into a colour texture, so it is left alone for now.
  if (source >= d3d::kColorSurfaceCount)
    return;
  GuestTarget* target = BoundColorTarget(source);
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
    // resolves into textures it goes on to render into.
    created->texture =
        device->createTexture(RenderTextureDesc::ColorTarget(dest_width, dest_height, kColorFormat));
    if (created->texture) {
      REXLOG_INFO("native_renderer: resolve destination 0x{:08X} is {}x{}", dest_address,
                  dest_width, dest_height);
      g_resolved.push_back(std::move(created));
      destination = g_resolved.back().get();
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

  if (g_resolve_examples < 12) {
    ++g_resolve_examples;
    REXLOG_INFO(
        "native_renderer: resolved EDRAM tile {} ({},{})..({},{}) into 0x{:08X} at ({},{})",
        target->base_tile, x1, y1, x2, y2, dest_address, place_x, place_y);
  }

  ++g_resolves_copied;
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
      return nullptr;
    }
    return candidate->texture.get();
  }
  return nullptr;
}

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
  if (width)
    *width = framebuffer->getWidth();
  if (height)
    *height = framebuffer->getHeight();
  return framebuffer;
}

void FrameNotifyCommandListBegun() { g_bound_framebuffer = nullptr; }

bool FrameComposite(RenderCommandList* commands, RenderTexture* backbuffer, uint32_t width,
                    uint32_t height) {
  ResolvedTexture* source = g_present_texture;
  if (source == nullptr || !source->texture) {
    ++g_composites_skipped;
    return false;
  }

  // Copy the overlapping region rather than requiring the two to agree. The
  // title renders 1280x720 into a swap chain created at the same size, so they
  // do agree today; a window resize is what breaks it, and a letterboxed copy
  // keeps something on screen while the scaling blit that belongs here does not
  // exist yet.
  const uint32_t copy_width = source->width < width ? source->width : width;
  const uint32_t copy_height = source->height < height ? source->height : height;
  if (copy_width == 0 || copy_height == 0) {
    ++g_composites_skipped;
    return false;
  }
  if ((source->width != width || source->height != height) && g_size_mismatch_reported < 4) {
    ++g_size_mismatch_reported;
    REXLOG_WARN(
        "native_renderer: the guest image is {}x{} and the swap chain is {}x{}; presenting the "
        "overlap. A scaling blit belongs here.",
        source->width, source->height, width, height);
  }

  Transition(commands, source->texture.get(), source->layout, RenderBarrierStage::COPY,
             RenderTextureLayout::COPY_SOURCE);
  commands->barriers(RenderBarrierStage::COPY,
                     RenderTextureBarrier(backbuffer, RenderTextureLayout::COPY_DEST));

  const RenderBox box(0, 0, int32_t(copy_width), int32_t(copy_height));
  commands->copyTextureRegion(RenderTextureCopyLocation::Subresource(backbuffer),
                              RenderTextureCopyLocation::Subresource(source->texture.get()), 0, 0,
                              0, &box);

  ++g_composites;
  return true;
}

RenderFormat FrameColorFormat() { return kColorFormat; }
RenderFormat FrameDepthFormat() { return kDepthFormat; }

void ShutdownFrameTargets() {
  g_bound_framebuffer = nullptr;
  g_framebuffers.clear();
  g_targets.clear();
  g_resolved.clear();
  for (bool& bound : g_bound_color_valid)
    bound = false;
  g_bound_depth_valid = false;
  g_present_texture = nullptr;
}

}  // namespace eternalsonata
