// eternalsonata - ReXGlue Recompiled Project
//
// Shared between the two translation units that speak Plume: the backend that
// owns the device and the swap chain (native_renderer_plume.cpp) and the
// overlay drawer that records into its command list
// (native_renderer_overlay.cpp).
//
// Not included anywhere else. Plume pulls in d3d12.h and windows.h, and the
// point of the public headers is to keep that out of the guest-facing code.

#pragma once

#include <plume_render_interface.h>
#include <rex/ui/presenter.h>

namespace eternalsonata {

// The per-frame handoff the SDK's detached overlay mode is built around: the
// app derives from AppUIDrawContext, adds its own payload, hands it to
// ImGuiDrawer::Draw, and its ImmediateDrawer casts it back in Begin.
//
// The payload here is the live command list. There is no presenter and no
// separate UI submission: the overlay records straight into the frame the
// backend has already begun and is about to present.
class PlumeUIDrawContext final : public rex::ui::AppUIDrawContext {
 public:
  PlumeUIDrawContext(uint32_t width, uint32_t height, plume::RenderCommandList* commands,
                     plume::RenderFormat render_target_format)
      : rex::ui::AppUIDrawContext(width, height),
        commands_(commands),
        render_target_format_(render_target_format) {}

  plume::RenderCommandList* commands() const { return commands_; }
  plume::RenderFormat render_target_format() const { return render_target_format_; }

 private:
  plume::RenderCommandList* commands_;
  plume::RenderFormat render_target_format_;
};

// The backend's device and queue, or null before it is up. The overlay drawer
// creates its own pipelines and buffers from these, and uses the queue only for
// one-shot texture uploads; frame recording goes through the command list the
// draw context carries.
plume::RenderDevice* PlumeDevice();
plume::RenderCommandQueue* PlumeQueue();

// Which blob format the backend's shaders have to be in. This is a property of
// the render interface, not of the device, because it is decided by the API
// that was selected rather than by the adapter behind it.
plume::RenderShaderFormat PlumeShaderFormat();

// The command list the guest's rendering records into, begun on demand.
//
// The guest does not produce a frame between two calls we control: it clears,
// draws and resolves from wherever it likes and only tells us the frame is over
// at Swap. So the frame's recording starts at the first guest render call after
// the previous present, not at a "begin frame" that has nowhere to live, and the
// present is simply the tail of that same list.
//
// Null before the backend is up. Guest thread only, which is also the thread
// that presents.
plume::RenderCommandList* PlumeGuestCommands();

// Copy the guest's finished image into the swap chain image, appended to the
// frame's own recording. False when there is nothing to show yet, which is the
// case until the guest's first resolve; the caller then falls back to a clear so
// that "no image" stays distinguishable from "black image" on sight.
//
// Leaves the backbuffer in COPY_DEST, since the caller has to transition it for
// the overlay pass either way.
bool FrameComposite(plume::RenderCommandList* commands, plume::RenderTexture* backbuffer,
                    uint32_t width, uint32_t height);

// Bind the targets the guest currently has bound as the command list's
// framebuffer, transitioning them if they are not already writable, and hand
// back its size so a draw can clamp the guest's viewport to it. Null when the
// guest has nothing bound, which is a draw that cannot be issued.
//
// The bind is deduplicated inside, because Plume's Vulkan backend ends the
// active render pass on every setFramebuffer.
plume::RenderFramebuffer* FrameBindDrawTargets(plume::RenderCommandList* commands,
                                               uint32_t* width, uint32_t* height);

// A fresh command list has no framebuffer bound, so the deduplication above has
// to be told. Called from wherever the frame's list is begun.
void FrameNotifyCommandListBegun();

// The formats the guest's host render targets are created in. A pipeline has to
// be built against the formats of the targets it will draw into, so these are
// shared rather than repeated: colour is the swap chain's format so the present
// blit is a copy rather than a conversion, and every colour surface this title
// creates is an 8888 variant.
plume::RenderFormat FrameColorFormat();
plume::RenderFormat FrameDepthFormat();

// Drop every host render target. The device is going away, so this has to run
// before it does.
void ShutdownFrameTargets();

}  // namespace eternalsonata
