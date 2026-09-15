// eternalsonata - ReXGlue Recompiled Project
//
// The host side of the native renderer: a Plume device and swap chain on the
// game's own window.
//
// This is the half that actually owns pixels. native_renderer_d3d.cpp watches
// what the guest asks for; this is where it will eventually be turned into real
// draws. Right now it does the minimum that proves the path end to end -- it
// acquires, clears and presents once per guest swap -- which is what turns the
// black window into something that visibly tracks the game.
//
// Why this exists at all rather than the SDK's own presenter: with no GPU
// plugin loaded the SDK sets `config.graphics` to null and never creates a
// presenter, on the understanding that the app brings its own renderer. So
// nothing draws, not even the F3/F4 overlays, until something here does.
//
// Deliberately kept behind a plain C++ interface with no Plume types in the
// header. Plume pulls in d3d12.h and windows.h, and confining that to one
// translation unit keeps it out of the guest-facing code.

#pragma once

#include <cstdint>

namespace rex::ui {
class UIDrawer;
}

namespace eternalsonata {

// Bring up a Plume device and a swap chain on `window_handle` (an HWND on
// Windows, an SDL_Window* on Linux, an ANativeWindow* on Android, an NSWindow*
// on Apple). Safe to call once; returns false and logs if the backend could not
// be created, in which case every other entry point here is a no-op and the
// game runs headless exactly as it did before.
//
// `window_view` is only read on Apple, where Plume's RenderWindow is a
// {NSWindow*, CAMetalLayer*} pair rather than a single handle and the layer is
// what actually becomes the Vulkan surface. Every other platform passes null.
bool InitPlumeBackend(void* window_handle, void* window_view = nullptr);

// True once InitPlumeBackend has succeeded.
bool PlumeBackendReady();

// Android hands out a new ANativeWindow every time the app returns to the
// foreground, and releases the old one on the way out; a swap chain built on
// the old one presents to nothing, which is a black window with the game still
// running behind it. Both only record the request: the swap chain is not
// thread safe, so the work happens on the next present, the same way a resize
// does.
void PlumeSurfaceLost();
void PlumeSurfaceRestored(void* window_handle);

// Hand over the SDK's ImGui drawer so the overlays can record into the frame.
// Until this is called the frame is only the clear. Null disables them again.
void PlumeSetOverlayDrawer(rex::ui::UIDrawer* drawer);

// Draw and present one host frame. Called from the guest's Swap hook, so the
// host frame rate follows the guest's, which is what we want until there is
// anything to decouple.
void PlumePresentFrame();

// Submit whatever the frame has recorded so far and wait for it, without
// presenting. The frame carries on recording into a fresh command list
// afterwards.
//
// This exists for one caller: the readback path, when the guest reads a resolve
// destination in the very frame it was resolved. The copy into the readback
// buffer is recorded but has not run, and the only way to answer the guest with
// this frame's pixels rather than the last frame's is to make the GPU catch up.
// It is a full stall, which is why it is on demand and not a frame boundary;
// the SDK's own `readback_resolve=full` is the same trade.
//
// Guest render thread only, since that is the thread that records the frame.
// False when there was nothing recorded or the backend is not up.
bool PlumeFlushGuestWork();

// How many frames may be recorded and submitted before the CPU blocks on the
// oldest one's fence.
//
// One would be the old behaviour: record a frame, submit it, wait for it, start
// the next. That made the frame cost CPU + GPU end to end, with the CPU idle
// for the whole GPU half and the GPU idle for the whole CPU half. Measured in
// the first overworld map, that was 16.1 ms of CPU and 6.2 ms of GPU making a
// 22.2 ms frame; the fence wait matched the GPU time to within 0.03 ms, which
// is what a total absence of overlap looks like.
//
// Two lets frame N's GPU work run while the CPU records frame N+1, so the frame
// is max(CPU, GPU) rather than their sum. Everything the GPU reads out of a
// frame's own resources therefore has to exist once per slot: the command list,
// the fence, the acquire semaphore, the timestamp pool, the upload arena and
// the readback buffers. More than two would buy nothing here (the CPU half is
// more than twice the GPU half, so the ring is never the constraint) and would
// cost another arena and another set of readback buffers.
inline constexpr uint32_t kFramesInFlight = 2;

// Which slot the frame currently being recorded owns, in [0, kFramesInFlight).
uint32_t PlumeFrameSlot();

// Whether the GPU work recorded during guest frame `frame` is known to have
// completed.
//
// This is the honest version of "the previous frame is done", which is what the
// readback path used to assume and what frames in flight took away. Ask this
// rather than comparing against FrameIndex() - 1 before reading anything the
// GPU wrote. Frames are counted by the frame layer's FrameIndex().
bool PlumeFrameRetired(uint64_t frame);

// The window changed size; the swap chain has to follow. Called from the app's
// pixel size hook, i.e. on the UI thread, so it only records the request and
// the next present acts on it.
void PlumeNotifyResize(uint32_t pixel_width, uint32_t pixel_height);

// Release the device and swap chain. Called on shutdown, before the window
// goes away.
void ShutdownPlumeBackend();

}  // namespace eternalsonata
