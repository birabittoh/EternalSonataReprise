// eternalsonata - The PS3's three extra Music gallery tracks.
#pragma once

#include <cstdint>

namespace eternalsonata {

// Grows the Music gallery's tabs to the PS3's layout. Call once the xex image
// is mapped and before the guest runs.
void ApplyPs3MusicTabs();

// The tab table the menu walks and its row stride.
uint32_t MusicTabTable();
uint32_t MusicTabStride();

// The highest track id the menu lists: 68 in PS3 mode, else the 360's 66.
int MusicTrackIdMax();

}  // namespace eternalsonata
