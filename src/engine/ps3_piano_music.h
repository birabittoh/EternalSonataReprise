// eternalsonata - The PS3's eight Piano Music pieces.
#pragma once

#include <cstdint>

namespace eternalsonata {

// Grows the Piano Music menu to the PS3's eight pieces, Little Dog Waltz
// fifth. Call once the xex image is mapped and before the guest runs.
void ApplyPs3PianoMusic();

// How many pieces the menu lists: 8 in PS3 mode, else the 360's 7.
int PianoMusicCount();

// The flag id table the menu walks, PianoMusicCount() entries.
uint32_t PianoMusicFlagTable();

// The edited copy of a Piano Music screen list, or 0 to keep `list`.
uint32_t Ps3PianoMusicList(uint32_t list);

}  // namespace eternalsonata
