// eternalsonata - ReXGlue Recompiled Project
//
// Native renderer: draws the guest through Plume by intercepting it at the
// Direct3D 9 API boundary, with its shaders compiled ahead of time (the
// inventory is a static blob in the xex; see scripts/extract_shaders.py).
// Nothing emulates Xenos, so there is no ring buffer, PM4 parsing or EDRAM.
//
// Selected by the `gpu_plugin` cvar, whose default is "plume" (settings.cpp).
// There is no rexgpu-plume DLL: the name is a sentinel that OnPostSetup clears
// from RuntimeConfig::gpu_plugin, so the SDK loads no plugin and runs headless.
// It then drives the guest's vblank interrupt from its own timer thread. Guest
// paths that write packets into a ring buffer are dead code and are
// intercepted or stubbed.

#pragma once

#include <cstdint>
#include <functional>

namespace rex::ui {
class Window;
}

namespace eternalsonata {

// The `gpu_plugin` value that selects this renderer. Not a real plugin name;
// nothing named rexgpu-plume is ever staged or loaded.
inline constexpr const char* kNativeRendererPluginName = "plume";

// True when the `gpu_plugin` cvar names this renderer, so no GPU plugin is
// loaded and this project owns presentation. Reads the cvar, so it is only
// meaningful once the config files have been loaded.
bool NativeRendererEnabled();

// Fraction of the window the 3D world is rendered at: 1.0 is the window's own
// resolution. Live, since the extent it feeds is republished every present.
// Always 1 on the Xenos path, which owns the cvars itself and does its own
// scaling.
float NativeRenderScale();

// The same number as it was at boot, for the one caller that runs before the
// window has ever published a size and so cannot retire anything.
float NativeRenderScaleAtBoot();

// True to scale the world image up to the window with nearest neighbour, for a
// sharp pixelated look at a low render scale, rather than the bilinear filter
// everything uses otherwise. Live, read per frame.
bool NativeRenderPixelatedScaling();

// Registers the cvars a GPU plugin would have registered, so selecting this
// renderer does not silently lose them: `vsync` and `resolution_scale`, both of
// which live inside rexgpu-xenos and therefore never register when no plugin is
// loaded. Call from OnPreSetup: after the config file has been read
// (so a saved value is still waiting to be applied to it) and before the SDK
// decides whether to load a plugin (so the plugin, if one is loaded instead,
// keeps sole ownership of the name). No-op unless NativeRendererEnabled().
void RegisterNativeRendererCvars();

// Called once per guest frame, from the swap hook, on the guest thread that
// issued the swap. The SDK fires its own per frame callback from the emulated
// GPU's swap packet, which this renderer does not have, so anything that would
// have ridden on it (the mod registry's tick above all) has to come from here.
//
// The interval is a guest frame, not a fixed period: it moves with the frame
// rate, and the framerate work makes that vary, so a callback must derive
// elapsed time from a clock rather than counting calls. No-op unless
// NativeRendererEnabled().
void SetGuestFrameCallback(std::function<void()> callback);

// Brings up the rendering backend. Call once the window exists and before the
// guest starts executing, so no guest D3D call can arrive ahead of it. Later
// calls do nothing. No-op unless NativeRendererEnabled().
void InitNativeRenderer(rex::ui::Window* window);

}  // namespace eternalsonata
