// eternalsonata - The position table routines, twelve wide.
//
// These walk the position table by its retail end address and reach a
// character through the 1 based alias dword_8243FC04[c], whose entries 11 and
// 12 land in the next array, so the address remap cannot widen them. They are
// rewritten here over the relocated arrays instead, following their PS3 twins
// (sub_1E88E0, sub_1E8D90, sub_1E91D0), which have the same logic at twelve.
// A character is its 1 based number c; position[c - 1] is its 1 based display
// position, 0 when it is not in the party (docs/party-system.md).

#include "party_arrays.h"
#include "generated/eternalsonata_init.h"

#include <cstdint>

#include <rex/hook.h>

namespace {

using eternalsonata::kPartyCharacterCount;
using eternalsonata::PartyArray;
using eternalsonata::PartyArrayAddress;

// The active three the rebuild prefers over the position order. Nothing in
// the 360 writes it, but it is read as the retail code reads it.
constexpr uint32_t kActiveBytes = 0x8243FC3Au;
constexpr uint32_t kBattleParty = 0x824D0480u;
constexpr uint32_t kBattleVoices = 0x824D13C0u;

struct Party {
  uint8_t* base;

  uint32_t Position(uint32_t c) const {
    return REX_LOAD_U32(PartyArrayAddress(PartyArray::kPosition, c - 1));
  }
  void SetPosition(uint32_t c, uint32_t p) const {
    REX_STORE_U32(PartyArrayAddress(PartyArray::kPosition, c - 1), p);
  }
  uint8_t SlotByte(uint32_t c) const {
    return REX_LOAD_U8(PartyArrayAddress(PartyArray::kSlotBytes, c - 1));
  }
  void SetSlotByte(uint32_t c, uint8_t v) const {
    REX_STORE_U8(PartyArrayAddress(PartyArray::kSlotBytes, c - 1), v);
  }
  uint32_t Count() const {
    uint32_t n = 0;
    for (uint32_t c = 1; c <= kPartyCharacterCount; ++c)
      n += Position(c) != 0;
    return n;
  }
  // The character at display position p, 0 for none. Where the retail code
  // then writes through index 0, it hits the word before the table; that
  // write is dropped here.
  uint32_t At(uint32_t p) const {
    for (uint32_t c = 1; c <= kPartyCharacterCount; ++c)
      if (Position(c) == p)
        return c;
    return 0;
  }
};

// sub_821E6428: the active three are the set bytes of kActiveBytes, filled up
// from display positions 1..3, then handed to the battle party and the voice
// loader.
void RebuildBattleParty(PPCContext& ctx, uint8_t* base) {
  const Party party{base};
  uint32_t ids[3] = {};
  int count = 0;
  for (int i = 0; i < 3; ++i) {
    const uint8_t id = REX_LOAD_U8(kActiveBytes + i);
    if (id)
      ids[count++] = id;
  }
  for (uint32_t p = 1; p <= 3 && count < 3; ++p) {
    const uint32_t c = party.At(p);
    if (!c)
      break;
    if (ids[0] != c && ids[1] != c && ids[2] != c)
      ids[count++] = c;
  }

  // The callees take the list by pointer: carve a frame below the caller's.
  PPCContext call = ctx;
  call.r1.u32 = ctx.r1.u32 - 0x80;
  REX_STORE_U32(call.r1.u32, ctx.r1.u32);
  const uint32_t list = call.r1.u32 + 0x50;
  for (int i = 0; i < 3; ++i)
    REX_STORE_U32(list + 4 * i, ids[i]);

  call.r3.u32 = kBattleParty;
  call.r4.u32 = list;
  sub_821A03D0(call, base);
  call.r3.u32 = kBattleParty;
  sub_8219FCE8(call, base);
  call.r3.u32 = kBattleParty;
  sub_8219FE10(call, base);
  call.r3.u32 = kBattleVoices;
  call.r4.u32 = list;
  sub_821BD1C0(call, base);
  ctx.r3.u64 = call.r3.u64;
}

}  // namespace

REX_HOOK_RAW(sub_821E6428) {
  RebuildBattleParty(ctx, base);
}

// Script native 5006: give character *index + 1 the next free position.
REX_HOOK_RAW(sub_820E78B8) {
  const Party party{base};
  const uint32_t c = REX_LOAD_U32(ctx.r3.u32) + 1;
  if (c - 1 < kPartyCharacterCount && !party.Position(c)) {
    party.SetPosition(c, party.Count() + 1);
    party.SetSlotByte(c, 0);
    RebuildBattleParty(ctx, base);
  }
  ctx.r3.u64 = 0;
}

// Script native 5007: drop character *index + 1, closing the gap behind it.
REX_HOOK_RAW(sub_820E7948) {
  const Party party{base};
  const uint32_t c = REX_LOAD_U32(ctx.r3.u32) + 1;
  const uint32_t n = party.Count();
  if (c - 1 < kPartyCharacterCount && party.Position(c)) {
    for (uint32_t p = party.Position(c); p < n; ++p) {
      if (const uint32_t next = party.At(p + 1))
        party.SetPosition(next, p);
    }
    party.SetPosition(c, 0);
    RebuildBattleParty(ctx, base);
  }
  ctx.r3.u64 = 0;
}

// Swaps the positions of characters a and b.
REX_HOOK_RAW(sub_821E61D0) {
  const Party party{base};
  const uint32_t a = ctx.r3.u32;
  const uint32_t b = ctx.r4.u32;
  if (a - 1 < kPartyCharacterCount && b - 1 < kPartyCharacterCount) {
    const uint32_t pa = party.Position(a);
    party.SetPosition(a, party.Position(b));
    party.SetPosition(b, pa);
  }
  RebuildBattleParty(ctx, base);
}

// Moves character c to the front, shifting whoever was ahead of it back. A
// character not yet in the party pushes everyone back and is left at 0, as on
// the 360.
REX_HOOK_RAW(sub_821E6240) {
  const Party party{base};
  const uint32_t c = ctx.r3.u32;
  if (c - 1 >= kPartyCharacterCount)
    return;
  const uint32_t n = party.Count();
  if (n < 1) {
    party.SetPosition(c, n + 1);
    party.SetSlotByte(c, static_cast<uint8_t>(n + 1));
    RebuildBattleParty(ctx, base);
    return;
  }
  const uint32_t p = party.Position(c);
  if (!p) {
    for (uint32_t i = n; i >= 1; --i) {
      if (const uint32_t w = party.At(i))
        party.SetPosition(w, i + 1);
    }
    party.SetPosition(c, 0);
    party.SetSlotByte(c, 0);
    RebuildBattleParty(ctx, base);
    return;
  }
  for (uint32_t i = p; i > 1; --i) {
    if (const uint32_t w = party.At(i - 1)) {
      party.SetPosition(w, i);
      if (party.SlotByte(w))
        party.SetSlotByte(w, static_cast<uint8_t>(i));
    }
  }
  party.SetPosition(c, 1);
  party.SetSlotByte(c, 1);
  RebuildBattleParty(ctx, base);
}
