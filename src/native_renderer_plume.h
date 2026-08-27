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
// Windows). Safe to call once; returns false and logs if the backend could not
// be created, in which case every other entry point here is a no-op and the
// game runs headless exactly as it did before.
bool InitPlumeBackend(void* window_handle);

// True once InitPlumeBackend has succeeded.
bool PlumeBackendReady();

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

// The window changed size; the swap chain has to follow. Called from the app's
// pixel size hook, i.e. on the UI thread, so it only records the request and
// the next present acts on it.
void PlumeNotifyResize(uint32_t pixel_width, uint32_t pixel_height);

// Release the device and swap chain. Called on shutdown, before the window
// goes away.
void ShutdownPlumeBackend();

}  // namespace eternalsonata
