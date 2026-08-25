// eternalsonata - ReXGlue Recompiled Project
//
// See native_renderer.h for what this is and why the ring buffer is the thing
// being cut. So far this is only the bring-up hook; nothing draws yet.

#include "native_renderer.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/window.h>

#include "guest_shaders.h"
#include "native_renderer_plume.h"

namespace eternalsonata {

bool NativeRendererEnabled() { return rex::cvar::GetFlagByName("gpu_plugin").empty(); }

void InitNativeRenderer(rex::ui::Window* window) {
  if (!NativeRendererEnabled())
    return;

  // The window is already created and is ours to render into however we like;
  // the SDK is not presenting the guest in this mode, and will not draw its own
  // overlays either until we hand it a drawer.
  void* handle = window ? window->GetNativeWindowHandle() : nullptr;
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
