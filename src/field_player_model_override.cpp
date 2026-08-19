#include "field_player_model_override.h"

#include "force_load_area.h"
#include "generated/eternalsonata_init.h"

#include "settings.h"

#include <atomic>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/system/kernel_state.h>

namespace {

// Character number -> cached model-handle slot address, per sub_821A2B38's
// computed-goto dispatch (raw-disasm'd at 0x821A2C90-0x821A2DA4; Hex-Rays
// stubs the table itself to `bctr` so this had to be read case-by-case).
// Not evenly strided: character 9 (tag "bCLV", Claves) uses B20, out of
// sequence with 10 ("bMCH", March) at B1C.
//
// The numbering matches the party code's character numbers exactly (see
// ../EternalSonataReprise-Mods/src/party_overlay/mod_main.cpp, which takes it
// from the game's own name table at 0x8203304B).
constexpr uint32_t kCharacterSlotAddr[10] = {
    0x82420AFCu,  // 1  bALG  Allegretto
    0x82420B00u,  // 2  bPLK  Polka
    0x82420B04u,  // 3  bBET  Beat
    0x82420B08u,  // 4  bCPN  Frederic
    0x82420B0Cu,  // 5  bVOL  Viola
    0x82420B10u,  // 6  bSLS  Salsa
    0x82420B14u,  // 7  bJRB  Jazz
    0x82420B18u,  // 8  bFST  Falsetto
    0x82420B20u,  // 9  bCLV  Claves
    0x82420B1Cu,  // 10 bMCH  March
};

constexpr const char* kCharacterNames[11] = {
    "(none)", "Allegretto", "Polka",    "Beat",   "Frederic", "Viola",
    "Salsa",  "Jazz",       "Falsetto", "Claves", "March",
};

// u32[10] keyed by character number: dword_8243FC08[c - 1] is character c's
// 1-based display position on the status screen, 0 meaning "not in the
// party". It is a position table, not a list of ids in party order -- see the
// derivation in party_overlay/mod_main.cpp. The active party is whoever holds
// positions 1..3, so the party's first member is the character whose entry
// equals 1.
constexpr uint32_t kStatusMemberList = 0x8243FC08u;
constexpr uint32_t kStatusMemberCount = 10u;
constexpr uint32_t kPartyLeaderPosition = 1u;

// Object kind at object+8; sub_820EE7D8 tags exactly kind 1 as "PC", the
// field-controlled character.
constexpr uint32_t kObjectKindPC = 1u;

// Map-manager singleton (sub_820EBA28 calls
// sub_820FCF80((int)&dword_8244B4B0, *a1, 0)), the live field-leader object at
// +1520, and the party slot sub_820FCF80 caches its a2 in.
constexpr uint32_t kMapManager = 0x8244B4B0u;
constexpr uint32_t kFieldObjectPtrOffset = 1520u;
constexpr uint32_t kCurrentPartySlot = 0x8243C270u;
// byte_8243C368: when set, sub_820F9EC8 takes its full map-reset branch (which
// re-runs sub_820FCF80 itself) instead of the plain resume.
constexpr uint32_t kMapResetFlag = 0x8243C368u;

// The map manager's second field object (mm+1528, the one sub_820FCF80's
// forced path pairs with the leader). Non-null is what gates the two
// sub_820F6420 calls that crash; see kSceneHandleTable below.
constexpr uint32_t kSecondObjectPtrOffset = 1528u;
// Field objects carry their scene-handle id at +4 and a "handle is live"
// marker at +20. The game only ever looks a handle up after checking that
// the byte at +20 is 1 (sub_820FCF80 does it twice, immediately after the
// sub_820F6420 pair; sub_820F6420 does it for the leader object).
constexpr uint32_t kObjectHandleIdOffset = 4u;
constexpr uint32_t kObjectHandleLiveOffset = 20u;
constexpr uint8_t kObjectHandleLive = 1u;
// The scene object registry sub_8217BED0 resolves handle ids against.
constexpr uint32_t kSceneHandleTable = 0x824CF500u;

// sub_8217BED0: scene-handle id -> object pointer, 0 when the id is not
// registered. See the crash note on the respawn call below.
REX_IMPORT(__imp__sub_8217BED0, ResolveSceneHandle, u32(u32, u32));

// Whether sub_820FCF80's forced path can safely run right now.
//
// Crash (access violation reading guest 0x140): with a3=1 (force),
// sub_820FCF80 reaches `sub_820F6420(leader, {1,0}, sub_820FBEB8, second)`,
// which passes the *second* object's handle id down to sub_82179A48 and on
// into sub_82172B20. sub_82172B20 opens with
// `*(sub_8217BED0(&dword_824CF500, id) + 320)` and never checks the result,
// so an unregistered id faults at 0x140. sub_820F6420 checks +16/+20 on the
// leader object but nothing at all on the second one, and the game's own
// callers only reach this branch when the party slot actually changed, so
// they never hit an id in that state. Forcing from here can.
bool ForcedRespawnIsSafe() {
  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  const uint32_t second = REX_LOAD_U32(kMapManager + kSecondObjectPtrOffset);
  if (second == 0 || second == 0xFFFFFFFFu) {
    // sub_820FCF80 skips the whole sub_820F6420 block when this is null.
    return true;
  }
  if (REX_LOAD_U8(second + kObjectHandleLiveOffset) != kObjectHandleLive) {
    return false;
  }
  return ResolveSceneHandle(kSceneHandleTable,
                            REX_LOAD_U32(second + kObjectHandleIdOffset)) != 0;
}

// The game's own field-leader respawn. a3 != 0 forces it to run even when the
// party slot is unchanged. Importing the plain symbol routes back through this
// file's sub_820EE7D8 hook, so the model substitution applies to it.
REX_IMPORT(sub_820FCF80, RespawnFieldLeader, void(uint32_t, uint32_t, uint32_t));

// Persisted value, one token per combo entry and in the same order. Defined
// (with the matching .allowed list) in settings.cpp alongside the other
// curated options, and saved with them via kBasicCvarNames.
constexpr const char* kSelectionTokens[] = {
    "default", "party", "allegretto", "polka",    "beat",   "frederic",
    "viola",   "salsa", "jazz",       "falsetto", "claves", "march",
};

constexpr const char* kSelectionNames[] = {
    "Default", "Party Leader", "Allegretto", "Polka",    "Beat",   "Frederic",
    "Viola",   "Salsa",        "Jazz",       "Falsetto", "Claves", "March",
};
static_assert(sizeof(kSelectionNames) / sizeof(kSelectionNames[0]) ==
                  eternalsonata::FieldPlayerModelOverride::kSelectionCount,
              "combo labels must match kSelectionCount");

// Mirror of the field_leader_model cvar. The spawn hook reads this on the
// guest thread on every PC spawn, so it is kept as a plain atomic rather than
// re-reading (and re-allocating) the cvar string there; SetSelection and
// LoadFromCvar are the only writers.
std::atomic<int> g_selection{eternalsonata::FieldPlayerModelOverride::kSelectionDefault};

// Character whose model the live field object is currently using, 0 meaning
// the game's own. Guest thread only; -1 forces the next check to respawn,
// which is how a selection change is made to take effect.
int g_applied_character = -1;

}  // namespace

namespace eternalsonata {

void FieldPlayerModelOverride::SetSelection(int selection) {
  if (selection < 0 || selection >= kSelectionCount) {
    return;
  }
  g_selection.store(selection, std::memory_order_relaxed);
  rex::cvar::SetFlagByName("field_leader_model", kSelectionTokens[selection]);
  SaveUserSettings();
}

int FieldPlayerModelOverride::Selection() {
  return g_selection.load(std::memory_order_relaxed);
}

const char* const* FieldPlayerModelOverride::SelectionNames() {
  return kSelectionNames;
}

int FieldPlayerModelOverride::DesiredCharacter() {
  const int selection = g_selection.load(std::memory_order_relaxed);
  if (selection == kSelectionDefault) {
    return 0;
  }
  if (selection == kSelectionFollowParty) {
    return PartyLeaderCharacter();
  }
  return selection - kSelectionFirstCharacter + 1;
}

const char* FieldPlayerModelOverride::CharacterName(int character) {
  if (character < 0 || character > 10) {
    return "?";
  }
  return kCharacterNames[character];
}

int FieldPlayerModelOverride::PartyLeaderCharacter() {
  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  for (uint32_t i = 0; i < kStatusMemberCount; ++i) {
    if (REX_LOAD_U32(kStatusMemberList + i * 4u) == kPartyLeaderPosition) {
      return static_cast<int>(i) + 1;
    }
  }
  return 0;
}

void FieldPlayerModelOverride::Bind(rex::Runtime* /*runtime*/) {
  // Pick up the persisted value once the config files have been loaded. An
  // unrecognised token falls back to "default" rather than guessing.
  const std::string value = rex::cvar::GetFlagByName("field_leader_model");
  int selection = kSelectionDefault;
  for (int i = 0; i < kSelectionCount; ++i) {
    if (value == kSelectionTokens[i]) {
      selection = i;
      break;
    }
  }
  g_selection.store(selection, std::memory_order_relaxed);
}

}  // namespace eternalsonata

// ---------------------------------------------------------------------------
// sub_820EE7D8 is the field-object model instantiator: r4/a2 is a pre-resolved
// character "template" resource handle (one of the kCharacterSlotAddr slots
// above, populated per character by sub_821A2B38's tag-string lookup), and
// *(u16*)(r3/a1 + 8) is the object's kind (1 == "PC"). Substituting r4 before
// the real function runs makes the spawn use a different character's model;
// everything downstream (positioning, weapon/animation setup) reads the
// resulting instance back out of the object, so nothing else needs to change.
// The game itself always passes Allegretto's slot for the field leader,
// regardless of party order.
//
// Only the game's own spawns are hooked -- boot and area transitions. Forcing
// a respawn through sub_820FCF80 to update the model on the spot does swap it,
// but leaves the character unable to move until the player opens and closes a
// menu, and the re-arm sequence for that was never found. Ruled out by
// testing: sub_820FBDB8's body (it never runs on a menu close, only on
// transitions); the camera rebind from sub_820F9EC8's `if (*(mm + 2560))`
// branch (darkens the whole scene); the sub_820FD8C0 pause cycle, both
// immediate and spread over frames; and calling the real sub_820EE010 /
// sub_820E8710 menu pair (blacks the screen out). State dumps also ruled out
// mm+36, the 0x200000 pause bit, the dword_82440590 object list and the camera
// binding -- post-respawn state matched a working one except for the leader's
// new model instance and control block. The open remaining lead is
// sub_820FA2F8, the state machine keyed on mm+1513.
//
// Since party order can only be changed from the status screen, picking the
// new model up on the next area load is enough for a debug tool.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// The PC object is persistent: an area transition respawns the map and NPCs
// but never the kind-1 field leader, so the hook below only fires at boot on
// its own. Something has to force sub_820FCF80 to re-run for a party change to
// show up.
//
// Doing that standalone leaves the character unable to move (see the long
// note on the spawn hook). The fix falls out of *how* the player worked around
// it: open the status menu, swap the model, close the menu. The respawn has to
// happen while the field is suspended, with the game's own resume running
// afterwards.
//
// sub_820F9EC8 is that resume, and it is reached on exactly the two edges that
// matter -- closing a menu (via sub_820E8710) and finishing a transition (via
// sub_820FBDB8). Respawning here, before the original runs, reproduces the
// working sequence exactly: the field is still suspended at this point, and
// the resume immediately after re-arms control the same way it does for a
// hand-closed menu.
// ---------------------------------------------------------------------------
REX_EXTERN(__imp__sub_820F9EC8);

REX_HOOK_RAW(sub_820F9EC8) {
  // Check if there's a pending area warp to apply
  {
    std::lock_guard<std::mutex> lock(eternalsonata::GetWarpMutex());
    if (eternalsonata::GetWarpPending()) {
      eternalsonata::GetWarpPending() = false;
      const uint32_t encoded_dword = eternalsonata::GetWarpEncodedDword();
      REX_STORE_U32(0x8243C230u, encoded_dword);
      // Set spawn position bytes to 0 (safe default that's usually handled by area)
      // Normal code sets these from dword_82081BC8[area_index], we use 0 as fallback
      REX_STORE_U8(0x8243CB69u, 0x00);
      REX_STORE_U8(0x8243CB6Au, 0x00);
      REX_STORE_U8(0x8243CB6Bu, 0x00);
      REX_STORE_U8(0x8243CB6Cu, 0x00);
      REX_STORE_U8(0x8243C368u, 1u);
    }
  }

  const uint8_t reset_flag = REX_LOAD_U8(kMapResetFlag);

  // Character 0 means "off" and is a valid target: respawning with the
  // override disabled restores the game's own model, which is what makes
  // switching the combo back to (off) actually take effect.
  const int character = eternalsonata::FieldPlayerModelOverride::DesiredCharacter();
  const uint32_t object = REX_LOAD_U32(kMapManager + kFieldObjectPtrOffset);
  // Skip the reset branch: it re-runs sub_820FCF80 itself, so a respawn here
  // would be redundant, and the spawn hook picks up the new model anyway.
  // ForcedRespawnIsSafe covers the forced path's unguarded handle lookup. When
  // it says no, leaving g_applied_character alone means the next resume edge
  // retries, so the model still lands as soon as the scene is settled.
  if (character != g_applied_character && object != 0 && object != 0xFFFFFFFFu &&
      REX_LOAD_U8(kMapResetFlag) == 0 && ForcedRespawnIsSafe()) {
    RespawnFieldLeader(kMapManager, REX_LOAD_U32(kCurrentPartySlot), 1u);
  }
  __imp__sub_820F9EC8(ctx, base);
}

REX_EXTERN(__imp__sub_820EE7D8);

REX_HOOK_RAW(sub_820EE7D8) {
  if (REX_LOAD_U16(ctx.r3.u32 + 8) == kObjectKindPC) {
    const int character = eternalsonata::FieldPlayerModelOverride::DesiredCharacter();
    // 0 means leave the game's own handle alone; record it so the resume hook
    // knows the live object is back on the default model.
    if (character == 0) {
      g_applied_character = 0;
    } else if (character >= 1 && character <= 10) {
      const u32 handle = REX_LOAD_U32(kCharacterSlotAddr[character - 1]);
      // An uncached slot means sub_821A2B38 never resolved that character's
      // model; substituting it would instantiate a null resource, so fall
      // through to the handle the game chose.
      if (handle != 0 && handle != 0xFFFFFFFFu) {
        ctx.r4.u32 = handle;
        g_applied_character = character;
      }
    }
  }
  __imp__sub_820EE7D8(ctx, base);
}
