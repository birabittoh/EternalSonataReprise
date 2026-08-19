// eternalsonata - Guest memory for the relocated per-character party arrays.
//
// The game keeps six arrays of per-character state back to back with no padding,
// each exactly ten entries wide, so none of them can grow in place. A seventh
// table, the read-only template the init and level-up paths build stats from,
// has the same problem in the image rather than in .bss. All seven move:
//
//   position    0x8243FC08  10 * 4   ends 0x8243FC30
//   slotbytes   0x8243FC30  10 * 1   ends 0x8243FC3A
//   charflags   0x8243FCFC  10 * 1   ends 0x8243FD06
//   stats_live  0x8243FD08  10 * 48  ends 0x8243FEE8
//   stats_base  0x8243FEE8  10 * 48  ends 0x824400C8
//   charwords   0x824400C8  10 * 2   ends 0x824400DC
//   template    0x82016150  10 * 136 ends 0x820166A0
//
// charflags and charwords are the two halves of the pending-award queue, one
// slot per possible party member. template is the odd one out twice over: it is
// source data rather than state, with the "adg01" string table hard against its
// end, so it has to be *seeded* -- the ten originals are copied across at
// startup, because no guest code ever writes it.
//
// Why all seven, when only four have to
// ------------------------------------
// An earlier revision moved the four cold arrays and let position, stats_live
// and charflags grow into the holes that left: position ran from 0x8243FC08 up
// to 0x8243FC38, over slotbytes' old home, and stats_live over stats_base's.
// Half the hooks, and an unrecoverable failure mode.
//
// The site table is built by sweeping the executable for instructions that
// address these arrays, and that sweep is not complete -- it cannot be, since
// most of it comes from IDA's data xrefs. Under grow-in-place, a site the sweep
// missed keeps writing to an address that is now *inside a live array*: a missed
// slotbytes store lands on position[10] and position[11]. The result is silently
// wrong state for exactly the two new characters, which is the hardest possible
// thing to debug and is what the first live build did.
//
// With every array relocated, the whole original block belongs to nobody. It is
// filled with a canary at startup and checked once per guest frame
// (kPoisonRanges / CheckPartyPoison below), so a missed site announces itself
// with an address instead of corrupting a neighbour. Growing in place halves the
// hook count and can come back once the canary stays clean across a full
// playthrough; it buys nothing else.
//
// Every instruction that materialises one of the seven bases is rewritten by a
// mid-ASM hook, and every instruction that uses an array's end address as a loop
// bound has that bound moved to the new end. Both tables are generated: see
// scripts/party_relocation_scan.py and scripts/party_relocation_emit.py, which
// write src/party_relocation.generated.cpp and the [[midasm_hook]] block in
// eternalsonata_config.toml.
//
// This header owns the other half: the guest memory the relocated arrays now
// live in. The hooks treat the new bases as compile-time constants, so the
// addresses have to be fixed rather than allocator-chosen, which is what
// AllocFixed gives us.
#pragma once

#include <cstdint>

namespace rex {
class Runtime;
namespace memory {
class Memory;
}  // namespace memory
}  // namespace rex

namespace eternalsonata {

// The cast size the relocated arrays are built for. The generated hooks assume
// this; changing it means regenerating them.
inline constexpr uint32_t kRelocatedCharacterCount = 12;

// Where the relocated arrays live. Both sit in one 64 KB page at the top of the
// XEX image heap (0x80000000..0x8BFFFFFF, 64 KB pages), well past the 4.2 MB
// this title's image occupies, so the page is free and stays guest-addressable
// the way guest code needs it to be.
inline constexpr uint32_t kRelocationBase = 0x8B000000;
inline constexpr uint32_t kRelocationSize = 0x10000;

inline constexpr uint32_t kSlotbytesBase = 0x8B000000;   // 12 * 1
inline constexpr uint32_t kStatsBaseBase = 0x8B000100;   // 12 * 48
inline constexpr uint32_t kTemplateBase = 0x8B001000;    // 12 * 136
inline constexpr uint32_t kCharwordsBase = 0x8B001800;   // 12 * 2
inline constexpr uint32_t kPositionBase = 0x8B001900;    // 12 * 4
inline constexpr uint32_t kCharflagsBase = 0x8B001A00;   // 12 * 1
inline constexpr uint32_t kStatsLiveBase = 0x8B002000;   // 12 * 48

// Strides, for the host-side code that reads and writes these arrays directly
// (party_system.cpp, party_counts.cpp, party_save.cpp). Guest code goes through
// the generated hooks and never sees these.
inline constexpr uint32_t kPositionStride = 4;
inline constexpr uint32_t kStatsStride = 48;

// Where the template table lives today, and how much of it is real game data.
// Everything past kTemplateSeedSize is the added slots, which start *zeroed*:
// they are vacant until a mod defines one, and a definition is what fills the
// entry in (see party_slots.h and SeedCharacterTemplate below).
inline constexpr uint32_t kTemplateSource = 0x82016150;
inline constexpr uint32_t kTemplateStride = 136;
inline constexpr uint32_t kTemplateSeedCount = 10;
inline constexpr uint32_t kTemplateSeedSize = kTemplateSeedCount * kTemplateStride;

// Reserves and zeroes the page above. Call once from OnPostSetup, before the
// guest starts running: the first thing the game does with these arrays is
// clear them per element through the reset paths, and those writes go through
// the hooks, so the memory has to exist by then. Returns false if the page
// could not be committed, in which case the hooks must not be enabled, since
// every relocated access would fault.
bool ReservePartyRelocationMemory(rex::Runtime* runtime);

// Host pointer to a guest address, or nullptr before
// ReservePartyRelocationMemory has run. The count hooks in party_counts.cpp use
// this to write the party arrays directly, since a mid-ASM hook only gets
// registers and some of the ten-to-twelve fixes are stores the game never makes.
uint8_t* PartyGuestPointer(uint32_t guest_address);

// Fills the relocated template table with the ten originals, and leaves every
// added slot's entry zeroed. Called by ReservePartyRelocationMemory.
bool SeedTemplateTable(rex::memory::Memory* memory);

// Gives added slot `character` (1-based, above kTemplateSeedCount) a stat
// template copied from retail character `source` (1..kTemplateSeedCount), with
// the entry id at +0 rewritten to the slot's own id, which is what the game's
// entity lookup matches on. This is how a mod-defined character gets starting
// stats and growth curves; a vacant slot's entry stays zeroed.
bool SeedCharacterTemplate(int character, int source);

// Zeroes an added slot's template entry again, for when a definition is given
// up.
bool ClearCharacterTemplate(int character);

// --- the canary over the block the arrays left behind ------------------------
//
// Nothing lives at the original addresses any more, so nothing should touch
// them. The sweep that produced the hook table is not complete, though, and a
// site it missed is invisible until it makes the game behave strangely. Filling
// the vacated block with a pattern and checking it turns that into a report.
//
// A write shows up directly, as a byte that no longer matches. A read does not,
// but a read of the pattern is loud in its own right: as a display position or
// a character id it is far outside any valid range, so it fails visibly rather
// than plausibly, which is the whole point.
//
// The template table is deliberately not poisoned: it lives in the image, it is
// read-only, and SeedTemplateTable copies out of it at startup. Overwriting it
// would break re-seeding and gains nothing, since a missed read there returns
// the ten real entries rather than nonsense.
inline constexpr uint8_t kPoisonByte = 0xCD;

// The .bss block the six state arrays vacated, as [begin, end) guest ranges.
// The two runs are the whole span each array group occupied, including the
// dword_8243FC04 below position and the gap between slotbytes and charflags,
// minus the addresses that were never party state to begin with.
struct PoisonRange {
  uint32_t begin;
  uint32_t end;
};
inline constexpr PoisonRange kPoisonRanges[] = {
    {0x8243FC08, 0x8243FC3A},   // position, slotbytes
    {0x8243FCFC, 0x824400DC},   // charflags, stats_live, stats_base, charwords
};

// Fills the ranges above with kPoisonByte. Called by
// ReservePartyRelocationMemory, after the relocated copies exist.
bool PoisonVacatedPartyBlock(rex::memory::Memory* memory);

// Re-reads the poisoned ranges and logs the first byte in each that changed,
// with its guest address, then repairs it so the next check reports the next
// distinct site rather than the same one every frame. Returns the number of
// modified bytes found. Cheap enough to call once per guest frame: it is 1042
// bytes across two ranges.
//
// A hit means some instruction still addresses the old block, i.e. the sweep in
// scripts/party_relocation_scan.py missed a site. Feed the reported address back
// into that script's ARRAYS/overrides and regenerate.
uint32_t CheckPartyPoison();

}  // namespace eternalsonata
