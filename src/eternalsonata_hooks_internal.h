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

}  // namespace eternalsonata_hooks
