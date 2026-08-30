// eternalsonata - ReXGlue Recompiled Project
//
// See native_renderer.h for what this is and why the ring buffer is the thing
// being cut. So far this is only the bring-up hook; nothing draws yet.

#include "native_renderer.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/window.h>

#if defined(PLUME_SDL_VULKAN_ENABLED) || defined(__ANDROID__)
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

}  // namespace

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
}

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
#else
  void* handle = window ? window->GetNativeWindowHandle() : nullptr;
#endif
  REXLOG_INFO(
      "native_renderer: no GPU plugin loaded, so there is no ring buffer and the guest's D3D "
      "packet writers are dead code. Native window handle {}.",
      handle);

  // Loaded before the backend so a missing or stale pack is reported once, at
  // startup, rather than as a pile of failed pipelines at the first draw. It is
  // not fatal: without it the frame is still clears, resolves and the overlay,
  // which is what the renderer produced before geometry existed.
  if (!LoadGuestShaders()) {
    REXLOG_WARN(
        "native_renderer: no guest shader pack, so nothing the game draws can be translated into "
        "a host pipeline. Rebuild to regenerate guest_shaders.bin.");
  }

  if (!InitPlumeBackend(handle)) {
    REXLOG_WARN(
        "native_renderer: no host backend, so the game will run headless to a black window. "
        "Everything except presentation still works.");
  }
}

}  // namespace eternalsonata
