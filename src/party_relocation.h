// eternalsonata - Guest memory for the relocated per-character party arrays.
//
// The game keeps four arrays of per-character state back to back with no
// padding, each exactly ten entries wide, so none of them can grow in place.
// Two of them do not have to move: relocating the two *cold* arrays leaves
// holes the two *hot* ones grow into.
//
//   position    0x8243FC08  10 * 4   ends 0x8243FC30   stays, grows to 0x8243FC38
//   slotbytes   0x8243FC30  10 * 1   ends 0x8243FC3A   RELOCATED
//   stats_live  0x8243FD08  10 * 48  ends 0x8243FEE8   stays, grows to 0x8243FF48
//   stats_base  0x8243FEE8  10 * 48  ends 0x82440068   RELOCATED
//
// A fifth table moves for the same reason, though it is not party state: the
// read-only per-character template the init and level-up paths build stats from.
//
//   template    0x82016150  10 * 136 ends 0x820166A0   RELOCATED
//
// A sixth array moves for the same reason. The pending-award queue is two
// parallel ten-slot arrays, one slot per possible party member:
//
//   charflags   0x8243FCFC  10 * 1   ends 0x8243FD06   stays, grows to 0x8243FD08
//   charwords   0x824400C8  10 * 2   ends 0x824400DC   RELOCATED
//
// charflags has exactly two spare bytes before stats_live. charwords does not:
// 0x824400DC is a live global of its own, so it moves.
//
// It sits in the image with the "adg01" string table hard against its end, so it
// cannot grow in place either. Being source data rather than state, it is the
// one table that has to be *seeded*: the ten originals are copied across at
// startup, because no guest code ever writes it.
//
// Every instruction that materialises one of the two relocated bases is
// rewritten by a mid-ASM hook, and every instruction that uses a grown array's
// old end address as a loop bound has that bound moved to the new end. Both
// tables are generated: see scripts/party_relocation_scan.py and
// scripts/party_relocation_emit.py, which write src/party_relocation.generated.cpp
// and the [[midasm_hook]] block in eternalsonata_config.toml.
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

// Where the template table lives today, and how much of it is real game data.
// Everything past kTemplateSeedSize is the two new characters, which start as
// a copy of character 1 until a mod fills them in.
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

// Fills the relocated template table: the ten originals copied across, then one
// placeholder entry per new character. Called by ReservePartyRelocationMemory;
// exposed so a mod-facing path can reseed it if it wants to rebuild the table.
bool SeedTemplateTable(rex::memory::Memory* memory);

}  // namespace eternalsonata
