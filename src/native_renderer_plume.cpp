// eternalsonata - ReXGlue Recompiled Project
//
// See native_renderer_plume.h.

#include "native_renderer_plume.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include <plume_render_interface.h>
#include <rex/ui/ui_drawer.h>

#include "native_renderer_draw.h"
#include "native_renderer_texture.h"
#include "native_renderer_pipeline.h"
#include "native_renderer_plume_internal.h"
#include "native_renderer_profile.h"

namespace plume {
#ifdef _WIN32
extern std::unique_ptr<RenderInterface> CreateD3D12Interface();
#endif
extern std::unique_ptr<RenderInterface> CreateVulkanInterface();
}  // namespace plume

namespace eternalsonata {
namespace {

using namespace plume;

// B8G8R8A8 rather than R8G8B8A8 because it is the format every Windows swap
// chain supports natively; picking the other one costs a conversion on present
// for no benefit here.
constexpr RenderFormat kSwapChainFormat = RenderFormat::B8G8R8A8_UNORM;
constexpr uint32_t kSwapChainBuffers = 2;

struct PlumeBackend {
  // Not named `interface`: windows.h defines that as a macro, and Plume drags
  // windows.h in behind d3d12.h.
  std::unique_ptr<RenderInterface> render_interface;
  std::unique_ptr<RenderDevice> device;
  std::unique_ptr<RenderCommandQueue> queue;
  std::unique_ptr<RenderCommandList> command_list;
  std::unique_ptr<RenderCommandFence> fence;
  std::unique_ptr<RenderSwapChain> swap_chain;
  std::unique_ptr<RenderCommandSemaphore> acquire_semaphore;
  std::vector<std::unique_ptr<RenderCommandSemaphore>> release_semaphores;
  std::vector<std::unique_ptr<RenderFramebuffer>> framebuffers;
  std::string api_name;
};

PlumeBackend g_backend;
std::atomic<bool> g_ready{false};

// Set from the UI thread when the window resizes, consumed by the next present
// on the guest thread. The swap chain is not thread safe, so the resize has to
// happen where every other swap chain call happens rather than where the event
// arrives.
std::atomic<bool> g_resize_pending{false};

// The SDK's ImGui drawer, once the app hands it over. Null until then, and
// null forever if the overlay drawer could not be created, in which case the
// frame is just the clear.
rex::ui::UIDrawer* g_overlay = nullptr;

uint64_t g_frames_presented = 0;
uint64_t g_acquire_failures = 0;
bool g_acquire_failure_reported = false;

// Whether the frame's command list is currently open. The guest opens it at its
// first render call of the frame and the present closes it; see
// PlumeGuestCommands.
bool g_recording = false;

// Submit and wait on whatever the frame recorded, without presenting it. Used
// when the frame cannot be shown (a minimised window, a failed acquire) but has
// already recorded guest work: the list has to be closed and drained either
// way, or the next frame reopens a list that is still queued.
void FlushWithoutPresent() {
  if (!g_recording)
    return;
  g_recording = false;
  g_backend.command_list->end();

  const RenderCommandList* submit = g_backend.command_list.get();
  g_backend.queue->executeCommandLists(&submit, 1, nullptr, 0, nullptr, 0, g_backend.fence.get());
  g_backend.queue->waitForCommandFence(g_backend.fence.get());
  ResetGuestDrawArena();
}

// One framebuffer per swap chain image. Rebuilt whenever the swap chain is,
// because the textures behind them are replaced by a resize.
void CreateFramebuffers() {
  g_backend.framebuffers.clear();
  const uint32_t count = g_backend.swap_chain->getTextureCount();
  for (uint32_t i = 0; i < count; ++i) {
    const RenderTexture* attachment = g_backend.swap_chain->getTexture(i);
    RenderFramebufferDesc desc;
    desc.colorAttachments = &attachment;
    desc.colorAttachmentsCount = 1;
    desc.depthAttachment = nullptr;
    g_backend.framebuffers.push_back(g_backend.device->createFramebuffer(desc));
  }
}

void ApplyResize() {
  if (!g_backend.swap_chain->resize()) {
    REXLOG_WARN("native_renderer: Plume swap chain resize failed");
    return;
  }
  CreateFramebuffers();
}

}  // namespace

RenderDevice* PlumeDevice() {
  return g_ready.load(std::memory_order_acquire) ? g_backend.device.get() : nullptr;
}

RenderCommandQueue* PlumeQueue() {
  return g_ready.load(std::memory_order_acquire) ? g_backend.queue.get() : nullptr;
}

RenderShaderFormat PlumeShaderFormat() {
  return g_ready.load(std::memory_order_acquire)
             ? g_backend.render_interface->getCapabilities().shaderFormat
             : RenderShaderFormat::UNKNOWN;
}

void PlumeSetOverlayDrawer(rex::ui::UIDrawer* drawer) { g_overlay = drawer; }

RenderCommandList* PlumeGuestCommands() {
  if (!g_ready.load(std::memory_order_acquire))
    return nullptr;
  if (!g_recording) {
    g_backend.command_list->begin();
    g_recording = true;
    FrameNotifyCommandListBegun();
  }
  return g_backend.command_list.get();
}

bool InitPlumeBackend(void* window_handle) {
  if (g_ready.load(std::memory_order_acquire))
    return true;
  if (window_handle == nullptr) {
    REXLOG_ERROR("native_renderer: no native window handle, cannot bring up Plume");
    return false;
  }

  // D3D12 on Windows, Vulkan elsewhere. Plume's Vulkan backend goes through
  // volk and loads the loader at runtime, so this does not add a link time
  // dependency on a Vulkan SDK.
#ifdef _WIN32
  g_backend.render_interface = CreateD3D12Interface();
  g_backend.api_name = "D3D12";
  if (!g_backend.render_interface) {
    g_backend.render_interface = CreateVulkanInterface();
    g_backend.api_name = "Vulkan";
  }
#else
  g_backend.render_interface = CreateVulkanInterface();
  g_backend.api_name = "Vulkan";
#endif
  if (!g_backend.render_interface) {
    REXLOG_ERROR("native_renderer: could not create any Plume render interface");
    return false;
  }

  g_backend.device = g_backend.render_interface->createDevice();
  if (!g_backend.device) {
    REXLOG_ERROR("native_renderer: Plume {} interface created no device", g_backend.api_name);
    g_backend.render_interface.reset();
    return false;
  }

  g_backend.queue = g_backend.device->createCommandQueue(RenderCommandListType::DIRECT);
  g_backend.command_list = g_backend.queue->createCommandList();
  g_backend.fence = g_backend.device->createCommandFence();
  g_backend.acquire_semaphore = g_backend.device->createCommandSemaphore();

  g_backend.swap_chain = g_backend.queue->createSwapChain(
      RenderSwapChainDesc(static_cast<RenderWindow>(window_handle), kSwapChainFormat,
                          kSwapChainBuffers));
  if (!g_backend.swap_chain) {
    REXLOG_ERROR("native_renderer: Plume could not create a swap chain on the game window");
    ShutdownPlumeBackend();
    return false;
  }

  // Vsync from the game's own cvar, which defaults to false (see settings.cpp
  // for why; the SDK's vblank pump ties the presentation-interval wait to it).
  // Plume's swap chains come up with vsync *on*, so without this the setting
  // silently did not apply to the native renderer at all.
  //
  // It is not just a tearing preference here. There are two swap chain buffers
  // and the present waits on its fence, so a frame that misses the vblank
  // cannot start the next one until the one after: the frame rate quantises to
  // 60, 30, 20 with nothing in between, and anything over 16.7 ms of real work
  // reads as exactly 30 fps regardless of how much over it is. Measured on the
  // heavy scenes: 28.7 ms/frame with vsync on became a flat 16.67 ms cap hit
  // with it off.
  const bool vsync = rex::cvar::GetFlagByName("vsync") == "true";
  g_backend.swap_chain->setVsyncEnabled(vsync);

  // The swap chain has no textures until the first resize, so this is required
  // rather than defensive; the example does the same.
  g_backend.swap_chain->resize();
  CreateFramebuffers();

  while (g_backend.release_semaphores.size() < g_backend.swap_chain->getTextureCount())
    g_backend.release_semaphores.push_back(g_backend.device->createCommandSemaphore());

  const RenderDeviceDescription& description = g_backend.device->getDescription();
  REXLOG_INFO(
      "native_renderer: Plume up on {} using \"{}\", swap chain {}x{} with {} buffers, vsync {}. "
      "The window is now ours to draw into.",
      g_backend.api_name, description.name, g_backend.swap_chain->getWidth(),
      g_backend.swap_chain->getHeight(), g_backend.swap_chain->getTextureCount(),
      vsync ? "on" : "off");

  g_ready.store(true, std::memory_order_release);
  return true;
}

bool PlumeBackendReady() { return g_ready.load(std::memory_order_acquire); }

void PlumePresentFrame() {
  if (!g_ready.load(std::memory_order_acquire))
    return;
  ProfileZone present_zone(kPhasePresent);

  // Both the queued resize and the swap chain's own "I need one" answer, since
  // a minimise or a DPI change can invalidate it without an event we hooked.
  if (g_resize_pending.exchange(false, std::memory_order_acq_rel) ||
      g_backend.swap_chain->needsResize()) {
    ApplyResize();
  }

  // A minimised window has a zero sized swap chain and nothing to present to.
  // The frame's guest work still has to be drained, or the next frame reopens a
  // list that is still recording.
  if (g_backend.swap_chain->isEmpty() || g_backend.framebuffers.empty()) {
    FlushWithoutPresent();
    return;
  }

  uint32_t image = 0;
  if (!g_backend.swap_chain->acquireTexture(g_backend.acquire_semaphore.get(), &image)) {
    // Usually means the swap chain went out of date between the check above and
    // here; the next frame's resize picks it up. Counted rather than logged per
    // occurrence so a resize storm cannot flood the log.
    ++g_acquire_failures;
    if (!g_acquire_failure_reported) {
      g_acquire_failure_reported = true;
      REXLOG_WARN(
          "native_renderer: Plume failed to acquire a swap chain texture; frames will be "
          "dropped until it recovers");
    }
    g_resize_pending.store(true, std::memory_order_release);
    FlushWithoutPresent();
    return;
  }
  if (image >= g_backend.framebuffers.size()) {
    FlushWithoutPresent();
    return;
  }

  RenderTexture* backbuffer = g_backend.swap_chain->getTexture(image);
  RenderCommandList* commands = PlumeGuestCommands();
  if (commands == nullptr)
    return;

  const uint32_t width = g_backend.swap_chain->getWidth();
  const uint32_t height = g_backend.swap_chain->getHeight();

  // The guest's image, if it has produced one. This is appended to the same
  // list the guest's own clears and resolves recorded into, so the copy is
  // ordered after them without needing anything else to synchronise the two.
  const bool have_guest_image = FrameComposite(commands, backbuffer, width, height);

  commands->barriers(RenderBarrierStage::GRAPHICS,
                     RenderTextureBarrier(backbuffer, RenderTextureLayout::COLOR_WRITE));
  commands->setFramebuffer(g_backend.framebuffers[image].get());
  commands->setViewports(RenderViewport(0.0f, 0.0f, float(width), float(height)));
  commands->setScissors(RenderRect(0, 0, int32_t(width), int32_t(height)));

  // Only when the guest has nothing to show. It is deliberately not black: a
  // black clear is indistinguishable from the no-backend case, and being able
  // to tell those apart on sight is worth keeping.
  if (!have_guest_image)
    commands->clearColor(0, RenderColor(0.05f, 0.06f, 0.10f, 1.0f));

  // The SDK's overlays record straight into this frame. There is no presenter
  // and no separate UI pass in detached mode: the drawer is handed the live
  // command list through the draw context and draws into the image we are about
  // to present.
  if (g_overlay != nullptr) {
    PlumeUIDrawContext context(width, height, commands, kSwapChainFormat);
    g_overlay->Draw(context);
  }

  commands->barriers(RenderBarrierStage::NONE,
                     RenderTextureBarrier(backbuffer, RenderTextureLayout::PRESENT));
  commands->end();
  g_recording = false;

  const RenderCommandList* submit = commands;
  RenderCommandSemaphore* wait = g_backend.acquire_semaphore.get();
  RenderCommandSemaphore* signal = g_backend.release_semaphores[image].get();
  g_backend.queue->executeCommandLists(&submit, 1, &wait, 1, &signal, 1, g_backend.fence.get());
  g_backend.swap_chain->present(image, &signal, 1);

  // One command list and one fence, so the next frame cannot start recording
  // until this one is done with them. Fine at this size; it becomes a real
  // frames-in-flight ring once there is actual work per frame.
  {
    ProfileZone fence_zone(kPhaseFenceWait);
    g_backend.queue->waitForCommandFence(g_backend.fence.get());
  }

  // Safe only because of the wait above: the frame that owned those bytes has
  // retired, so the next one can hand them out again. This is the assumption to
  // revisit first when there are frames in flight.
  ResetGuestDrawArena();

  if (++g_frames_presented % 600 == 0) {
    REXLOG_INFO("native_renderer: Plume presented {} frames ({} acquire failures)",
                g_frames_presented, g_acquire_failures);
  }
}

void PlumeNotifyResize(uint32_t pixel_width, uint32_t pixel_height) {
  (void)pixel_width;
  (void)pixel_height;
  g_resize_pending.store(true, std::memory_order_release);
}

void ShutdownPlumeBackend() {
  g_ready.store(false, std::memory_order_release);

  // Order matters: the queue has to be idle before anything it referenced is
  // destroyed, and the swap chain has to go before the queue that made it.
  if (g_backend.swap_chain)
    g_backend.swap_chain->wait();

  ShutdownGuestDraws();
  ShutdownTextureMirror();
  ShutdownGuestPipelines();
  ShutdownFrameTargets();
  g_backend.framebuffers.clear();
  g_backend.release_semaphores.clear();
  g_backend.acquire_semaphore.reset();
  g_backend.swap_chain.reset();
  g_backend.fence.reset();
  g_backend.command_list.reset();
  g_backend.queue.reset();
  g_backend.device.reset();
  g_backend.render_interface.reset();
}

}  // namespace eternalsonata
