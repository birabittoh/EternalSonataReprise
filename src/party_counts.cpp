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
//                   stores, so entries 11 and 12 -- which live in the bytes the
//                   relocated slotbytes array vacated -- are never written. A
//                   hook stores the two missing zeroes. This one would present
//                   as two phantom party members rather than as a crash.
//
// The matching [[midasm_hook]] blocks are NOT in eternalsonata_config.toml yet,
// for the same reason the relocation table is not: half a conversion is a
// broken game. The table below documents each hook's address and options so the
// config can be written in the same commit that enables everything.
//
//   address     after  hook                             notes
//   0x821E5C10  yes    PartyCount_Twelve(r10)           sub_821E5A38 slotbytes clear
//   0x821E5DCC  yes    PartyCount_Twelve(r11)           sub_821E5D68 slotbytes clear
//   0x821E7190  yes    PartyCount_Twelve(r21)           sub_821E7138 character init
//   0x821E7628  yes    PartyCount_Twelve(r10)           sub_821E7358 charwords clear
//   0x821E7648  yes    PartyCount_Twelve(r10)           sub_821E7358 charflags clear
//   0x821E5AA0  yes    PartyCount_ClearPositionTail()   sub_821E5A38 reset path
//   0x821E5DB4  yes    PartyCount_ClearPositionTail()   sub_821E5D68 reset path
//   0x821E7690  no     PartyCount_QueueScan(r31)        jump_address_on_true = 0x821E767C
//   0x821E7EB0  no     PartyCount_QueueScan(r30)        jump_address_on_true = 0x821E7E9C
//   0x821E76F4  no     PartyCount_ExtendedId(r11)       jump_address_on_true = 0x821E76FC
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

// Where the position array's two new entries live: the bytes the relocated
// slotbytes array vacated at 0x8243FC30. Kept here rather than in the header
// because nothing else needs them; the relocation side only cares that the
// array *can* grow this far.
constexpr uint32_t kPositionBase = 0x8243FC08;
constexpr uint32_t kPositionStride = 4;
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
// two entries past that are old slotbytes bytes, so without this they start as
// whatever the ten-byte slotbytes clear left behind -- which reads as two extra
// party members with junk display positions.
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

// Both reset paths follow their last position store with a `stb` into the
// relocated slotbytes array, which needs its base register shifted. A mid-ASM
// hook is keyed by instruction address and only one can live at each, so the
// shift cannot sit on the `stb` itself (the restore is already there, after the
// instruction) and has to go on the instruction before -- which is exactly the
// store this file already hooks. One address, one hook, so the two jobs are
// merged here rather than fighting over the slot.
//
// Kept in sync with PartyReloc_slotbytes_Shift in the generated file by the
// static_assert below: both add the same delta.
void PartyReloc_slotbytes_Shift(PPCRegister& reg);

void PartyCount_ClearPositionTail_ShiftSlotbytes(PPCRegister& base) {
    PartyCount_ClearPositionTail();
    PartyReloc_slotbytes_Shift(base);
}

// `cmpwi rX, 0xA` at the top of a scan over the ten-slot pending-award queue,
// with `blt` back to the loop head. Returning true takes that branch for the
// two new slots; the original instruction still handles the first ten.
bool PartyCount_QueueScan(PPCRegister& index) {
    return index.u32 < kRelocatedCharacterCount;
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
