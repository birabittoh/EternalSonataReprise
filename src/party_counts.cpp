// eternalsonata - The other half of raising the cast from ten to twelve.
//
// party_relocation.{h,cpp} and the generated hook table move the per-character
// arrays so twelve entries fit. That is necessary and not sufficient: the game
// also *counts* to ten, in code that has nothing to do with where the arrays
// live. This file is the hand-written half, one entry per site, because unlike
// the relocation sites these are not one uniform shape and there are few enough
// of them to read individually.
//
// Three shapes appear:
//
//   li rX, 0xA      a loop trip count, loaded then handed to mtctr or counted
//                   down. Hooked *after* the li, which simply rewrites rX.
//
//   cmpwi rX, 0xA   a loop bound or an id range check. A mid-ASM hook cannot
//                   change the immediate, so the hook fires *before* the
//                   compare and takes the branch itself via jump_address_on_true
//                   when the extended range applies. The original branch is
//                   left in place and still handles the first ten.
//
//   nothing at all  the reset paths clear the position array as ten individual
//                   stores, so entries 11 and 12 of the relocated twelve-entry
//                   array are never written. A hook stores the two missing
//                   zeroes. This one would present as two phantom party members
//                   rather than as a crash.
//
// The matching [[midasm_hook]] blocks live in eternalsonata_config.toml, outside
// the generated block. The table below is the index:
//
//   address     after  hook                             notes
//   0x821E5C10  yes    PartyCount_Twelve(r10)           sub_821E5A38 slotbytes clear
//   0x821E5DCC  yes    PartyCount_Twelve(r11)           sub_821E5D68 slotbytes clear
//   0x821E7190  yes    PartyCount_Twelve(r21)           sub_821E7138 character init
//   0x821E7628  yes    PartyCount_Twelve(r10)           sub_821E7358 charwords clear
//   0x821E7648  yes    PartyCount_Twelve(r10)           sub_821E7358 charflags clear
//   0x821E5AA0  yes    PartyCount_ClearPositionTail_    sub_821E5A38 reset path;
//                        RebaseSlotbytes(r31)           merged, see below
//   0x821E5DB4  yes    PartyCount_ClearPositionTail_    sub_821E5D68 reset path;
//                        RebaseSlotbytes(r31)           merged, see below
//   0x821E7690  no     PartyCount_QueueScan(r31)        jump_address_on_true = 0x821E767C
//   0x821E7EB0  no     PartyCount_QueueScan(r30)        jump_address_on_true = 0x821E7E9C
//   0x821E76F4  no     PartyCount_ExtendedId(r11)       jump_address_on_true = 0x821E76FC
//   0x821DD940  no     PartyCount_LayoutTen(r27)        jump_address_on_true = 0x821DD948
//
// Still missing, and deliberately not guessed at here: sub_821FA908, the party
// select screen, which holds a ten-element display-order table on the stack,
// iterates it with a literal ten, pads with `if (count < 10)`, and pulls widget
// handles from a UI layout resource at fixed indices. Two more characters need
// two more widgets in the asset, so that one is not a hook table at all.

#include "party_relocation.h"
#include "party_slots.h"

#include <rex/ppc/context.h>

namespace {

using eternalsonata::kRelocatedCharacterCount;

using eternalsonata::kPositionBase;
using eternalsonata::kPositionStride;

// How many entries the game's own reset paths clear. They store ten and stop,
// so the two past that are this file's job.
constexpr uint32_t kPositionClearedCount = 10;

}  // namespace

// The hook bodies are looked up by name by the recompiler, so they keep
// external linkage.

// Every `li rX, 0xA` that is a per-character trip count. Fired after the
// instruction, so the register holds the ten the game just loaded.
void PartyCount_Twelve(PPCRegister& reg) {
    reg.u64 = kRelocatedCharacterCount;
}

// The reset paths clear position[0..9] as ten individual stores and stop. The
// relocated array has twelve entries, and nothing in the game writes the last
// two, so without this they keep whatever the reserved page held -- which reads
// as two extra party members with junk display positions.
void PartyCount_ClearPositionTail() {
    for (uint32_t i = kPositionClearedCount; i < kRelocatedCharacterCount; ++i) {
        const uint32_t address = kPositionBase + i * kPositionStride;
        auto* slot = eternalsonata::PartyGuestPointer(address);
        if (!slot) {
            // Before the reservation ran, which should be impossible on the
            // reset path. Leaving the slot alone is the safe half of the bad
            // outcome: a stale entry rather than a write through nullptr.
            return;
        }
        *reinterpret_cast<uint32_t*>(slot) = 0;
    }
}

// Both reset paths clear the position array as ten consecutive stores through
// one register and then walk straight on into the slotbytes clear, still through
// the same register. The generated table treats that as one run: the register is
// shifted once before the run and restored once after, and where it crosses from
// position into slotbytes the delta is corrected by the difference. That
// correction lands on the last position store -- which is exactly the
// instruction this file already hooks. A mid-ASM hook is keyed by instruction
// address and only one can live at each, so the two jobs are merged here rather
// than fighting over the slot.
//
// The emitter knows about this merge (MERGED_WITH_COUNT_HOOK in
// scripts/party_relocation_emit.py) and registers this name at those two
// addresses instead of its own, while still generating the body called below.
void PartyRebase_position_To_slotbytes(PPCRegister& reg);

void PartyCount_ClearPositionTail_RebaseSlotbytes(PPCRegister& base) {
    PartyCount_ClearPositionTail();
    PartyRebase_position_To_slotbytes(base);
}

// `cmpwi rX, 0xA` at the top of a scan over the ten-slot pending-award queue,
// with `blt` back to the loop head. Returning true takes that branch for the
// two new slots; the original instruction still handles the first ten.
bool PartyCount_QueueScan(PPCRegister& index) {
    return index.u32 < kRelocatedCharacterCount;
}

// sub_821DD808 builds the party HUD's command list, and picks a layout by how
// many members are in the party:
//
//   ble  loc_821DDCF4      count == 0, nothing to draw
//   <= 3 loc_821DDC84      sub_821DDD00 emitters
//   <= 6 loc_821DDBB0      sub_821DED50
//   <= 9 loc_821DDAC0      sub_821E1608
//   == 10 fall through     sub_821E3408
//   otherwise -> loc_821DDCF4, which is the return
//
// Eleven or twelve members hit that last line and the function returns having
// emitted only its four header records. That is not merely a missing layout: the
// per-layout emitters are what append the command VM's 0xFFFF terminator, so the
// list is left unterminated. sub_821F2F38 dispatches `opcode / 100` through the
// table at 0x82026428, and anything it cannot match falls into loc_821F4BC4,
// which re-reads the same record without advancing and jumps back to the
// dispatcher. An unterminated list is therefore an infinite loop, which is the
// softlock on adding an eleventh party member: the VM parks on a record whose
// first word is 570, bucket 5 handles only 500 through 503, and it spins there
// forever.
//
// This takes the ten-member layout for eleven and twelve, by jumping into it
// rather than by clamping the count: r27 is also this function's return value,
// and the caller should still be told the real party size. Ten portraits are
// drawn and the eleventh and twelfth are not, which is the honest outcome until
// the ten-wide UI layout resource itself grows -- the emitters place members by
// display position 1..10 and there is no widget behind position 11.
bool PartyCount_LayoutTen(PPCRegister& count) {
    return count.u32 > 10 && count.u32 <= kRelocatedCharacterCount;
}

// `cmpwi r11, 0xA` / `bgt` rejecting character ids above ten before the award
// is applied. The low end is already checked by the `cmpwi r11, 1` above it, so
// this only has to let the added ids through to the code the bgt skips -- and
// only those an actual mod has defined, so an unclaimed slot is rejected here
// exactly as retail rejected it.
bool PartyCount_ExtendedId(PPCRegister& id) {
    return id.u32 > 10 && id.u32 <= kRelocatedCharacterCount &&
           eternalsonata::IsCharacterDefined(static_cast<int>(id.u32));
}
