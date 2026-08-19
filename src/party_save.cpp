// eternalsonata - Carrying characters 11 and 12 through the save file.
//
// party_relocation.{h,cpp} widen the in-memory arrays and party_counts.cpp
// teaches the loops to count past ten. Neither touches the save, and the save
// is where a twelve-character party would otherwise lose its two new members on
// every load.
//
// The save is friendlier than it looks. sub_82241190 (write) and sub_822404E8
// (load) are mirror images, and neither writes a fixed struct: both build a list
// of (ptr, size) blocks by calling sub_8223DB40 repeatedly, then hand the list
// to sub_8223DF80, which does the I/O in one direction or the other. The writer
// uses 22 of the 32 available block slots, so there is room, and the game
// already appends a block conditionally on its own save version, which is the
// idiom this file follows.
//
// What it does NOT do is widen the two 480-byte stat blocks. Those are
// 48 * 10, hardcoded as an immediate at all four sites, and every later offset
// on both the write and the load side is a baked immediate behind them. Instead
// the version goes from 2 to 3 and two 96-byte blocks holding characters 11 and
// 12 are appended, pointing straight at the live arrays rather than at a staging
// copy. A version-2 retail save still loads, because the new blocks are simply
// not registered for it. A version-3 save is 192 bytes larger than retail
// expects and retail cannot read it; that is the accepted, one-directional
// trade.
//
// A second thing it does not do is redirect the 2324-byte block at 0x8243F3E8.
// That one is copied as a span and contains position and slotbytes, both of
// which moved; see PartySave_LoadPartyBlock at the bottom of this file.
//
// The matching [[midasm_hook]] blocks are in eternalsonata_config.toml, outside
// the generated relocation block. This table is the index:
//
//   address     after  hook                              site
//   0x822414D0  yes    PartySave_BumpVersion(r10)        sub_82241190 `li r10, 2`
//   0x82241674  no     PartySave_AppendWriteBlocks(r28)  after the last DB40 call
//   0x82241698  yes    PartySave_GrowTotalSize(r8)       the `addi r8, r8, 0x5B98`
//   0x82240600  no     PartySave_AppendLoadBlocks(r30, r29)  loc_82240600
//   0x82240B28  yes    PartySave_LoadPartyBlock()        after the block restore
//   0x82241278  yes    PartySave_StorePartyBlock()       before the block capture
//
// On the write side r28 is the block-list descriptor and r8 is the total-size
// argument being built by the `addis 0x52` / `addi 0x5B98` pair. On the load
// side r30 is the descriptor and r29 points at the slot descriptor whose +0x18
// holds the version; loc_82240600 is the join point right after the existing
// version-2 conditional block, so the append lands after every block the game
// registers for itself and before sub_8223DF80 consumes the list.

#include "party_relocation.h"

#include <cstring>

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>

namespace {

using eternalsonata::kRelocatedCharacterCount;
using eternalsonata::kStatsBaseBase;
using eternalsonata::kStatsLiveBase;
using eternalsonata::kStatsStride;

// The stats arrays are 48-byte structs per character. Characters 11 and 12 are
// the entries past the ten a retail save holds, so each side-car block starts
// one retail array-length in and runs to the end of the widened array. Both
// arrays are relocated, so both side-cars are addressed off their new bases;
// see party_relocation.h.
constexpr uint32_t kRetailCharacterCount = 10;
constexpr uint32_t kRetailStatsSize = kRetailCharacterCount * kStatsStride;
constexpr uint32_t kSideCarSize =
    (kRelocatedCharacterCount - kRetailCharacterCount) * kStatsStride;

constexpr uint32_t kStatsLiveSideCar = kStatsLiveBase + kRetailStatsSize;
constexpr uint32_t kStatsBaseSideCar = kStatsBaseBase + kRetailStatsSize;

static_assert(kSideCarSize == 96, "two characters of stats, 48 bytes each");

// The save version this build writes, and the lowest version that carries the
// side-car blocks. The retail game writes 2 and its loader already gates a
// trailing block on `>= 2`, so 3 continues the game's own scheme.
constexpr uint32_t kSaveVersionTwelve = 3;

// sub_8223DB40's block-list descriptor, replicated rather than called. The
// function is five stores behind two guards; calling it from a mid-ASM hook
// would mean a guest call on the guest thread mid-instruction, which is far more
// machinery than the stores are worth.
constexpr uint32_t kDescBusy = 512;      // u32, non-zero means refuse
constexpr uint32_t kDescCount = 1084;    // u32, blocks registered so far
constexpr uint32_t kDescPtr = 572;       // u32, + 16 * index
constexpr uint32_t kDescSize = 576;      // u32, + 16 * index
constexpr uint32_t kDescArg = 580;       // u32, + 16 * index
constexpr uint32_t kDescFlag = 584;      // u8,  + 16 * index
constexpr uint32_t kDescMaxBlocks = 32;

template <typename T>
bool ReadGuest(uint32_t address, T* out) {
  auto* host = eternalsonata::PartyGuestPointer(address);
  if (!host) {
    return false;
  }
  *out = rex::memory::load_and_swap<T>(host);
  return true;
}

template <typename T>
bool WriteGuest(uint32_t address, T value) {
  auto* host = eternalsonata::PartyGuestPointer(address);
  if (!host) {
    return false;
  }
  rex::memory::store_and_swap<T>(host, value);
  return true;
}

// sub_8223DB40(descriptor, ptr, size, 0, 0), inlined. Returns false when the
// game itself would have returned 0, in which case the caller stops: a
// half-registered pair would put a block count and a total size out of step.
bool RegisterBlock(uint32_t descriptor, uint32_t ptr, uint32_t size) {
  uint32_t busy = 0;
  if (!ReadGuest(descriptor + kDescBusy, &busy) || busy != 0) {
    return false;
  }
  uint32_t count = 0;
  if (!ReadGuest(descriptor + kDescCount, &count) || count >= kDescMaxBlocks) {
    return false;
  }
  const uint32_t slot = 16 * count;
  if (!WriteGuest<uint32_t>(descriptor + kDescPtr + slot, ptr) ||
      !WriteGuest<uint32_t>(descriptor + kDescSize + slot, size) ||
      !WriteGuest<uint32_t>(descriptor + kDescArg + slot, 0) ||
      !WriteGuest<uint8_t>(descriptor + kDescFlag + slot, 0)) {
    return false;
  }
  return WriteGuest<uint32_t>(descriptor + kDescCount, count + 1);
}

// The two side-car blocks, in the order both sides have to agree on. Order is
// the whole contract here: the loader matches blocks positionally, so base then
// live on the write side means base then live on the load side.
bool AppendSideCarBlocks(uint32_t descriptor) {
  if (!RegisterBlock(descriptor, kStatsBaseSideCar, kSideCarSize)) {
    REXLOG_ERROR("party save: could not append the base-stats side-car block");
    return false;
  }
  if (!RegisterBlock(descriptor, kStatsLiveSideCar, kSideCarSize)) {
    // The base block is registered and the live one is not, so the two sides
    // now disagree about the block list. Loudly, because a save written in this
    // state is one the loader will misread.
    REXLOG_ERROR("party save: appended half the side-car; save is inconsistent");
    return false;
  }
  return true;
}

// position and slotbytes are inside the 2324-byte block the save copies as one
// span, so the copy still lands on their old addresses. Carry the ten saved
// entries of each across, in whichever direction the caller needs.
//
// Both arrays are plain bytes to this code -- ten 4-byte display positions and
// ten 1-byte slot values -- so a straight memcpy is right; there is no
// endianness to fix because nothing here interprets the values.
bool CopyStagedPartyArrays(bool from_staging) {
  struct Span {
    uint32_t staging;
    uint32_t live;
    uint32_t size;
    const char* name;
  };
  const Span spans[] = {
      {eternalsonata::kPositionStagingBase, eternalsonata::kPositionBase,
       eternalsonata::kSavedCharacterCount * eternalsonata::kPositionStride,
       "position"},
      {eternalsonata::kSlotbytesStagingBase, eternalsonata::kSlotbytesBase,
       eternalsonata::kSavedCharacterCount, "slotbytes"},
  };

  for (const auto& span : spans) {
    auto* staging = eternalsonata::PartyGuestPointer(span.staging);
    auto* live = eternalsonata::PartyGuestPointer(span.live);
    if (!staging || !live) {
      REXLOG_ERROR("party save: no memory to carry {} across the save block",
                   span.name);
      return false;
    }
    if (from_staging) {
      std::memcpy(live, staging, span.size);
    } else {
      std::memcpy(staging, live, span.size);
    }
  }
  return true;
}

}  // namespace

// The hook bodies are looked up by name by the recompiler, so they keep
// external linkage.

// `li r10, 2` at 0x822414D0, stored into the save header at 0x822414EC. Fired
// after the instruction, so the register holds the two the game just loaded.
void PartySave_BumpVersion(PPCRegister& version) {
  version.u64 = kSaveVersionTwelve;
}

// Before 0x82241674, i.e. after the last block the writer registers for itself
// and before it builds sub_8223DF80's arguments.
void PartySave_AppendWriteBlocks(PPCRegister& descriptor) {
  AppendSideCarBlocks(descriptor.u32);
}

// After the `addi r8, r8, 0x5B98` at 0x82241698 that completes the baked
// total-size constant. The two appended blocks have to be counted in it, or the
// write is short by exactly their length.
void PartySave_GrowTotalSize(PPCRegister& total_size) {
  total_size.u64 = total_size.u32 + 2 * kSideCarSize;
}

// Before loc_82240600, the join point right after the loader's own version-2
// conditional block. Registers the same two blocks in the same order, but only
// for a save this build wrote: a version-2 retail save has no side-car, and
// asking for one would read 192 bytes of whatever follows it.
void PartySave_AppendLoadBlocks(PPCRegister& descriptor, PPCRegister& slot) {
  uint32_t header = 0;
  if (!ReadGuest(slot.u32, &header)) {
    return;
  }
  uint32_t version = 0;
  if (!ReadGuest(header + 0x18, &version)) {
    return;
  }
  if (version < kSaveVersionTwelve) {
    return;
  }
  AppendSideCarBlocks(descriptor.u32);
}

// The 2324-byte block at 0x8243F3E8 straddles two arrays that moved.
//
// Everything else the relocation touches is reached through a pointer some
// instruction materialises, so rewriting the instruction moves the access. This
// block is not: sub_822CF5B0 copies 0x8243F3E8..0x8243FCFC as one span, and
// position (0x8243FC08) and slotbytes (0x8243FC30) are 2080 and 2120 bytes into
// it. The span cannot be redirected, because most of it -- party level, the
// budget pair, and the rest of the block -- never moved and still has to land at
// the old addresses. So the copy keeps happening, and these two hooks carry the
// two arrays across it.
//
// This is what the canary found. Without them the loader restored a save's
// party table into dead memory, the relocated position array kept whatever it
// had, and the game hung as soon as anything read a party position -- opening
// the status menu was enough.
//
// Characters 11 and 12 are not involved: the block holds ten of each, exactly as
// retail wrote it, and the two added slots ride in the side-car blocks above.
// (The side-car currently carries stats only, so an added character's *display
// position* still does not survive a save. That is the next piece of this file,
// not something these hooks should improvise.)

// After the `bl sub_822CF5B0` at 0x82240B28, so the block has landed and the ten
// saved entries can be lifted out of it into the relocated arrays.
void PartySave_LoadPartyBlock() {
  CopyStagedPartyArrays(/*from_staging=*/true);
}

// After the `mr r3, r20` at 0x82241278, i.e. on the instruction before the
// matching `bl` on the write side, so the block picks up the live values rather
// than whatever the load left at the old addresses.
void PartySave_StorePartyBlock() {
  CopyStagedPartyArrays(/*from_staging=*/false);
}
