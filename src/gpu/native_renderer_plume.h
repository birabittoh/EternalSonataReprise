// eternalsonata - ReXGlue Recompiled Project
//
// The host side of the native renderer: a Plume device and swap chain on the
// game's own window. native_renderer_d3d.cpp watches what the guest asks for;
// this turns it into draws and presents once per guest swap.
//
// With no GPU plugin the SDK creates no presenter, so nothing draws, including
// the F3/F4 overlays, until this does.
//
// Kept behind a plain C++ interface with no Plume types in the header: Plume
// pulls in d3d12.h and windows.h, which stay confined to one translation unit.

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

// Present a frame holding only the overlays, for the loading screen shown while
// the UI thread is busy before the guest runs. Waits for its own GPU work, so it
// never occupies a frame slot the guest's first frames expect to find free.
// Must not be called once the guest is presenting.
void PlumePresentOverlayOnly();

// Submit whatever the frame has recorded so far and wait for it, without
// presenting. The frame carries on recording into a fresh command list
// afterwards.
//
// Used by the readback path when the guest reads a resolve destination in the
// frame it was resolved. The copy into the readback buffer is recorded but has
// not run, and answering with this frame's pixels rather than the last frame's
// needs the GPU to catch up. It is a full stall, so it is on demand and not a
// frame boundary; the SDK's `readback_resolve=full` is the same trade.
//
// Guest render thread only, since that is the thread that records the frame.
// False when there was nothing recorded or the backend is not up.
bool PlumeFlushGuestWork();

// How many frames may be recorded and submitted before the CPU blocks on the
// oldest one's fence.
//
// Two lets frame N's GPU work run while the CPU records frame N+1, so the frame
// costs max(CPU, GPU) rather than their sum. One would record, submit and wait
// each frame (in the first overworld map, 16.1 ms of CPU plus 6.2 ms of GPU
// made a 22.2 ms frame). Everything the GPU reads out of a frame's own
// resources therefore exists once per slot: the command list, the fence, the
// acquire semaphore, the timestamp pool, the upload arena and the readback
// buffers. More than two would buy nothing, since the CPU half is more than
// twice the GPU half, and would cost another arena and set of readback
// buffers.
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
