// eternalsonata - The id range checks that reject characters 11 and 12.
//
// The last piece of the ten-to-twelve conversion, and the easy one. Three
// functions gate on the character id before doing anything, and all three
// reject anything outside 1..10 outright:
//
//   sub_821E7898  the stat recompute every status and equipment screen depends
//                 on. Returns immediately unless 1 <= id <= 10.
//   sub_821A03D0  the battle-party model loader, which formats
//                 "btldata\player\pc%03d.bop". Rejects `id - 1 > 9` before it
//                 gets as far as the name. An asset mod can supply pc011.bop
//                 and pc012.bop; this is what stops the game asking for them.
//   sub_820E78B8  the party menu's "give a character the next free display
//                 position" routine, which the mod API calls to add a member.
//                 Gates the 0-based index on `*a1 <= 9`, so a placement for
//                 indices 10 and 11 fell through silently - the character was
//                 rostered but got no display position, which read as "nothing
//                 happened" in the overlay.
//   sub_821BD0D0  the battle *voice* path builder, "btldata\voice\pc%03d.csf".
//                 Same `id - 1 > 9` shape, and by far the nastiest of the four,
//                 because its caller sub_821BD1C0 frees the party slot's model
//                 object *before* calling it:
//
//                   sub_82113FD0(v18[1]); v18[1] = 0; sub_8210CBB8(v18);
//                   if (!sub_821BD0D0(a1, id, buf) || !sub_8210C9D8(...))
//                       return 0;
//
//                 So a rejected id does not merely skip the voice: it leaves a
//                 released object linked into the engine's per-frame task list,
//                 and the next frame's dispatcher (sub_82132A08) walks into it
//                 and calls through a dead vtable. That is the crash on moving
//                 an added character into the active party, which is when the
//                 battle-party rebuild reaches this path at all.
//
// All three are `cmpwi`/`cmplwi` against a literal, which a mid-ASM hook cannot
// rewrite. So all three hooks fire *before* the compare and take the accept
// path themselves via jump_address_on_true when the id is one of the two new
// ones, leaving the original instruction to handle the first ten exactly as
// before. Jumping past the compare leaves cr6 holding whatever it held; in all
// three cases the next reader of cr6 on the accept path is preceded by its own
// compare (or by a call, which makes cr6 volatile anyway), so nothing observes
// the skip.
//
// A whole-function override was considered for sub_821E7898, since it is fully
// understood, but it is a thousand-odd bytes of stat and equipment maths that
// would then have to be maintained by hand for no gain over a two-instruction
// hook.
//
// None of the three opens for an added id unconditionally. They ask the slot
// registry (party_slots.h) whether a mod has defined that character, so a build
// with no mod loaded gates exactly like retail: an undefined slot has no name,
// no stat template and no model, and these are the three doors it would
// otherwise walk through.
//
// A fourth hook belongs to the same model loader. sub_821A03D0 formats the
// asset path from the character id itself, so a defined slot asks for pc011.bop
// and pc012.bop by default; PartyBounds_ModelId lets a definition point at some
// other pcNNN instead, for a mod whose model is not ready or which wants to
// borrow one.
//
// The matching [[midasm_hook]] blocks are in eternalsonata_config.toml, in the
// "src/party_bounds.cpp" section. Addresses and options:
//
//   address     hook                            jump_address_on_true
//   0x821E78AC  PartyBounds_ExtendedId(r11)     0x821E78B4   sub_821E7898 stat recompute
//   0x821A0528  PartyBounds_ExtendedIndex(r11)  0x821A0530   sub_821A03D0 model loader
//   0x820E78D0  PartyBounds_ExtendedIndex(r11)  0x820E78D8   sub_820E78B8 position place
//   0x821A0530  PartyBounds_ModelId(r6)         -            sub_821A03D0 model id
//   0x821BD110  PartyBounds_VoiceId(r10, r29)   0x821BD114   sub_821BD0D0 voice path
//
// All five fire before the instruction at the listed address. 0x821A0530 is the
// accept path's first instruction, which is also where the third hook's jump
// lands, so the model-id rewrite is reached by both routes; r6 is still the
// character id there and is the "%03d" argument sub_822CC8B0 formats.
//
// The voice hook sits on the `bgt` rather than on the `cmplwi` six instructions
// above it, because the six instructions in between zero the local struct the
// accept path goes on to use; jumping from the compare would skip them.
// 0x821BD114 is the branch's own fall-through.

#include "party_relocation.h"
#include "party_slots.h"

#include <atomic>
#include <cstring>
#include <string>

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>

namespace {

using eternalsonata::IsCharacterDefined;
using eternalsonata::kRelocatedCharacterCount;
using eternalsonata::ModelIdForCharacter;

constexpr uint32_t kRetailCharacterCount = 10;

// True for an id past the retail cast that some mod has filled in.
bool IsDefinedAddedId(uint32_t id) {
  return id > kRetailCharacterCount && id <= kRelocatedCharacterCount &&
         IsCharacterDefined(static_cast<int>(id));
}

// These gates sit on paths the game runs every frame, so a plain log line per
// call would drown the log and change the timing of the very thing being
// diagnosed. Each (gate, id, verdict) triple is logged once and then goes
// quiet, which is enough to answer "did this gate see the added character, and
// what did it decide" after a single reproduction.
void TraceOnce(std::atomic<uint32_t>& seen, const char* gate, uint32_t id, bool accepted,
               int mapped = -1) {
  // One bit per (id, verdict), so each gate reports each character once for
  // "accepted" and once for "rejected". Ids past the table share the top bit,
  // which is fine: they are not what this is for.
  const uint32_t bit = 1u << (((id < 15u ? id : 15u) * 2u + (accepted ? 1u : 0u)) & 31u);
  if (seen.fetch_or(bit, std::memory_order_relaxed) & bit) {
    return;
  }
  if (mapped >= 0) {
    REXLOG_INFO("party gate {}: id {} {} (asset id {})", gate, id,
                accepted ? "accepted" : "rejected", mapped);
  } else {
    REXLOG_INFO("party gate {}: id {} {}", gate, id, accepted ? "accepted" : "rejected");
  }
}

}  // namespace

// The hook bodies are looked up by name by the recompiler, so they keep
// external linkage.

// `cmpwi cr6, r11, 0xA` / `bgt` in sub_821E7898, on a 1-based character id that
// the `cmpwi r11, 1` above has already floored. Only the new ids need the jump;
// 1..10 fall through to the original branch, which does not take it.
bool PartyBounds_ExtendedId(PPCRegister& id) {
  const bool accepted = IsDefinedAddedId(id.u32);
  if (id.u32 > kRetailCharacterCount) {
    static std::atomic<uint32_t> seen{0};
    TraceOnce(seen, "stat-recompute", id.u32, accepted);
  }
  return accepted;
}

// `cmplwi cr6, r11, 9` / `bgt` in sub_821A03D0, on `id - 1` rather than on the
// id. The compare is unsigned, so id 0 has already wrapped to a huge number and
// is rejected by the same branch; keeping the test in terms of the index
// preserves that. The same accept set covers sub_820E78B8's gate on the 0-based
// index of the character being placed into the next free display position:
// there the compare is `cmplwi cr6, r11, 9` / `bgt` over the whole body, and
// jumping straight in for indices 10 and 11 is what lets characters 11 and 12
// into the party at all.
bool PartyBounds_ExtendedIndex(PPCRegister& index) {
  const bool accepted = IsDefinedAddedId(index.u32 + 1);
  if (index.u32 + 1 > kRetailCharacterCount && index.u32 < kRelocatedCharacterCount) {
    static std::atomic<uint32_t> seen{0};
    TraceOnce(seen, "model-or-place", index.u32 + 1, accepted);
  }
  return accepted;
}

// The "%03d" in "btldata\player\pc%03d.bop", which is the character id unless
// the slot's definition names a different model. Retail ids are never remapped,
// so this is a no-op for the whole original cast.
void PartyBounds_ModelId(PPCRegister& id) {
  const uint32_t character = id.u32;
  if (character <= kRetailCharacterCount || character > kRelocatedCharacterCount) {
    return;
  }
  const int mapped = ModelIdForCharacter(static_cast<int>(character));
  static std::atomic<uint32_t> seen{0};
  TraceOnce(seen, "model-id", character, true, mapped);
  id.u32 = static_cast<uint32_t>(mapped);
}

// `bgt cr6, loc_821BD1A0` in sub_821BD0D0, on a cr6 set six instructions
// earlier from `id - 1` against 9. Taking the fall-through for a defined added
// id is what keeps its caller from returning through the bail-out that has
// already freed the party slot's model object.
//
// The id itself is remapped here as well, for the same reason the model loader
// remaps it: the format that follows is "btldata\voice\pc%03d.csf" and r29 is
// its argument, so a slot borrowing character 7's assets asks for pc007.csf
// rather than for a pc011.csf nobody shipped. A definition with no assets at
// all still fails the file check inside, and the caller still bails out
// destructively - that failure belongs to the mod, not to the gate.
bool PartyBounds_VoiceId(PPCRegister& index, PPCRegister& id) {
  const uint32_t character = index.u32 + 1;
  const bool accepted = IsDefinedAddedId(character);
  if (character > kRetailCharacterCount && index.u32 < kRelocatedCharacterCount) {
    static std::atomic<uint32_t> seen{0};
    TraceOnce(seen, "voice", character, accepted,
              accepted ? ModelIdForCharacter(static_cast<int>(character)) : -1);
  }
  if (!accepted) {
    return false;
  }
  id.u32 = static_cast<uint32_t>(ModelIdForCharacter(static_cast<int>(character)));
  return true;
}

// The field character's model setup, sub_821A2B38. This one is not a range
// check that can be opened: `addi r11, r11, -1` / `cmplwi r11, 9` / `bgt` /
// `bctr` is a **ten-entry jump table**, one arm per retail character, and each
// arm fills in the model descriptor that the object about to be created is
// built from. Letting an eleventh index through would branch through the end of
// the table; falling past it - which is what happens untreated - creates the
// field object with no model at all, and that half-built object is what the
// engine's task list is still holding when it gets recycled. It is the crash
// after making an added character the party leader and walking around.
//
// So the id is remapped *before* the table is indexed, and an added character
// takes its borrowed character's arm intact. r11 holds the id at 0x821A2C84,
// one instruction ahead of the subtract.
//
// Note the later `v16 = *v8` re-reads the id from memory rather than from r11,
// so the weapon and costume attachments further down still see the added id and
// attach nothing. That is cosmetic and deliberately left alone; it is not what
// crashes.
void PartyBounds_FieldModelId(PPCRegister& id) {
  const uint32_t character = id.u32;
  if (character <= kRetailCharacterCount || character > kRelocatedCharacterCount) {
    return;
  }
  const int mapped = ModelIdForCharacter(static_cast<int>(character));
  static std::atomic<uint32_t> seen{0};
  TraceOnce(seen, "field-model", character, true, mapped);
  id.u32 = static_cast<uint32_t>(mapped);
}

// Diagnostics, not fixes. Both of the party-model rebuild paths free a slot's
// objects *before* they load the replacements, and both return early if a load
// fails, leaving a released object linked in the engine's per-frame task list -
// which is what a dead-vtable crash in the dispatcher sub_82132A08 looks like,
// and what a softlock in it looks like too when the dead task loops instead of
// faulting.
//
// "voice or model load failed" was not specific enough to act on. Both callers
// have *two* early returns -- the id gate refusing to build a path, and
// sub_8210C9D8 failing to load the path it built -- and they mean opposite
// things: the first is this port's gating being wrong, the second is an asset
// that is missing or unreadable. Each site now says which, and names the file,
// because "which asset failed" is the whole question.
//
// Both callers format into the same place, `r1 + 0x60` (var_C0 in sub_821A03D0,
// var_A0 in sub_821BD1C0), so the path is read from there.
namespace {

constexpr uint32_t kPathBufferOffset = 0x60;
constexpr size_t kMaxPathChars = 192;

// The formatted asset path a bail-out was about, or "<unreadable>". Plain ASCII
// in guest memory, so no byte swapping: these are chars, not words.
std::string GuestPathAt(uint32_t address) {
  const auto* host = eternalsonata::PartyGuestPointer(address);
  if (!host) {
    return "<unreadable>";
  }
  const auto* end = static_cast<const uint8_t*>(
      std::memchr(host, '\0', kMaxPathChars));
  const size_t length = end ? static_cast<size_t>(end - host) : kMaxPathChars;
  return std::string(reinterpret_cast<const char*>(host), length);
}

}  // namespace

// sub_821BD1C0, at the `beq` after sub_8210C9D8 (0x821BD334): the voice path was
// built and the file behind it would not load. r29 points at the character id
// the caller passed down.
void PartyTrace_FieldModelLoadFailed(PPCRegister& sp, PPCRegister& id_ptr) {
  uint32_t id = 0;
  if (const auto* host = eternalsonata::PartyGuestPointer(id_ptr.u32)) {
    id = rex::memory::load_and_swap<uint32_t>(host);
  }
  REXLOG_WARN("party: sub_821BD1C0 bailed out after freeing a party slot: "
              "character {} asked for \"{}\" and it would not load. Its object "
              "is now dangling.",
              id, GuestPathAt(sp.u32 + kPathBufferOffset));
}

// sub_821BD1C0, at the `beq` after sub_821BD0D0 (0x821BD320): the path was never
// built, because the voice gate refused the id. That is this port's gating, not
// a missing file, and it is the one of the two that PartyBounds_VoiceId is
// supposed to prevent.
void PartyTrace_FieldVoicePathRejected(PPCRegister& id_ptr) {
  uint32_t id = 0;
  if (const auto* host = eternalsonata::PartyGuestPointer(id_ptr.u32)) {
    id = rex::memory::load_and_swap<uint32_t>(host);
  }
  REXLOG_WARN("party: sub_821BD0D0 refused to build a voice path for character "
              "{}, so sub_821BD1C0 bailed out after freeing the slot. This is a "
              "gate, not a missing asset.",
              id);
}

// sub_821A03D0, at the `beq` after sub_8210C9D8 (0x821A0578). r29 is the battle
// party slot index, 0..2.
void PartyTrace_BattleModelLoadFailed(PPCRegister& sp, PPCRegister& slot) {
  REXLOG_WARN("party: sub_821A03D0 bailed out after freeing the battle party: "
              "slot {} asked for \"{}\" and it would not load. Its objects are "
              "now dangling.",
              slot.u32, GuestPathAt(sp.u32 + kPathBufferOffset));
}
