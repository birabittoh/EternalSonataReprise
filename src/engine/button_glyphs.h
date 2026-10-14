// eternalsonata - Xbox, PlayStation or keyboard and mouse button art over the release's own.
#pragma once

#include <cstdint>

namespace button_glyphs {

// Applies the button_glyphs cvar whenever it changes.
void Register();

// AppKeep.bmd and its slot array are in place: applies the current choice.
void OnAppKeepLoaded();

// BattleKeep.bop's slot array (a guest address) is filled: applies the current
// choice to the battle HUD's button sheet.
void OnBattleKeepLoaded(uint32_t slot_array);

// Whether the face buttons drawn by the host (touch controls) are the
// PlayStation ones: the setting's choice, or the release's own for auto.
bool UsePlayStationArt();

// Follows the prompt_device setting: in auto, swaps between the pad and the
// keyboard and mouse art as the player switches. Call once per frame from one thread.
void Update();

}  // namespace button_glyphs
