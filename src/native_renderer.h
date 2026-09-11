// eternalsonata - ReXGlue Recompiled Project
//
// Native renderer: the beginnings of a renderer that does not emulate Xenos.
//
// The stock path loads the "xenos" GPU plugin, which emulates the GPU at the
// hardware level: it stands up a ring buffer, the guest's D3D runtime writes
// PM4 packets into it, and the plugin parses those packets and translates the
// shader microcode at runtime. The plan here is to intercept the game one
// level higher, at the Direct3D API boundary, and to compile the game's
// shaders ahead of time (the whole inventory is a static blob in the xex; see
// scripts/extract_shaders.py).
//
// The two are mutually exclusive, and the ring buffer is the reason: it exists
// only because Xenos is being emulated. Select the native renderer by setting
// the `gpu_plugin` cvar to "plume" (its default is "xenos", set in
// settings.cpp). There is no rexgpu-plume DLL: the name is a sentinel this
// project recognises in OnPreSetup, where it clears RuntimeConfig::gpu_plugin
// so the SDK loads no plugin at all. With no plugin loaded
// the SDK runs headless: it drives the guest's vblank interrupt from its own
// timer thread, and there is no ring buffer, so every guest path that writes a
// packet into one is dead code that must be intercepted or stubbed rather than
// executed.
//
// This is deliberately staged. Step one, which is what is implemented so far,
// is the cut itself: the game boots to a black screen and keeps running. The
// D3D device hooks, the device struct layout, shader constant tracking,
// texture/surface mirrors and finally the resolve/swap/present chain get built
// on top of that, in that order.

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

// Brings up the rendering backend. Call from OnPreLaunchModule, before the
// guest starts executing and so before any guest D3D call can arrive.
// No-op unless NativeRendererEnabled(); there is no backend behind it yet.
void InitNativeRenderer(rex::ui::Window* window);

}  // namespace eternalsonata
