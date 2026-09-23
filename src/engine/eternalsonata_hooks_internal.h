// eternalsonata - ReXGlue Recompiled Project
//
// Shared declarations between eternalsonata_hooks.cpp, eternalsonata_framerate.cpp,
// and eternalsonata_options.cpp.

#pragma once

#include "generated/eternalsonata_init.h"

namespace eternalsonata_hooks {

// Defined in eternalsonata_options.cpp (the memory-differ debug tool). Polled
// from the present hook in eternalsonata_framerate.cpp so the manual
// F9-F12 hotkeys work from anywhere, not only while a menu is up.
void ScanPollKeys(u8* base);
void ScanTick(u8* base);

// Also defined in eternalsonata_options.cpp, and called from the same present
// hook for the same reason: it has to keep running after the Options screen is
// gone, which is precisely when the menu's own per-frame hooks stop firing.
// Handles the relaunch owed to a restart-scoped setting changed from the
// main-menu Options screen.
void OptionsTick();

// Defined in eternalsonata_hooks.cpp. Given a string the BTX lookup returned,
// yields our PC wording of it, or 0 to leave it alone. Called from the
// sub_8223B780 hook, which lives in eternalsonata_options.cpp because a guest
// function can only be hooked once.
u32 ConsoleTextOverrideFor(u8* base, u32 text_address);

// Defined in eternalsonata_framerate.cpp: the wall-clock stepping behind the
// "unlocked" frame rate. FrameDeltaScale is applied by the sub_82181728 hook
// in eternalsonata_lipsync.cpp (the delta getter can only be hooked once);
// the two fixups run at the top of the profiler's zone hooks on sub_820C8378
// and sub_820C9550 for the same reason.
bool WallClockActive();
double FrameDeltaScale();
void AnimEntryFixup(PPCContext& ctx, u8* base);
void AnimPartFixup(PPCContext& ctx, u8* base);

}  // namespace eternalsonata_hooks
