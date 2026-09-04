// eternalsonata - ReXGlue Recompiled Project
//
// See native_renderer.h for what this is and why the ring buffer is the thing
// being cut. So far this is only the bring-up hook; nothing draws yet.

#include "native_renderer.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <string>

#if defined(PLUME_SDL_VULKAN_ENABLED) || defined(__ANDROID__) || defined(__APPLE__)
#include <SDL3/SDL.h>
#endif

#include "guest_shaders.h"
#include "native_renderer_plume.h"

namespace eternalsonata {

bool NativeRendererEnabled() {
  // Latched on the first call, which is RegisterNativeRendererCvars from
  // OnPreSetup: after the config files have been read and before anything else
  // asks. Deliberately not a live read.
  //
  // Which renderer is running is decided once, at boot, by whether the SDK
  // loaded a GPU plugin; changing gpu_plugin at runtime only records a
  // preference for the next launch. A live read would flip every hook in
  // native_renderer_d3d.cpp out from under a guest still running with no
  // plugin behind it, and the next D3DDevice__BlockUntilFenceRetired would
  // then spin forever on a retired-fence counter nothing writes.
  static const bool enabled =
      rex::cvar::GetFlagByName("gpu_plugin") == kNativeRendererPluginName;
  return enabled;
}

namespace {

// Storage behind the `vsync` cvar registered below. Deliberately a mirror of
// what REXCVAR_DEFINE_BOOL would have produced, down to the setter accepting
// anything and treating everything but true/1/yes as false, so the cvar reads
// and writes exactly like the Xenos plugin's own.
bool g_vsync = true;

// Storage behind `resolution_scale`, same idea. See RegisterNativeRendererCvars.
int32_t g_resolution_scale = 1;

constexpr int32_t kMinRenderScale = 1;
constexpr int32_t kMaxRenderScale = 8;

}  // namespace

uint32_t NativeRenderScale() {
  // Latched, like NativeRendererEnabled: a host render target's size is part of
  // its identity (see GuestTarget in native_renderer_frame.cpp), so changing
  // this mid-run would leave every target the previous frames drew into keyed at
  // the old size. The cvar is kRequiresRestart for the same reason on Xenos.
  static const uint32_t scale = [] {
    if (!NativeRendererEnabled())
      return 1u;
    const int32_t value = std::clamp(g_resolution_scale, kMinRenderScale, kMaxRenderScale);
    if (value > 1)
      REXLOG_INFO("native_renderer: rendering at {}x the guest's 1280x720", value);
    return uint32_t(value);
  }();
  return scale;
}

void RegisterNativeRendererCvars() {
  if (!NativeRendererEnabled())
    return;

  // Same name, type, category, description and default as
  // REXCVAR_DEFINE_BOOL(vsync, true, "GPU", ...) in the SDK's
  // command_processor.cpp, which is compiled into rexgpu-xenos and so only
  // exists when that plugin is loaded. Registering it here rather than
  // statically is what keeps the two from colliding: a static definition would
  // win the registry slot before the plugin ever loaded, leaving the plugin's
  // own storage frozen at whatever it read at registration time.
  //
  // Anything the config file, command line or environment set for `vsync` is
  // still pending at this point and gets applied by RegisterFlag, as is the
  // game's own default for it (kGameDefaults in settings.cpp), which is what
  // makes this default the same under either renderer.
  rex::cvar::FlagEntry entry;
  entry.name = "vsync";
  entry.type = rex::cvar::FlagType::Boolean;
  entry.category = "GPU";
  entry.description = "Enable vertical sync";
  entry.setter = [](std::string_view value) {
    g_vsync = value == "true" || value == "1" || value == "yes";
    return true;
  };
  entry.getter = []() { return std::string(g_vsync ? "true" : "false"); };
  entry.command_callback = [](std::string_view) {};
  entry.lifecycle = rex::cvar::Lifecycle::kHotReload;
  entry.default_value = "true";
  rex::cvar::RegisterFlag(std::move(entry));

  // Same again for `resolution_scale`, which the Xenos plugin defines in
  // graphics/pipeline/texture/cache.cpp and which is therefore missing entirely
  // under this renderer. That is not cosmetic: the settings overlay's
  // Resolution row and the game's own Options screen both write it alongside
  // `resolution` (SetResolutionSetting in settings.cpp), so without it the
  // window grew and the game kept rendering 720p.
  //
  // Type, category, range, default and lifecycle match the SDK's definition, so
  // a settings.toml round-trips between the two renderers unchanged.
  rex::cvar::FlagEntry scale;
  scale.name = "resolution_scale";
  scale.type = rex::cvar::FlagType::Int32;
  scale.category = "GPU";
  scale.description =
      "Draw resolution scale for both X and Y axes (same as setting "
      "draw_resolution_scale_x and draw_resolution_scale_y)";
  scale.setter = [](std::string_view value) {
    int32_t parsed = 0;
    const char* begin = value.data();
    const auto result = std::from_chars(begin, begin + value.size(), parsed);
    if (result.ec != std::errc())
      return false;
    g_resolution_scale = std::clamp(parsed, kMinRenderScale, kMaxRenderScale);
    return true;
  };
  scale.getter = []() { return std::to_string(g_resolution_scale); };
  scale.command_callback = [](std::string_view) {};
  scale.lifecycle = rex::cvar::Lifecycle::kRequiresRestart;
  scale.constraints.min = kMinRenderScale;
  scale.constraints.max = kMaxRenderScale;
  scale.default_value = "1";
  rex::cvar::RegisterFlag(std::move(scale));
}

#if defined(__ANDROID__)
namespace {

// Reads the current ANativeWindow back out of SDL. Only valid to call once SDL
// has published a new one, which is what OnSurfaceRestored signals.
void* CurrentAndroidWindowHandle() {
  int window_count = 0;
  SDL_Window** windows = SDL_GetWindows(&window_count);
  void* handle = nullptr;
  if (windows && window_count > 0) {
    handle = SDL_GetPointerProperty(SDL_GetWindowProperties(windows[0]),
                                    SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
  }
  SDL_free(windows);
  return handle;
}

// The swap chain is built on the ANativeWindow captured at init, and Android
// throws that away whenever the app stops being the focused foreground
// activity. Without rebuilding it the game keeps running, and presenting, into
// a surface that no longer exists: audio continues and the window is black.
class SurfaceListener final : public rex::ui::WindowListener {
 public:
  void OnSurfaceLost(rex::ui::UIEvent&) override { PlumeSurfaceLost(); }
  void OnSurfaceRestored(rex::ui::UIEvent&) override {
    PlumeSurfaceRestored(CurrentAndroidWindowHandle());
  }
};

// Outlives the window on purpose: it is removed only at process exit, which is
// also when the window goes away.
SurfaceListener g_surface_listener;

}  // namespace
#endif  // __ANDROID__

void InitNativeRenderer(rex::ui::Window* window) {
  if (!NativeRendererEnabled())
    return;

  // The window is already created and is ours to render into however we like;
  // the SDK is not presenting the guest in this mode, and will not draw its own
  // overlays either until we hand it a drawer.
  //
  // Plume's SDL/Vulkan backend takes an SDL_Window* directly (it creates its
  // own VkSurfaceKHR via SDL_Vulkan_CreateSurface, which already knows how to
  // talk to whichever platform backend SDL picked, X11 or Wayland). Plain
  // GetNativeWindowHandle() only ever returns a platform-native handle
  // (HWND on Windows); on Linux it's always null, since there's no single
  // native handle to hand back, and the SDK exposes no accessor for the
  // SDL_Window behind rex::ui::WindowSDL. Ask SDL instead: the app only ever
  // opens the one window, so the first entry is ours.
#if defined(PLUME_SDL_VULKAN_ENABLED)
  void* handle = nullptr;
  if (window) {
    int window_count = 0;
    SDL_Window** windows = SDL_GetWindows(&window_count);
    if (windows && window_count > 0) {
      handle = windows[0];
    }
    // SDL hands back a fresh array; only the SDL_Window pointers inside it are
    // owned by SDL and outlive it.
    SDL_free(windows);
  }
#elif defined(__ANDROID__)
  // Plume's Android backend expects an ANativeWindow*. SDL exposes this as a
  // window property; GetNativeWindowHandle() returns null on non-Win32.
  void* handle = nullptr;
  if (window) {
    int window_count = 0;
    SDL_Window** windows = SDL_GetWindows(&window_count);
    if (windows && window_count > 0) {
      handle = SDL_GetPointerProperty(
          SDL_GetWindowProperties(windows[0]),
          SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
    }
    SDL_free(windows);
  }
#elif defined(__APPLE__)
  // Plume's RenderWindow is a {NSWindow*, CAMetalLayer*} pair here, and the
  // layer is what vkCreateMetalSurfaceEXT needs. SDL's window has no layer of
  // its own, so attach one. Both SDL calls are main-thread only, which
  // OnPreLaunchModule is.
  //
  // Not routed through PLUME_SDL_VULKAN_ENABLED even though this is still
  // Vulkan: Plume checks that macro before __APPLE__ in the same #elif chain,
  // so defining it would pick the Linux shape. See CMakeLists.txt.
  void* handle = nullptr;
  void* view = nullptr;
  if (window) {
    int window_count = 0;
    SDL_Window** windows = SDL_GetWindows(&window_count);
    if (windows && window_count > 0) {
      handle = SDL_GetPointerProperty(SDL_GetWindowProperties(windows[0]),
                                      SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
      // Leaked at exit on purpose: the swap chain reads the layer until the
      // last present, and SDL wants the view destroyed before its window,
      // which only SDL's own teardown can order.
      static SDL_MetalView metal_view = nullptr;
      metal_view = SDL_Metal_CreateView(windows[0]);
      if (metal_view != nullptr) {
        view = SDL_Metal_GetLayer(metal_view);
      } else {
        REXLOG_ERROR("native_renderer: SDL_Metal_CreateView failed: {}", SDL_GetError());
      }
    }
    SDL_free(windows);
  }
#else
  void* handle = window ? window->GetNativeWindowHandle() : nullptr;
#endif
#if defined(__APPLE__)
  REXLOG_INFO(
      "native_renderer: no GPU plugin loaded, so there is no ring buffer and the guest's D3D "
      "packet writers are dead code. Native window handle {}, Metal layer {}.",
      handle, view);
#else
  REXLOG_INFO(
      "native_renderer: no GPU plugin loaded, so there is no ring buffer and the guest's D3D "
      "packet writers are dead code. Native window handle {}.",
      handle);
#endif

  // Loaded before the backend so a missing or stale pack is reported once, at
  // startup, rather than as a pile of failed pipelines at the first draw. It is
  // not fatal: without it the frame is still clears, resolves and the overlay,
  // which is what the renderer produced before geometry existed.
  if (!LoadGuestShaders()) {
    REXLOG_WARN(
        "native_renderer: no guest shader pack, so nothing the game draws can be translated into "
        "a host pipeline. Rebuild to regenerate guest_shaders.bin.");
  }

#if defined(__APPLE__)
  if (!InitPlumeBackend(handle, view)) {
#else
  if (!InitPlumeBackend(handle)) {
#endif
    REXLOG_WARN(
        "native_renderer: no host backend, so the game will run headless to a black window. "
        "Everything except presentation still works.");
  }

#if defined(__ANDROID__)
  if (window) {
    window->AddListener(&g_surface_listener);
  }
#endif
}

}  // namespace eternalsonata
