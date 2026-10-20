// eternalsonata - The progression_curves setting (progression_curves.cpp).
#pragma once

namespace eternalsonata {

// Writes the chosen release's growth curves into the party template when the
// setting changed since the last call. Cheap otherwise.
void ApplyProgressionCurves();

}  // namespace eternalsonata
