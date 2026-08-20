// eternalsonata - Guest memory layout of the battle system.
//
// Reverse-engineered from assets/default.xex (IDA database
// assets/default.xex.i64). Split out of debug_win_battle.cpp so anything else
// that needs to read battle state (an enemy HP overlay, a turn-order display)
// can share one definition instead of re-deriving the offsets.
//
// Everything hangs off the battle manager singleton dword_824D0440. Note that
// docs/script-vm-notes.md 5.3 quotes the record array bases as manager
// offsets, and its enemy figure is easy to misread: the enemy *array* starts
// at manager+429568 (unk_82539240), while the manager+461840/461848 figures in
// that section are the HP fields *inside* record 0 (unk_82541050 /
// unk_82541058). The constants below are expressed as an absolute array base
// plus an in-record offset, which is the form the game's own code uses.
//
// Confirmed readers, so these are decoded fields rather than guesses:
//   - sub_821B7450  battle-over predicate; scans kEnemyHpRatio over all live
//                   enemies and reports a win when every one is <= 0.
//   - sub_8218F4C8  low-health chatter; reads kEnemyHpRatio as a cur/max ratio
//                   against the same 0.2 threshold the party side uses.
//   - sub_82190900, sub_8219D330, sub_821CEB78
//                   all gate "is this enemy still up" on kEnemyHpRatio > 0.
//   - sub_821AA6D8  resolves the per-unit FSM object and returns its state.
#pragma once

#include <cstdint>

namespace eternalsonata {
namespace battle {

// Battle manager singleton (dword_824D0440).
inline constexpr uint32_t kManager = 0x824D0440u;

// Live unit counts, both u8. byte_824D0720 / byte_824D0721.
inline constexpr uint32_t kPartyCountAddr = 0x824D0720u;
inline constexpr uint32_t kEnemyCountAddr = 0x824D0721u;

// Party member records (unk_824FD1A0 == manager+183648).
// record[0] is the character id, 1..10.
inline constexpr uint32_t kPartyArrayBase = 0x824FD1A0u;
inline constexpr uint32_t kPartyRecordStride = 81972u;
inline constexpr uint32_t kPartyCharacterIdOffset = 0u;
// Three i32 in a row at unk_82510FE8 == manager+265128: level, current HP,
// maximum HP. NOT a two-field {max, cur} HP pair, which is what an earlier
// version of this header claimed - that mistake surfaced as a battle overlay
// showing each member's level where their maximum HP belonged (2026-08-19).
//
// The evidence for each:
//   +0  level. Observed live; no reader of it has been pinned down, so this
//       is the one field here resting on observation alone.
//   +4  current HP. sub_821B7450 walks manager+265132 on a stride of 20493
//       ints (== the 81972-byte record stride) and treats the party as wiped
//       once every one is <= 0. sub_821E7668, the post-battle write-back,
//       reads the same field as the HP to carry into the save.
//   +8  maximum HP. sub_8218F4C8 loads +4 and +8 as one qword and divides the
//       first by the second to get the cur/max ratio it tests against 0.2,
//       the same low-health threshold the enemy side uses.
inline constexpr uint32_t kPartyStatsOffset = 265128u - 183648u;
inline constexpr uint32_t kPartyLevelOffset = kPartyStatsOffset + 0u;
inline constexpr uint32_t kPartyHpCurOffset = kPartyStatsOffset + 4u;
inline constexpr uint32_t kPartyHpMaxOffset = kPartyStatsOffset + 8u;

// Enemy records (unk_82539240 == manager+429568).
//
// An enemy record is not flat. It is two 16136-byte "part" records back to
// back (32272 bytes, which is where the trailing fields start), and a record
// level index selects which part is live. Bosses that change form are why:
// sub_8219F4B8, the record constructor, memsets 32456 bytes and then inits
// exactly two parts through sub_8219F408.
//
//   record + 0                      part 0
//   record + 16136                  part 1
//   record + 32272 (0x7E10)  i32    live part index, 0 or 1
//   record + 32280 (0x7E18)  float  cached current/max HP ratio
//
// Every "which part" computation in the game is the same expression, e.g. in
// sub_8224EBD0 and sub_821C48D0:
//     part = record + 16136 * *(i32*)(record + 32272)
//
// Each part carries a 168-byte stat block at part+300. sub_8218F038 exists
// only to memcpy that block out for the script VM, which is what fixes both
// its base and its size. Inside it:
//
//   part + 300  i16  name id (1-based; see kEnemyNameBtxBlock below)
//   part + 302  i16  level
//   part + 304  i32  current HP
//   part + 308  i32  maximum HP
//
// How each was pinned down, since an earlier version of this header claimed
// the raw HP counters did not exist at all (three sweeps for them had found
// readers of the ratio only, because every access goes through the part
// indirection above and so never appears as a constant displacement):
//
//   name id    sub_821ABE88 resolves a unit's display name for both sides
//              from one descriptor: for a party member it feeds the record's
//              character id to the BTX block at 0x823857D0, and for an enemy
//              the i16 at part+300 to the block at 0x82332D90, both as
//              index - 1.
//   level      sub_821BAB70's battle-chatter picker compares the party's
//              maximum level against max over enemies of
//              SLOWORD(*(i32*)(part+300)). Careful: this is big-endian, so
//              the low half of the dword at +300 is the i16 at part+302, NOT
//              the one at +300 that sub_821ABE88 reads. The two fields sit
//              in the same dword and are easy to conflate.
//   current HP sub_8224EBD0's "target the weakest" AI sort picks the minimum
//              of, for a party member, kPartyHpCurOffset, and for an enemy,
//              part+304 - one comparison over both sides, so the two are the
//              same quantity by construction.
//   maximum HP sub_821B3FC0 computes (float)*(i32*)(part+308) * the ratio at
//              record+32280 and tests the product against 10.0, a "this one
//              is nearly dead" check. That product is only HP if the field
//              is the ratio's denominator.
//
// The ratio stays the field with the most readers (the battle-over predicate
// among them, see below), and it is consistent with the pair: max * ratio ==
// current. Anything writing HP should keep all three in step.
inline constexpr uint32_t kEnemyArrayBase = 0x82539240u;
inline constexpr uint32_t kEnemyRecordStride = 32456u;
inline constexpr uint32_t kEnemyPartStride = 16136u;
inline constexpr uint32_t kEnemyPartCount = 2u;
inline constexpr uint32_t kEnemyPartIndexOffset = 0x7E10u;
inline constexpr uint32_t kEnemyHpRatioOffset = 0x7E18u;
// Within a part record.
inline constexpr uint32_t kEnemyStatsOffset = 300u;
inline constexpr uint32_t kEnemyStatsSize = 168u;
inline constexpr uint32_t kEnemyNameIdOffset = 300u;  // i16, 1-based
inline constexpr uint32_t kEnemyLevelOffset = 302u;   // i16
inline constexpr uint32_t kEnemyHpCurOffset = 304u;   // i32
inline constexpr uint32_t kEnemyHpMaxOffset = 308u;   // i32

// The BTX text block enemy names are resolved from, via the game's own text
// lookup sub_8223B780(block, name_id - 1). Party names come from the block at
// 0x823857D0 instead, but nothing here needs those: the party API already
// answers them, and it honours per-character renames that this would not.
inline constexpr uint32_t kEnemyNameBtxBlock = 0x82332D90u;

// Address helpers. Callers still have to bounds-check against the live counts.
inline constexpr uint32_t PartyRecord(uint32_t slot) {
  return kPartyArrayBase + kPartyRecordStride * slot;
}
inline constexpr uint32_t EnemyRecord(uint32_t slot) {
  return kEnemyArrayBase + kEnemyRecordStride * slot;
}
inline constexpr uint32_t EnemyHpRatio(uint32_t slot) {
  return EnemyRecord(slot) + kEnemyHpRatioOffset;
}
inline constexpr uint32_t EnemyPartIndexAddr(uint32_t slot) {
  return EnemyRecord(slot) + kEnemyPartIndexOffset;
}
// The live part's base. `part_index` is the value read from
// EnemyPartIndexAddr, which callers should bounds-check against
// kEnemyPartCount - a garbage index would address outside the record.
inline constexpr uint32_t EnemyPart(uint32_t slot, uint32_t part_index) {
  return EnemyRecord(slot) + kEnemyPartStride * part_index;
}

// --- Battle FSM ----------------------------------------------------------
//
// sub_821ACBF8 is the battle state machine. Its current state lives at
// manager+533040: every "advance to state N" in the function is a `li r11, N`
// branching to loc_821ACF64, which is a single `stw r11, 0(r18)` into that
// field. States are 1..23, dispatched through the u16 offset table at
// word_82082730 (targets are loc_821ACEDC + entry, so state == index + 1).
//
// A state's body runs EVERY frame it is current, not once on entry. The
// dispatch does sit behind `if (manager+533040 != manager+537136)`, which reads
// like an on-entry guard, but nothing in the function ever writes 537136: it
// holds 26 in every state, which is outside the 1..23 range, so the comparison
// is always unequal and the guard never fires. State 12 is the proof from the
// other direction, being held for the whole of every turn while calling
// sub_821AB1A0 on each of those frames. Do not expose 537136 as a previous
// state; it is not one.
//
// All 23 states below were walked out of the dispatch table on 2026-08-19, and
// the ones marked "live" were also watched on a running battle. Each entry
// gives what the state does and what it advances to; "holds" means the state
// stays put until the named condition is met.
//
// Getting into the battle:
//
//    1  teardown of whatever the previous battle left behind: releases 7+2
//       render handles, calls sub_8219F7E0, then goes to 2 unconditionally.
//    2  holds until the scene-ready byte at manager+0x2E3 is set, then 3.
//    3  the big one-shot setup, ~340 instructions: runs sub_8219F698 over
//       every enemy record, resolves the encounter's BOP file, zeroes the
//       per-unit scratch in all party (stride kPartyRecordStride) and enemy
//       (kEnemyRecordStride) records, and initialises the camera and the four
//       unit-FSM tables (sub_821A90D8/91F0/92F8/9400). Goes to 4, EXCEPT when
//       the tutorial block at manager+0x83238 is armed, in which case it calls
//       sub_821B8840 and jumps straight to 7. Tutorial battles have no intro.
//    4  intro setup: zeroes the sequence object at manager+0x83208, calls
//       sub_821BAB70, seeds the phase byte to 1 or 3 depending on whether the
//       object's enemy index is set, then 5.
//    5  the battle intro (live): the camera circles while both sides say their
//       opening lines. The whole body is one call, sub_821BB310 on that
//       sequence object, which advances the state itself when the intro ends.
//
// The turn loop, 7 -> 6 -> 8 -> 9 -> 10 -> 11 -> 12 -> 13 -> 7. Note that 7
// runs BEFORE 6 despite the numbering:
//
//    7  turn start (live): resolves the acting unit through sub_821980D0 and
//       asks sub_821905D0 whether it is still valid, retargeting via
//       sub_821AA500 if not. Normally goes to 6. The exception is the branch
//       where the outcome code at manager+0x83249 is 3, which instead polls
//       sub_821AB1A0 and goes to 14.
//    6  turn setup: refreshes the status/effect passes, clears the per-unit
//       turn scratch on both sides, computes the turn order (sub_821ABF70) and
//       wipes the 16-entry action queues, then 8.
//    8  swings the camera onto the acting unit (sub_821ABBB0 to place it,
//       sub_8217CEA0 to move it, sub_8219D568 over 200 frames), then 9.
//    9  holds until sub_821ACB18 says that camera move is done, then 10.
//   10  opens the actor's turn (sub_821AA320), then 11.
//   11  holds while the actor's pending action code (sub_821AA6D8) is 0x21 or
//       0x23, i.e. while the command is still being chosen. Once something is
//       committed it fires sub_821AA500, bumps the turn counter at
//       manager+0x8324C, arms the turn-mode object at manager+0x8226C and
//       goes to 12.
//   12  ACTUAL GAMEPLAY (live), and where a battle spends nearly all of its
//       time: it is held for the entire duration of every turn, the player's
//       and the enemies' alike. On entry it kicks the encounter's tutorial
//       script (btldata\script\tutorial\tNNNN.e via sub_821BBED8, guarded by a
//       done-byte so it fires once), then every frame it polls sub_821AB1A0
//       and checks the turn-mode object; when that says the turn is over it
//       writes 13. This is the state a forced win lands in, and the state the
//       kActorHolderOffset descriptor below describes.
//   13  end of turn (live), entered only when 12 hands off to it. Holds until
//       sub_821AA730 agrees the action has finished resolving, then tears down
//       the FDOT effect, clears both sides' per-unit turn flags, applies the
//       end-of-turn passes (sub_8218D8C8/DBB0/DE18/DF88) and goes back to 7.
//
// Leaving the battle. sub_821AB1A0, called from 7, 12 and 13, is what decides
// the battle is over: it polls sub_821B7450, and once that agrees it plays out
// the victory pose for 300 or 600 frames and then writes state 17. Being in
// 12 is therefore necessary but nowhere near sufficient.
//
//   17  the victory flourish, and NOT a direct route to 18: it stops the
//       effects, calls sub_821B4D90 and sub_821AB7E0, and goes to 14.
//   14  the outcome branch, and the only place the three endings are told
//       apart. It reads the outcome code at manager+0x83249 and the acting
//       unit's kind:
//         code 3            -> 16, the party escaped
//         kind 1 (an enemy) -> 15, the party lost
//         otherwise         -> 18, the party won
//       On the winning path it also runs the closing camera work
//       (sub_821B51C8, sub_821B5410, sub_8218B5C0, sub_8218B6B8).
//   15  DEFEAT (live), and terminal as far as this FSM is concerned: it calls
//       sub_821A61F0 forever and never writes another state. The game-over
//       hand-off happens inside that object, not here.
//   16  ESCAPED: counts dword_82565CA8 up and holds for 100 frames or until
//       the player presses the skip button, then goes straight to 22. Fleeing
//       therefore skips 18 through 21 entirely: no results, no rewards, but
//       the HP write-back still runs.
//
// On the winning path 18 starts a fixed chain:
//
//   18  end of battle proper (sub_821AC0C0)
//   19  the level-up / results screens (sub_82198450)
//   20  a branch, not a screen: reads the required party level from the
//       encounter record (manager+0x68DFC, halfword at +0x34) and compares it
//       against the current one (dword_8243F3EC's low byte). Below it, opens
//       the party-level-up screen (sub_821C3B78) and goes to 21; otherwise
//       skips straight to 22. Most battles therefore never show 20 or 21.
//   21  the party-level-up screen while it is up (sub_821C3D50)
//   22  the write-back, and only ever a frame or two long: walks the live
//       party records, floors each member's current HP at 1 so nobody leaves
//       the battle KO'd, and calls sub_821E7668 to push the battle HP pair
//       back into the save's party stats.
//   23  teardown (sub_821AC2A8)
//
// Anything watching for "the battle is ending" should watch for >= 18 rather
// than for one state: 20 and 21 are conditional, and 22 is easy to miss. That
// test deliberately excludes 15 and 16, which are the losing and escaping
// endings and never reach 18.
inline constexpr uint32_t kFsmStateOffset = 533040u;
// Getting in.
inline constexpr uint32_t kFsmStateReset = 1u;
inline constexpr uint32_t kFsmStateWaitScene = 2u;
inline constexpr uint32_t kFsmStateSetup = 3u;
inline constexpr uint32_t kFsmStateIntroSetup = 4u;
inline constexpr uint32_t kFsmStateIntro = 5u;
// The turn loop. 7 runs before 6; see the walkthrough above.
inline constexpr uint32_t kFsmStateTurnStart = 7u;
inline constexpr uint32_t kFsmStateTurnSetup = 6u;
inline constexpr uint32_t kFsmStateActorCamera = 8u;
inline constexpr uint32_t kFsmStateActorCameraWait = 9u;
inline constexpr uint32_t kFsmStateTurnOpen = 10u;
inline constexpr uint32_t kFsmStateCommandWait = 11u;
// Gameplay. The one to test against for "the battle is actually being played".
inline constexpr uint32_t kFsmStateTurn = 12u;
inline constexpr uint32_t kFsmStateTurnEnd = 13u;
// The outcome branch: from here the battle goes to exactly one of 15, 16 or 18.
inline constexpr uint32_t kFsmStateOutcome = 14u;
// Defeat and escape. Neither is part of the victory chain below, so
// FsmStateIsEndingBattle() deliberately does not cover them.
inline constexpr uint32_t kFsmStateDefeat = 15u;
inline constexpr uint32_t kFsmStateEscaped = 16u;
// The wrap-up chain above. Once the machine is at or past this the battle is
// already decided and nothing can be done about it.
inline constexpr uint32_t kFsmStateVictoryPose = 17u;
inline constexpr uint32_t kFsmStateEndOfBattle = 18u;
inline constexpr uint32_t kFsmStateLevelUp = 19u;
inline constexpr uint32_t kFsmStatePartyLevelCheck = 20u;
inline constexpr uint32_t kFsmStatePartyLevelUp = 21u;
inline constexpr uint32_t kFsmStateWriteBack = 22u;
inline constexpr uint32_t kFsmStateTeardown = 23u;

// The outcome code state 14 branches on, and the one value of it that means
// anything on its own: 3 is "the party escaped". States 7 and 14 are its only
// readers, and 14 checks it before it checks anything else.
inline constexpr uint32_t kOutcomeCodeOffset = 0x83249u;
inline constexpr uint8_t kOutcomeCodeEscaped = 3u;

// The tutorial block. Byte 0 is the tutorial id (1 based, indexing
// off_8238E330's tNNNN.e paths), byte 1 is the "already kicked" latch, and +4
// holds the script handle. State 3 skips the intro entirely when byte 0 is
// set, which is why tutorial battles never show state 4 or 5.
inline constexpr uint32_t kTutorialOffset = 0x83238u;

// Whether a battle is running, straight from the machine that runs it.
//
// This works because the FSM is never torn down: state 23's body
// (sub_821AC2A8) ends with a literal `manager+533040 = 2`, parking the machine
// on the "hold until the scene-ready byte is set" state to wait for the next
// encounter. So between battles the state reads 2, during a battle it reads 3
// through 23, and before the first one it reads 0 out of zeroed static memory.
// State 1 goes to 2 unconditionally in the same frame, so counting it as idle
// costs nothing and keeps the boundary at one comparison.
//
// Prefer this to any of the scene-mode globals. The applied mode reads 3
// ("field") for a battle's whole duration and only turns 4 as the battle ends,
// and the requested mode is cleared faster than one rendered frame; see
// room_presence.cpp for the full write-up of that dead end.
inline constexpr bool FsmStateIsInBattle(uint32_t state) {
  return state >= kFsmStateSetup && state <= kFsmStateTeardown;
}

// True once the battle is decided and the wrap-up chain has started.
inline constexpr bool FsmStateIsEndingBattle(uint32_t state) {
  return state >= kFsmStateEndOfBattle && state <= kFsmStateTeardown;
}

// Per-enemy flag bits at record+0x154; sub_8219D330 tests bit 9 alongside the
// HP ratio when deciding whether an enemy is a valid target.
inline constexpr uint32_t kEnemyFlagsOffset = 0x154u;

// --- Battle intro --------------------------------------------------------
//
// FSM state 5 (kFsmStateIntro) ticks one object, at manager+0x83208, through
// sub_821BB310. On screen this is the pre-battle exchange: each side's units
// deliver a line while the camera circles them.
//
// The object runs a phase counter in its first byte, 1 -> 2 -> 3 -> 4. Do NOT
// read that as a one-shot sequence with 4 as "the end" - it is closer to one
// pass per speaker, and the enemies' half plays out in full inside phases 1
// and 2 before phase 4 is ever reached:
//
//   1  starts the enemy side: sub_82178EA0 against the enemy record indexed
//      by the object's +4 (the stride there is 8114 dwords, i.e. the 32456
//      byte kEnemyRecordStride, which is what identifies +4 as an enemy
//      index). Goes straight to 2.
//   2  the enemy's line and camera work. Spins up a motion handle into +16
//      once the +20 timer passes 50, then waits for the voice line
//      (sub_821431C0 on dword_8243D89C) to end before clearing the handle,
//      resetting the timer and moving to 3.
//   3  one frame of sub_821BB060 setup, then 4.
//   4  the party side, driven by sub_821BAFB0 off the object's +8 (a party
//      index) and +12 (a motion id), on the same "start a motion once the
//      timer passes 50" pattern. Finishes via sub_821BB140 once EITHER the
//      timer passes 600.0 OR the player presses the skip button (sub_82199408
//      against bit 0x1000).
//
// The +20 timer is NOT a frame count. sub_82181728 returns 300 / byte_82465F90
// per tick, so the unit is 1/300 s and at 60 fps the timer advances 5.0 per
// frame (measured live: timer=495 at t=1.71s). The thresholds above are
// therefore 50.0 = 0.17 s of lead-in before a line starts, and 600.0 = a 2
// second cap, not the 10 seconds a 60 Hz reading would suggest.
//
// Both of phase 4's finish conditions are gated on bit 0 of dword_8253923C[19]
// being clear: story battles set it and are unskippable, and only the voice
// line running out ends those.
//
// sub_821BB140 is the finisher, and it is what the skip button reaches. It
// takes no real argument (it works off globals; the call site passes the
// object in r3 anyway), repositions the units and camera, and hands off to
// sub_821BA360. It does NOT write manager+533040 itself - the FSM leaves
// state 5 on its own once the intro is done.
//
// Crucially for anything forcing a skip, sub_821BB140 opens with
// sub_821A9508, which walks every live unit on BOTH sides and calls the
// unit's own FSM vtable slot 4 to return it to idle (26 -> 27, 35 -> 36,
// anything else -> 8). That is why calling it from phase 1 or 2 is fine: the
// enemy-side animations it interrupts are torn down by the same routine that
// handles the party side. It does not stop an in-flight voice line, but
// neither does the game's own skip button.
inline constexpr uint32_t kIntroObjectOffset = 0x83208u;
inline constexpr uint32_t kIntroPhaseOffset = 0u;            // u8, 1..4
inline constexpr uint32_t kIntroEnemyIndexOffset = 4u;       // i32
inline constexpr uint32_t kIntroPartyIndexOffset = 8u;       // i32
inline constexpr uint32_t kIntroMotionIdOffset = 12u;        // i32
inline constexpr uint32_t kIntroSequenceHandleOffset = 16u;  // i32, < 0 = none
inline constexpr uint32_t kIntroTimerOffset = 20u;           // float, 1/300 s
inline constexpr uint32_t kIntroPhasePartySide = 4u;

inline constexpr uint32_t IntroObject() { return kManager + kIntroObjectOffset; }

// --- Turn / action state -------------------------------------------------
//
// manager+533120 holds the object whose +196 points at the acting unit's
// descriptor, a {u32 kind, u8 slot} pair. kind 0 is a party member and 1 an
// enemy; sub_821980D0 substitutes 2 when nothing is acting.
inline constexpr uint32_t kActorHolderOffset = 533120u;
inline constexpr uint32_t kActorDescriptorOffset = 196u;
inline constexpr uint32_t kActorKindParty = 0u;
inline constexpr uint32_t kActorKindEnemy = 1u;

// The battle-over predicate sub_821B7450 is ASYMMETRIC in the acting unit's
// kind, which matters to anything trying to force a win:
//
//   kind 0 (a party member is acting)  ->  scans ENEMY hp, all <= 0 == victory
//   kind 1 (an enemy is acting)        ->  scans PARTY hp, all <= 0 == defeat
//
// It never asks whether the enemies are dead while an enemy holds the turn.
// (The decompiler renders the test as HIDWORD of the descriptor loaded as one
// qword; on big-endian that high dword is the first four bytes, i.e. kind.)
// So killing every enemy during an enemy's turn wedges the battle: the acting
// enemy can no longer finish, the actor never flips back to a party member,
// and the victory branch is never evaluated. Wait for kind 0.

// Per-unit FSM object pointers indexed by slot, stride 4, state at object+4.
// sub_821AA6D8 indexes these as 4 * (slot + 133261) and 4 * (slot + 133264).
inline constexpr uint32_t kUnitFsmPartyBase = 533044u;
inline constexpr uint32_t kUnitFsmEnemyBase = 533056u;
inline constexpr uint32_t kUnitFsmStateOffset = 4u;
// sub_821AA6D8's "no such unit" sentinel.
inline constexpr uint32_t kUnitStateNone = 101u;

// Unit FSM states during which an action is resolving. sub_821B7450 refuses to
// end the battle while the acting unit is in one of these, which is why
// anything that wants to force a battle to end has to wait them out.
inline constexpr bool UnitIsResolvingAction(uint32_t state) {
  return state >= 16u && (state <= 17u || (state - 68u) <= 5u);
}

}  // namespace battle
}  // namespace eternalsonata
