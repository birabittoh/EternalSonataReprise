// eternalsonata - Ten character bounds raised to twelve.
//
// Accesses past an array's retail end already reach the twelve wide copies
// (party_arrays.cpp); what stops the game from making them is its own
// bounds: loop counts, end addresses and id gates. config/party.toml hooks
// each of those sites with one of these, which redo the bound for twelve.

#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include "party_arrays.h"

namespace {

constexpr int32_t kRetail = static_cast<int32_t>(eternalsonata::kRetailCharacterCount);
constexpr int32_t kTwelve = static_cast<int32_t>(eternalsonata::kPartyCharacterCount);

void Compare(PPCCRRegister& cr, int32_t left, int32_t right) {
  cr.lt = left < right;
  cr.gt = left > right;
  cr.eq = left == right;
}

void CompareLogical(PPCCRRegister& cr, uint32_t left, uint32_t right) {
  cr.lt = left < right;
  cr.gt = left > right;
  cr.eq = left == right;
}

}  // namespace

// After `cmpwi cr, r, 10` (a count or a 1 based id): compare with 12.
extern "C++" void PartyCompareCount(PPCCRRegister& cr, PPCRegister& r) {
  Compare(cr, r.s32, kTwelve);
}

// After `cmpwi cr, r, 9` (a 0 based index): compare with 11.
extern "C++" void PartyCompareIndex(PPCCRRegister& cr, PPCRegister& r) {
  Compare(cr, r.s32, kTwelve - 1);
}

// After `cmplwi cr, r, 9`.
extern "C++" void PartyCompareIndexLogical(PPCCRRegister& cr, PPCRegister& r) {
  CompareLogical(cr, r.u32, kTwelve - 1);
}

// After `cmplwi cr, r, 10`.
extern "C++" void PartyCompareCountLogical(PPCCRRegister& cr, PPCRegister& r) {
  CompareLogical(cr, r.u32, kTwelve);
}

// After `cmpwi cr, r, 40` (a byte offset into ten words): compare with 48.
extern "C++" void PartyCompareWordEnd(PPCCRRegister& cr, PPCRegister& r) {
  Compare(cr, r.s32, kTwelve * 4);
}

// After `cmpwi cr, r, 11` (a 1 based id loop's end): compare with 13.
extern "C++" void PartyCompareLimit(PPCCRRegister& cr, PPCRegister& r) {
  Compare(cr, r.s32, kTwelve + 1);
}

// After `cmpwi cr, r, 10` choosing a ten member layout: take it for more too.
extern "C++" void PartyLayoutTen(PPCCRRegister& cr, PPCRegister& r) {
  cr.eq = r.s32 >= kRetail;
}

// Before a member count is used to index a ten slot menu: at most ten.
extern "C++" void PartyClampCount(PPCRegister& r) {
  if (r.s32 > kRetail)
    r.s64 = kRetail;
}

// After a reset stores position[9]: the reset clears the whole table.
extern "C++" void PartyClearExtraPositions() {
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory)
    return;
  for (uint32_t i = eternalsonata::kRetailCharacterCount; i < eternalsonata::kPartyCharacterCount;
       ++i)
    *memory->TranslateVirtual<uint32_t*>(
        eternalsonata::PartyArrayAddress(eternalsonata::PartyArray::kPosition, i)) = 0;
}

// After `li r, 10` that sets a loop count.
extern "C++" void PartyCount(PPCRegister& r) {
  r.s64 = kTwelve;
}

// After an instruction that forms an array's retail end address: move it out
// by two entries of that array's stride.
extern "C++" void PartyEndU8(PPCRegister& r) {
  r.u64 = r.u32 + (kTwelve - kRetail) * 1;
}
extern "C++" void PartyEndU16(PPCRegister& r) {
  r.u64 = r.u32 + (kTwelve - kRetail) * 2;
}
extern "C++" void PartyEndU32(PPCRegister& r) {
  r.u64 = r.u32 + (kTwelve - kRetail) * 4;
}
extern "C++" void PartyEndStats(PPCRegister& r) {
  r.u64 = r.u32 + (kTwelve - kRetail) * 48;
}
