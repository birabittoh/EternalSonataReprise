// eternalsonata - Host state kept with each save slot.
//
// Some state has no room in the guest save: the costume worn, and the PS3's
// costume unlocks and camp menu flags. It is written as key = value lines to
// reprise.txt inside the slot's save container, so deleting the save deletes
// it too. A save without one loads with everything at its default.
#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace eternalsonata {

using SaveRecord = std::map<std::string, std::string>;

// sub_82241190 started a save: take the state as it is now.
void CaptureSaveRecord();
// That save reached the container: write what was captured.
void CommitSaveRecord(int slot);
// sub_82240AF8(save) restored the guest globals; `save` is its argument.
void NotifySaveRecordLoaded(uint32_t save);

}  // namespace eternalsonata
