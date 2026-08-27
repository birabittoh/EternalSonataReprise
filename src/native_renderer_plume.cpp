// eternalsonata - ReXGlue Recompiled Project
//
// See native_renderer_plume.h.

#include "native_renderer_plume.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
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
#if defined(PLUME_SDL_VULKAN_ENABLED)
extern std::unique_ptr<RenderInterface> CreateVulkanInterface(RenderWindow sdlWindow);
#else
extern std::unique_ptr<RenderInterface> CreateVulkanInterface();
#endif
}  // namespace plume

namespace eternalsonata {
namespace {

using namespace plume;

// B8G8R8A8 rather than R8G8B8A8 because it is the format every Windows swap
// chain supports natively; picking the other one costs a conversion on present
// for no benefit here.
constexpr RenderFormat kSwapChainFormat = RenderFormat::B8G8R8A8_UNORM;
// Three, not two. With two there is exactly one spare buffer, so a frame cannot
// start until the previous one's flip has retired its buffer — which under vsync
// means waiting for a vblank, and the frame rate quantises to 60/30/20 with
// nothing in between. The third buffer is what lets a frame that overran by a
// little cost a little rather than a whole vblank. See kMaxFrameLatency.
constexpr uint32_t kSwapChainBuffers = 3;

// How many presents may be outstanding before the frame loop blocks. Two gives
// the third buffer above something to do; one would serialise again. This is
// also the value Plume hands to SetMaximumFrameLatency, which it calls
// unconditionally on D3D12 — the previous 0 (the RenderSwapChainDesc default)
// is not a legal argument, so the swap chain was silently running on DXGI's
// default of 1.
constexpr uint32_t kMaxFrameLatency = 2;

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

// Same shape as the resize, and for the same reason: the `vsync` cvar can be
// changed from the settings overlay at any time, but the swap chain is only
// safe to touch from the thread that presents. The change callback records the
// request; the next present applies it.
std::atomic<bool> g_vsync_wanted{false};
std::atomic<bool> g_vsync_applied{false};

// Whether the swap chain was created with present wait, i.e. whether the
// per-frame wait() below is legal to call. Set once during init.
bool g_present_wait = false;

bool ReadVsyncCvar() {
  return rex::cvar::GetFlagByName("vsync") == "true";
}

void ApplyVsyncIfChanged() {
  const bool wanted = g_vsync_wanted.load(std::memory_order_acquire);
  if (wanted == g_vsync_applied.load(std::memory_order_relaxed))
    return;
  g_backend.swap_chain->setVsyncEnabled(wanted);
  g_vsync_applied.store(wanted, std::memory_order_relaxed);
  REXLOG_INFO("native_renderer: vsync {}", wanted ? "on" : "off");
}

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

// Grows the per-image release semaphores to match the swap chain. Separate from
// creation because the count is not fixed: Vulkan reports back however many
// images the driver actually gave us, which can exceed what we asked for and can
// change across a resize (a mailbox swap chain asks for a third image). The
// semaphores are indexed by the acquired image index, so a vector that lagged
// behind the texture count would be indexed out of bounds.
void GrowReleaseSemaphores() {
  while (g_backend.release_semaphores.size() < g_backend.swap_chain->getTextureCount())
    g_backend.release_semaphores.push_back(g_backend.device->createCommandSemaphore());
}

void ApplyResize() {
  if (!g_backend.swap_chain->resize()) {
    REXLOG_WARN("native_renderer: Plume swap chain resize failed");
    return;
  }
  CreateFramebuffers();
  GrowReleaseSemaphores();
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
  //
  // Plume always builds its Vulkan backend, including on Windows, so the
  // Vulkan path can be forced here to reproduce a Linux-only defect on a
  // Windows box: set ETERNALSONATA_RENDER_API=vulkan (or =d3d12 to be
  // explicit). Anything else, or unset, keeps the per platform default.
#ifdef _WIN32
  const char* api_override = std::getenv("ETERNALSONATA_RENDER_API");
  const bool force_vulkan =
      api_override != nullptr &&
      (std::strcmp(api_override, "vulkan") == 0 || std::strcmp(api_override, "Vulkan") == 0 ||
       std::strcmp(api_override, "vk") == 0);
  if (force_vulkan) {
    REXLOG_INFO("native_renderer: ETERNALSONATA_RENDER_API selects Vulkan over D3D12");
    g_backend.render_interface = CreateVulkanInterface();
    g_backend.api_name = "Vulkan";
    if (!g_backend.render_interface)
      REXLOG_ERROR("native_renderer: Vulkan was forced but no interface could be created");
  } else {
    g_backend.render_interface = CreateD3D12Interface();
    g_backend.api_name = "D3D12";
    if (!g_backend.render_interface) {
      g_backend.render_interface = CreateVulkanInterface();
      g_backend.api_name = "Vulkan";
    }
  }
#elif defined(PLUME_SDL_VULKAN_ENABLED)
  g_backend.render_interface = CreateVulkanInterface(static_cast<RenderWindow>(window_handle));
  g_backend.api_name = "Vulkan";
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

  // Present wait is what makes vsync usable on a frame-clocked engine. Without
  // it Plume's D3D12 present issues `Present(1, 0)`, which blocks on the vblank
  // and quantises the achieved rate to 60/30/20; the guest advances its sim a
  // fixed 300/declared units per present, so a quantised rate is not choppiness
  // but literal slow motion (game speed = actual fps / declared fps). It showed
  // up as half speed during battle attacks, where a frame first overruns
  // 16.7 ms. With present wait, Plume issues `Present(0, 0)` instead: no tearing
  // (the flip still lands on a vblank, DXGI_PRESENT_ALLOW_TEARING is not set),
  // but the call does not block, and pacing comes from the frame-latency
  // waitable object plus the host limiter in eternalsonata_framerate.cpp.
  //
  // Gated on the capability because wait() and present() both assert on it.
  // D3D12 always reports it; Vulkan needs VK_KHR_present_wait, and where that is
  // missing this falls back to today's behaviour (FIFO, still quantised — fixing
  // that needs MAILBOX, which is a Plume-side change).
  g_present_wait = g_backend.device->getCapabilities().presentWait;
  g_backend.swap_chain = g_backend.queue->createSwapChain(
      RenderSwapChainDesc(static_cast<RenderWindow>(window_handle), kSwapChainFormat,
                          kSwapChainBuffers, g_present_wait, kMaxFrameLatency));
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
  // It used to be more than a tearing preference here: with two buffers and a
  // blocking `Present(1, 0)`, a frame that missed the vblank could not start the
  // next one until the one after, so the rate quantised to 60/30/20 and anything
  // over 16.7 ms of work read as exactly 30 fps regardless of how far over it
  // was. Measured on the heavy scenes: 28.7 ms/frame with vsync on became a flat
  // 16.67 ms cap with it off. The third buffer and present wait above remove
  // that quantisation, so this is back to being just a tearing preference.
  // The cvar itself is registered by RegisterNativeRendererCvars(), because the
  // one the SDK defines lives in the Xenos plugin and no plugin is loaded here.
  // It is hot-reloadable there, so it is hot-reloadable here too: the callback
  // only records the request, and the next present applies it (Vulkan reports
  // needsResize() until the present mode actually matches, which the present's
  // existing resize check then honours).
  const bool vsync = ReadVsyncCvar();
  g_backend.swap_chain->setVsyncEnabled(vsync);
  g_vsync_wanted.store(vsync, std::memory_order_release);
  g_vsync_applied.store(vsync, std::memory_order_relaxed);
  // Read back through the registry rather than parsing the callback's value
  // string: the setter has already run by then, and it is the one place that
  // knows "1" and "yes" mean true as well.
  rex::cvar::RegisterChangeCallback("vsync", [](std::string_view, std::string_view) {
    g_vsync_wanted.store(ReadVsyncCvar(), std::memory_order_release);
  });

  // The swap chain has no textures until the first resize, so this is required
  // rather than defensive; the example does the same.
  g_backend.swap_chain->resize();
  CreateFramebuffers();

  GrowReleaseSemaphores();

  const RenderDeviceDescription& description = g_backend.device->getDescription();
  REXLOG_INFO(
      "native_renderer: Plume up on {} using \"{}\", swap chain {}x{} with {} buffers, vsync {}, "
      "present wait {}. The window is now ours to draw into.",
      g_backend.api_name, description.name, g_backend.swap_chain->getWidth(),
      g_backend.swap_chain->getHeight(), g_backend.swap_chain->getTextureCount(),
      vsync ? "on" : "off", g_present_wait ? "on" : "off (vsync will quantise the frame rate)");

  g_ready.store(true, std::memory_order_release);
  return true;
}

bool PlumeBackendReady() { return g_ready.load(std::memory_order_acquire); }

bool PlumeFlushGuestWork() {
  if (!g_ready.load(std::memory_order_acquire) || !g_recording)
    return false;
  FlushWithoutPresent();
  return true;
}

void PlumePresentFrame() {
  if (!g_ready.load(std::memory_order_acquire))
    return;
  ProfileZone present_zone(kPhasePresent);

  // Before the resize check, because on Vulkan a present mode change is exactly
  // what needsResize() reports.
  ApplyVsyncIfChanged();

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

  // Bounds how far ahead of the display the frame loop may run. `Present(0, 0)`
  // does not block, so without this the queue would fill to the buffer count and
  // every frame would carry that much added input lag. Blocking here instead of
  // inside present is the whole point: it yields when there is genuinely nothing
  // to do rather than snapping the frame rate to a divisor of the refresh rate.
  if (g_present_wait) {
    ProfileZone wait_zone(kPhasePresent);
    g_backend.swap_chain->wait();
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

  // The guest's image, if it has produced one. Its barrier is appended to the
  // same list the guest's own clears and resolves recorded into, so the present
  // is ordered after them without needing anything else to synchronise the two.
  const bool have_guest_image = FramePreparePresent(commands);

  commands->barriers(RenderBarrierStage::GRAPHICS,
                     RenderTextureBarrier(backbuffer, RenderTextureLayout::COLOR_WRITE));
  commands->setFramebuffer(g_backend.framebuffers[image].get());
  commands->setViewports(RenderViewport(0.0f, 0.0f, float(width), float(height)));
  commands->setScissors(RenderRect(0, 0, int32_t(width), int32_t(height)));

  // Unconditional, because the blit does not necessarily cover the window: a
  // letterboxed present leaves bars, and clearing them every frame is what keeps
  // whatever the previous frame put there from showing through. When the guest
  // has nothing to show at all the clear is the frame, and it is deliberately
  // not black: a black clear is indistinguishable from the no-backend case, and
  // being able to tell those apart on sight is worth keeping.
  if (have_guest_image)
    commands->clearColor(0, RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
  else
    commands->clearColor(0, RenderColor(0.05f, 0.06f, 0.10f, 1.0f));

  FramePresentGuestImage(commands, width, height);

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
