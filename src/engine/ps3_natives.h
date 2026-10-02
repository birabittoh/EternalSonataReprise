// eternalsonata - PS3 only state behind natives 5026..5033.
#pragma once

#include <cstdint>

#include "save_record.h"

namespace eternalsonata {

// Save record part, the camp menu flags; reset is a new game's state.
void ResetPs3Record();
void SavePs3Record(SaveRecord& record);
void LoadPs3Record(const SaveRecord& record);

}  // namespace eternalsonata
