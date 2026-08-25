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
// only because Xenos is being emulated. Selecting the native renderer is
// therefore nothing more than setting the `gpu_plugin` cvar to the empty
// string (its default is "xenos", set in settings.cpp). With no plugin loaded
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

namespace rex::ui {
class Window;
}

namespace eternalsonata {

// True when no GPU plugin is loaded, i.e. the `gpu_plugin` cvar is empty, so
// this project owns presentation. Reads the cvar, so it is only meaningful
// once the config files have been loaded.
bool NativeRendererEnabled();

// Brings up the rendering backend. Call from OnPreLaunchModule, before the
// guest starts executing and so before any guest D3D call can arrive.
// No-op unless NativeRendererEnabled(); there is no backend behind it yet.
void InitNativeRenderer(rex::ui::Window* window);

}  // namespace eternalsonata
