// eternalsonata - Debug tool: makes the overworld leader use the model of
// whoever is first in the active party.
//
// The game always spawns Allegretto as the field-controlled character
// (sub_820FCF80 passes dword_82420AFC for field slot 0 regardless of party
// order). This hooks the spawn and substitutes the cached model handle of the
// party's first member instead, so reordering the party from the status
// screen -- the only place the game lets you reorder it -- is reflected in the
// overworld.
//
// The game also hides the field leader's weapon mesh, by party slot rather than
// by model, so a substituted model keeps a weapon the overworld should not show.
// The .cpp extends that hide list to cover whichever model actually spawned.
//
// The substitution happens on the game's own (re)spawns: area transitions and
// the boot spawn. It deliberately does NOT force a respawn to update the model
// on the spot; forcing one leaves the character unable to move until the
// player opens and closes a menu, and this is a debug convenience, not
// something worth breaking control over. See the note in the .cpp.
#pragma once

#include <string>

namespace rex {
class Runtime;
}  // namespace rex

namespace eternalsonata {

class FieldPlayerModelOverride {
 public:
  // Present for symmetry with the other debug tools; nothing to register,
  // since the whole feature is the spawn hook. Call from OnPostSetup.
  static void Bind(rex::Runtime* runtime);

  // Selection, as indexed by the settings overlay's Overworld Model combo:
  //   0      -- default, use the game's own model (Allegretto)
  //   1      -- follow the active party's first member
  //   2..13  -- force character 1..12 (11 and 12 on PS3 data only)
  static constexpr int kSelectionDefault = 0;
  static constexpr int kSelectionFollowParty = 1;
  static constexpr int kSelectionFirstCharacter = 2;
  static constexpr int kSelectionCount = 14;

  // Selections the current target offers: the first SelectionCount() of
  // SelectionNames().
  static int SelectionCount();

  static void SetSelection(int selection);
  static int Selection();

  // Labels for the combo, kSelectionCount entries.
  static const char* const* SelectionNames();

  // Character number (1..12) the override currently resolves to, or 0 for
  // "leave the game's own model alone".
  static int DesiredCharacter();

  // Character number (1..12) of the active party's first member, or 0 if it
  // cannot be determined. Shown by the overlay.
  static int PartyLeaderCharacter();

  // Display name for a character number, "(none)" for 0, "?" for nobody.
  static std::string CharacterName(int character);

  // Respawns the leader on the next field tick outside a cutscene, so a model
  // whose data changed under the same character is rebuilt. Thread safe.
  static void RequestRespawn();
};

}  // namespace eternalsonata
