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

#if defined(__APPLE__)
#include <dlfcn.h>

#include <filesystem>
#include <string>

#include <rex/filesystem.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>

#include <plume_render_interface.h>
#include <rex/ui/ui_drawer.h>

#include "native_renderer_draw.h"
#include "native_renderer_frame.h"
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
//
// Android is the exception, and it is not a preference there but a requirement.
// Adreno's surfaces do not list B8G8R8A8_UNORM at all, and Plume reacts to a
// format the surface does not support by giving up on picking one, reporting it
// with fprintf(stderr) (which goes nowhere on Android) and then creating the
// swap chain with VK_FORMAT_UNDEFINED regardless. The driver accepts that, so
// there is no error anywhere: presents succeed, buffers reach SurfaceFlinger,
// and everything drawn into those images is discarded. That was the black
// screen, and validation is the only thing that says so:
//
//   VUID-VkSwapchainCreateInfoKHR-imageFormat-01273
//   vkCreateSwapchainKHR(): pCreateInfo->imageFormat is VK_FORMAT_UNDEFINED.
//
// Nothing else has to change with the channel order: a format describes memory
// layout, not which shader output feeds which channel, so the blit still writes
// colour 0 and the surface still reads it correctly.
#if REX_PLATFORM_ANDROID
constexpr RenderFormat kSwapChainFormat = RenderFormat::R8G8B8A8_UNORM;
#else
constexpr RenderFormat kSwapChainFormat = RenderFormat::B8G8R8A8_UNORM;
#endif
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

#if defined(__APPLE__)
// Point Vulkan at the loader and MoltenVK shipped in the package, so the game
// runs without a Vulkan SDK or Homebrew installed.
//
// The loader has to be dlopen'd by absolute path here rather than found later:
// volk dlopens it by leaf name, which only searches the DYLD_ variables, and
// dyld captures those at launch so setenv cannot extend them. Preloading works
// because a leaf dlopen matches an already loaded image. The driver manifest,
// by contrast, is read at instance creation, so VK_DRIVER_FILES is in time.
// Without it vkCreateInstance returns VK_ERROR_INCOMPATIBLE_DRIVER.
//
// Both halves defer to an environment that already says otherwise. Probes the
// flat layout then the .app one, matching the SDK's own vulkan_moltenvk.cpp.
void BootstrapAppleVulkanRuntime() {
  namespace fs = std::filesystem;

  const fs::path exe_dir = rex::filesystem::GetExecutableFolder();
  if (exe_dir.empty())
    return;
  // Contents/MacOS -> Contents, for the bundle layouts below. Empty when the
  // executable is not inside a bundle, in which case those probes are skipped.
  const fs::path bundle = exe_dir.filename() == "MacOS" ? exe_dir.parent_path() : fs::path();

  auto first_existing = [](const std::vector<fs::path>& candidates) -> fs::path {
    for (const fs::path& candidate : candidates) {
      std::error_code ec;
      if (!candidate.empty() && fs::exists(candidate, ec))
        return candidate;
    }
    return {};
  };

  // vulkan/lib first: that is the layout the SDK's CMake stages, so it is the
  // loader the SDK was built against rather than whatever else is lying around.
  std::vector<fs::path> loaders = {
      exe_dir / "vulkan" / "lib" / "libvulkan.1.dylib", exe_dir / "libvulkan.1.dylib",
      exe_dir / "libvulkan.dylib", exe_dir / "lib" / "libvulkan.1.dylib",
      exe_dir / "vulkan" / "lib" / "libMoltenVK.dylib", exe_dir / "libMoltenVK.dylib"};
  if (!bundle.empty()) {
    loaders.push_back(bundle / "Frameworks" / "libvulkan.1.dylib");
    loaders.push_back(bundle / "Frameworks" / "libvulkan.dylib");
    loaders.push_back(bundle / "Frameworks" / "libMoltenVK.dylib");
  }

  const fs::path loader = first_existing(loaders);
  if (!loader.empty()) {
    // RTLD_GLOBAL, not RTLD_LOCAL: volk's later dlopen has to be able to match
    // this image. Never dlclose'd; it is needed for the life of the process.
    if (dlopen(loader.string().c_str(), RTLD_NOW | RTLD_GLOBAL) != nullptr) {
      REXLOG_INFO("native_renderer: preloaded the bundled Vulkan loader from {}", loader.string());
    } else {
      REXLOG_WARN("native_renderer: could not load the bundled Vulkan loader at {}: {}",
                  loader.string(), dlerror());
    }
  } else {
    REXLOG_INFO(
        "native_renderer: no Vulkan loader shipped with this build, falling back to whatever is "
        "installed on the system");
  }

  const char* existing = std::getenv("VK_DRIVER_FILES");
  const char* existing_legacy = std::getenv("VK_ICD_FILENAMES");
  if ((existing && existing[0]) || (existing_legacy && existing_legacy[0]))
    return;

  std::vector<fs::path> icds = {
      exe_dir / "vulkan" / "share" / "vulkan" / "icd.d" / "MoltenVK_icd.json",
      exe_dir / "share" / "vulkan" / "icd.d" / "MoltenVK_icd.json",
      exe_dir / "vulkan" / "icd.d" / "MoltenVK_icd.json"};
  if (!bundle.empty()) {
    icds.push_back(bundle / "Resources" / "vulkan" / "icd.d" / "MoltenVK_icd.json");
  }

  const fs::path icd = first_existing(icds);
  if (icd.empty()) {
    REXLOG_INFO(
        "native_renderer: no MoltenVK driver manifest shipped with this build; if Vulkan instance "
        "creation fails with ErrorIncompatibleDriver, that is why");
    return;
  }
  // Both names: VK_DRIVER_FILES is the current one, VK_ICD_FILENAMES the
  // deprecated spelling older loaders still read.
  setenv("VK_DRIVER_FILES", icd.string().c_str(), 1);
  setenv("VK_ICD_FILENAMES", icd.string().c_str(), 1);
  REXLOG_INFO("native_renderer: using the bundled MoltenVK driver manifest at {}", icd.string());
}
#endif  // __APPLE__

struct PlumeBackend {
  // Not named `interface`: windows.h defines that as a macro, and Plume drags
  // windows.h in behind d3d12.h.
  std::unique_ptr<RenderInterface> render_interface;
  std::unique_ptr<RenderDevice> device;
  std::unique_ptr<RenderCommandQueue> queue;
  std::unique_ptr<RenderSwapChain> swap_chain;
  std::vector<std::unique_ptr<RenderCommandSemaphore>> release_semaphores;
  std::vector<std::unique_ptr<RenderFramebuffer>> framebuffers;
  std::string api_name;
};

// Everything a frame owns for as long as the GPU is still reading it. One per
// slot, which is the whole of what frames in flight costs here; see
// kFramesInFlight for why the number is two.
//
// The upload arena and the readback buffers are per slot too, but they live
// with the code that uses them (native_renderer_draw.cpp and
// native_renderer_frame.cpp) and are recycled through BeginGuestDrawFrame.
struct FrameSlot {
  std::unique_ptr<RenderCommandList> command_list;
  std::unique_ptr<RenderCommandFence> fence;
  // Per slot rather than shared: the next frame acquires its image before the
  // previous frame's wait on this has necessarily executed.
  std::unique_ptr<RenderCommandSemaphore> acquire_semaphore;
  // Two timestamps, written at the start and the end of this slot's command
  // list. See g_gpu_frame_ns in the profile header for why: every other
  // instrument in this renderer measures CPU, and the fence wait alone cannot
  // tell a busy GPU from a present that is pacing us.
  std::unique_ptr<RenderQueryPool> gpu_timer;
  // Whether `fence` has work outstanding on it, and which guest frame that work
  // belongs to. Both are needed: a slot that has never been submitted must not
  // be waited on, and a slot that has retires a specific frame index.
  bool submitted = false;
  uint64_t frame_index = 0;
  // Whether this slot's opening timestamp has been written and is still going
  // to be paired with a closing one.
  bool timer_open = false;
  // Whether a list has been opened since this slot was claimed. A frame can
  // open several, because a mid-frame flush closes one and the next guest
  // render call opens another; only the first is timed.
  bool list_opened = false;
};

FrameSlot g_slots[kFramesInFlight];
uint32_t g_slot = 0;

// The highest guest frame index whose GPU work has completed; see
// PlumeFrameRetired. Frame indices start at zero, so "nothing has retired yet"
// needs a sentinel rather than a lower value.
// Atomic because the readback path asks this from the guest thread that faulted
// on a resolve destination, which is not the thread that retires frames. It was
// a plain uint64_t while the only reader was a one-shot check; a reader that
// now polls it in a loop makes the race real.
std::atomic<uint64_t> g_retired_frame{~0ull};

PlumeBackend g_backend;
std::atomic<bool> g_ready{false};

// Set from the UI thread when the window resizes, consumed by the next present
// on the guest thread. The swap chain is not thread safe, so the resize has to
// happen where every other swap chain call happens rather than where the event
// arrives.
std::atomic<bool> g_resize_pending{false};

// The window surface going away and coming back (Android backgrounding). Same
// deferral as the resize: recorded on the UI thread, applied by the next
// present. `g_pending_surface` holds the new ANativeWindow to rebuild on.
std::atomic<bool> g_surface_lost{false};
std::atomic<void*> g_pending_surface{nullptr};

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

// Everything submitted to this queue runs in the order it was submitted, so
// waiting on any one fence retires every frame submitted at or before it. That
// is what lets the flush path below retire the whole ring by waiting on one.
void RetireThrough(uint64_t frame) {
  const uint64_t retired = g_retired_frame.load(std::memory_order_relaxed);
  if (retired == ~0ull || frame > retired)
    g_retired_frame.store(frame, std::memory_order_release);
}

// Block until this slot's outstanding work has run, and collect what it
// measured. A slot with nothing outstanding returns immediately.
void WaitForSlot(FrameSlot& slot) {
  if (!slot.submitted)
    return;
  {
    ProfileZone fence_zone(kPhaseFenceWait);
    g_backend.queue->waitForCommandFence(slot.fence.get());
  }
  slot.submitted = false;
  RetireThrough(slot.frame_index);

  // Only now are the timestamps this slot wrote guaranteed to have landed. Both
  // are in nanoseconds on the GPU's clock; a frame where the second is not
  // ahead of the first is a wrap or a dropped resolve, and is dropped rather
  // than clamped so the average stays honest.
  if (slot.timer_open) {
    slot.timer_open = false;
    slot.gpu_timer->queryResults();
    const uint64_t* stamps = slot.gpu_timer->getResults();
    if (stamps != nullptr && stamps[1] > stamps[0]) {
      g_gpu_frame_ns += stamps[1] - stamps[0];
      ++g_gpu_frame_count;
    }
  }
}

// Drain the whole ring. For the two things that need the queue genuinely idle:
// a swap chain resize, and shutdown.
void WaitForAllSlots() {
  for (FrameSlot& slot : g_slots)
    WaitForSlot(slot);
}

// Submit and wait on whatever the frame recorded, without presenting it. Used
// when the frame cannot be shown (a minimised window, a failed acquire) but has
// already recorded guest work, and by the readback path when the guest reads a
// destination in the frame that resolved it.
//
// Unlike the present, this waits on the frame it just submitted, because the
// caller is asking for exactly that: a full stall until this frame's GPU work
// has run. It stays on the same slot, so the frame carries on recording into a
// fresh list over a recycled arena.
void FlushWithoutPresent() {
  if (!g_recording)
    return;
  FrameSlot& slot = g_slots[g_slot];
  g_recording = false;
  // No closing timestamp is written here, so this frame is dropped from the GPU
  // average rather than read back half written. Flushes are rare by design.
  slot.timer_open = false;
  slot.command_list->end();

  const RenderCommandList* submit = slot.command_list.get();
  g_backend.queue->executeCommandLists(&submit, 1, nullptr, 0, nullptr, 0, slot.fence.get());
  slot.submitted = true;
  slot.frame_index = FrameIndex();
  WaitForSlot(slot);
  BeginGuestDrawFrame(g_slot);
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

// Drops the swap chain and everything indexed by its images. Presenting is
// skipped until a new surface arrives, but the frame's guest work still has to
// be drained every frame or the next one reopens a list that is still
// recording, which is what FlushWithoutPresent in the present path is for.
void ReleaseSwapChain() {
  if (!g_backend.swap_chain)
    return;
  // The images are still being rendered into by whatever is in flight.
  WaitForAllSlots();
  g_backend.framebuffers.clear();
  g_backend.swap_chain.reset();
}

bool RebuildSwapChain(void* window_handle) {
  ReleaseSwapChain();
  if (window_handle == nullptr)
    return false;

  g_backend.swap_chain = g_backend.queue->createSwapChain(
      RenderSwapChainDesc(static_cast<RenderWindow>(window_handle), kSwapChainFormat,
                          kSwapChainBuffers, g_present_wait, kMaxFrameLatency));
  if (!g_backend.swap_chain) {
    REXLOG_ERROR("native_renderer: could not rebuild the swap chain on the new window surface");
    return false;
  }
  // Same order as init: vsync before the first resize, since on Vulkan the
  // present mode is what the resize actually bakes in.
  g_backend.swap_chain->setVsyncEnabled(g_vsync_wanted.load(std::memory_order_acquire));
  g_vsync_applied.store(g_vsync_wanted.load(std::memory_order_acquire), std::memory_order_relaxed);
  g_backend.swap_chain->resize();
  CreateFramebuffers();
  GrowReleaseSemaphores();
  REXLOG_INFO("native_renderer: swap chain rebuilt on the new window surface, {}x{}",
              g_backend.swap_chain->getWidth(), g_backend.swap_chain->getHeight());
  return true;
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

RenderFormat PlumeSwapChainFormat() { return kSwapChainFormat; }

void PlumeSetOverlayDrawer(rex::ui::UIDrawer* drawer) { g_overlay = drawer; }

RenderCommandList* PlumeGuestCommands() {
  if (!g_ready.load(std::memory_order_acquire))
    return nullptr;
  FrameSlot& slot = g_slots[g_slot];
  if (!g_recording) {
    slot.command_list->begin();
    // Only the frame's first list is timed. A list opened after a mid-frame
    // flush would pair its opening stamp with the closing one at present and
    // report the tail of the frame as the whole of it.
    const bool first_list_of_frame = !slot.list_opened;
    slot.list_opened = true;
    if (slot.gpu_timer && first_list_of_frame) {
      // Outside any render pass, which is where Vulkan requires the reset; on
      // D3D12 it is a no-op and only the writes matter.
      slot.command_list->resetQueryPool(slot.gpu_timer.get(), 0, 2);
      slot.command_list->writeTimestamp(slot.gpu_timer.get(), 0);
      slot.timer_open = true;
    }
    g_recording = true;
    FrameNotifyCommandListBegun();
  }
  return slot.command_list.get();
}

uint32_t PlumeFrameSlot() { return g_slot; }

bool PlumeFrameRetired(uint64_t frame) {
  const uint64_t retired = g_retired_frame.load(std::memory_order_acquire);
  return retired != ~0ull && frame <= retired;
}

bool InitPlumeBackend(void* window_handle, void* window_view) {
  if (g_ready.load(std::memory_order_acquire))
    return true;
  if (window_handle == nullptr) {
    REXLOG_ERROR("native_renderer: no native window handle, cannot bring up Plume");
    return false;
  }

  // Plume's RenderWindow is a single handle except on Apple, where it is a
  // {NSWindow*, CAMetalLayer*} pair. Plume only asserts the layer is non-null,
  // which a release build drops, so check it here instead.
  //
  // The interface still comes from the no-argument CreateVulkanInterface()
  // below: Apple lists VK_EXT_metal_surface statically and only needs a window
  // at swap chain time, unlike the SDL path.
#if defined(__APPLE__)
  if (window_view == nullptr) {
    REXLOG_ERROR("native_renderer: no CAMetalLayer for the game window, cannot bring up Plume");
    return false;
  }
  const RenderWindow render_window{window_handle, window_view};

  // Before anything touches volk. See the function's comment for why this
  // cannot be done with environment variables alone.
  BootstrapAppleVulkanRuntime();
#else
  (void)window_view;
  const RenderWindow render_window = static_cast<RenderWindow>(window_handle);
#endif

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
  g_backend.render_interface = CreateVulkanInterface(render_window);
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
  for (FrameSlot& slot : g_slots) {
    slot.command_list = g_backend.queue->createCommandList();
    slot.fence = g_backend.device->createCommandFence();
    slot.acquire_semaphore = g_backend.device->createCommandSemaphore();
    slot.gpu_timer = g_backend.device->createQueryPool(2);
  }
  g_slot = 0;
  g_retired_frame.store(~0ull, std::memory_order_release);

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
      RenderSwapChainDesc(render_window, kSwapChainFormat,
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

void PlumeSurfaceLost() {
  if (!g_ready.load(std::memory_order_acquire))
    return;
  g_pending_surface.store(nullptr, std::memory_order_release);
  g_surface_lost.store(true, std::memory_order_release);
}

void PlumeSurfaceRestored(void* window_handle) {
  if (!g_ready.load(std::memory_order_acquire) || window_handle == nullptr)
    return;
  g_pending_surface.store(window_handle, std::memory_order_release);
}

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

  // Before anything that touches the swap chain, since these can leave it
  // absent. The old surface is already gone by the time the event reaches us,
  // so the teardown is what stops the next present from drawing into a
  // released window.
  if (g_surface_lost.exchange(false, std::memory_order_acq_rel))
    ReleaseSwapChain();
  if (void* surface = g_pending_surface.exchange(nullptr, std::memory_order_acq_rel))
    RebuildSwapChain(surface);

  // Backgrounded, with no surface to present to. The guest keeps running and
  // still needs its work drained.
  if (!g_backend.swap_chain) {
    FlushWithoutPresent();
    return;
  }

  // Before the resize check, because on Vulkan a present mode change is exactly
  // what needsResize() reports.
  ApplyVsyncIfChanged();

  // Both the queued resize and the swap chain's own "I need one" answer, since
  // a minimise or a DPI change can invalidate it without an event we hooked.
  if (g_resize_pending.exchange(false, std::memory_order_acq_rel) ||
      g_backend.swap_chain->needsResize()) {
    // A resize replaces the swap chain's textures, and a frame still in flight
    // is rendering into one of them. Nothing else in the ring needs the queue
    // idle; this does, so it drains it rather than assuming it.
    WaitForAllSlots();
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

  // The frame being recorded, captured before FramePreparePresent below counts
  // the frame boundary: what is about to be submitted is this frame's work, and
  // that is the index the readback path asks about.
  const uint64_t recording_frame = FrameIndex();

  uint32_t image = 0;
  if (!g_backend.swap_chain->acquireTexture(g_slots[g_slot].acquire_semaphore.get(), &image)) {
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
  FrameSlot& submitting = g_slots[g_slot];
  if (submitting.timer_open)
    commands->writeTimestamp(submitting.gpu_timer.get(), 1);
  commands->end();
  g_recording = false;

  const RenderCommandList* submit = commands;
  RenderCommandSemaphore* wait = submitting.acquire_semaphore.get();
  RenderCommandSemaphore* signal = g_backend.release_semaphores[image].get();
  g_backend.queue->executeCommandLists(&submit, 1, &wait, 1, &signal, 1, submitting.fence.get());
  submitting.submitted = true;
  submitting.frame_index = recording_frame;
  g_backend.swap_chain->present(image, &signal, 1);

  // And then do *not* wait for it. This is the whole of frames in flight: the
  // frame just submitted runs on the GPU while the CPU records the next one,
  // and the only thing waited on here is the frame kFramesInFlight back, which
  // by now has had a full frame of CPU time to finish.
  //
  // Measured in the first overworld map before this change, the fence wait was
  // 6.21 ms against 6.18 ms of GPU time: the CPU sat idle for the exact
  // duration of the GPU's work, every frame, and the frame cost the sum of the
  // two rather than the larger of them.
  g_slot = (g_slot + 1) % kFramesInFlight;
  FrameSlot& next = g_slots[g_slot];
  WaitForSlot(next);
  next.list_opened = false;

  // Safe only because of that wait: the frame that owned this slot's arena has
  // retired, so the next one can hand those bytes out again.
  BeginGuestDrawFrame(g_slot);

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
  // Draining the ring is what makes the queue idle now that a submitted frame
  // is no longer waited on where it was submitted.
  if (g_backend.queue)
    WaitForAllSlots();
  if (g_backend.swap_chain)
    g_backend.swap_chain->wait();

  ShutdownGuestDraws();
  ShutdownTextureMirror();
  ShutdownGuestPipelines();
  ShutdownFrameTargets();
  g_backend.framebuffers.clear();
  g_backend.release_semaphores.clear();
  g_backend.swap_chain.reset();
  for (FrameSlot& slot : g_slots) {
    slot.gpu_timer.reset();
    slot.acquire_semaphore.reset();
    slot.fence.reset();
    slot.command_list.reset();
    slot.submitted = false;
    slot.timer_open = false;
    slot.list_opened = false;
  }
  g_backend.queue.reset();
  g_backend.device.reset();
  g_backend.render_interface.reset();
}

}  // namespace eternalsonata
