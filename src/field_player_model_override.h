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
// The substitution happens on the game's own (re)spawns: area transitions and
// the boot spawn. It deliberately does NOT force a respawn to update the model
// on the spot; forcing one leaves the character unable to move until the
// player opens and closes a menu, and this is a debug convenience, not
// something worth breaking control over. See the note in the .cpp.
#pragma once

namespace rex {
class Runtime;
}  // namespace rex

namespace eternalsonata {

class FieldPlayerModelOverride {
 public:
  // Present for symmetry with the other debug tools; nothing to register,
  // since the whole feature is the spawn hook. Call from OnPostSetup.
  static void Bind(rex::Runtime* runtime);

  // Selection, as indexed by the settings overlay's Field Leader Model combo:
  //   0      -- default, use the game's own model (Allegretto)
  //   1      -- follow the active party's first member
  //   2..11  -- force character 1..10
  static constexpr int kSelectionDefault = 0;
  static constexpr int kSelectionFollowParty = 1;
  static constexpr int kSelectionFirstCharacter = 2;
  static constexpr int kSelectionCount = 12;

  static void SetSelection(int selection);
  static int Selection();

  // Labels for the combo, kSelectionCount entries.
  static const char* const* SelectionNames();

  // Character number (1..10) the override currently resolves to, or 0 for
  // "leave the game's own model alone".
  static int DesiredCharacter();

  // Character number (1..10) of the active party's first member, or 0 if it
  // cannot be determined. Shown by the overlay.
  static int PartyLeaderCharacter();

  // Display name for a character number, or "?" if out of range.
  static const char* CharacterName(int character);
};

}  // namespace eternalsonata
