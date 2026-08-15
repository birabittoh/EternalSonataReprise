// eternalsonata - the menu draw walk's missing-child crash.
//
// sub_82160DC8 walks a menu node's child list. Each child is an 8-byte
// (type byte, u32 handle) pair, and per child the walk resolves the handle
// twice: sub_8217BED0 for the display object, and sub_82176CD8 for the record
// that carries the flags it is about to test. The second lookup is a binary
// search that answers 0 when the handle is not registered - which includes the
// handle -1, since sub_82176CD8 treats -1 as "never cached" and then fails to
// find it. The walk stores that 0 in r31 and dereferences it anyway:
//
//   0x82160E6C  bl sub_8217BED0
//   0x82160E70  lwz r11, 4(r31)      <- r31 is 0 when the record is missing
//   0x82160E78  clrlwi. r11, r11, 31
//   0x82160E7C  beq loc_82160EE0     <- the flag test the load feeds
//
// Retail gets away with it because a child is only ever added with a handle
// that resolves. With twelve characters that stops being true: on the party
// status screen (screen 0x17, built by the display-list interpreter
// sub_821F2F38) one element for the added character is not created, the
// interpreter passes the list's placeholder -1 to sub_82212048 anyway, and
// sub_82160F00 appends it as an ordinary child and counts it. The next draw
// walks into it and faults reading guest address 4.
//
// The hook fires before the load and takes the walk's own "nothing to do for
// this child" exit, 0x82160EE0, which is where the flag test the load feeds
// branches to. That path re-reads the child count, advances the index and the
// cursor, and loops - so a child with no record is skipped exactly the way a
// child whose flag bit is clear already was. The skipped instructions are the
// load itself and `mr r30, r3`; r30 is only read inside the body being
// skipped, and every iteration sets it again before use.
//
// This guards the draw, not the cause. The element that fails to be created
// for the added character is a separate problem - the interpreter reaches the
// call site at 0x821F44C4 with r19 (its current-element handle) still -1,
// having taken the "reuse the handle the command carries" branch rather than
// the one that creates an element. Fixing that is what makes the screen draw
// the missing widget instead of merely not crashing on it.
//
// The matching [[midasm_hook]] block is in eternalsonata_config.toml:
//
//   address     hook                        jump_address_on_true
//   0x82160E70  PartyUi_MissingChild(r31)   0x82160EE0   sub_82160DC8 draw walk

#include <rex/ppc/context.h>

// Looked up by name by the recompiler, so it keeps external linkage.

// True when sub_82176CD8 answered 0 for this child, i.e. the child has no
// record to read flags out of. The walk's per-child body is then skipped.
bool PartyUi_MissingChild(PPCRegister& record) {
  return record.u32 == 0;
}
