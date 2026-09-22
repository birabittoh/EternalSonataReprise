// eternalsonata - Aiming input tweaks; see aim_input.cpp.
#pragma once

namespace eternalsonata {

// Drops the mouse-look override once the aim hook stops running. Called once
// per guest frame from the render pump's present hook.
void AimInputTick();

}  // namespace eternalsonata
