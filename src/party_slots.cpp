// eternalsonata - The registry behind the two vacant character slots.
// See party_slots.h for what this is and who calls it.

#include "party_slots.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

#include <rex/logging.h>

#include "party_relocation.h"

// Defined in party_system.cpp. Undefining a slot that is still in the party has
// to take it out first, and the party edit path is the only thing that knows
// how.
extern "C" __declspec(dllexport) int EternalSonataRemoveCharacterFromParty(int character);
extern "C" __declspec(dllexport) int EternalSonataGetCharacterPosition(int character);
extern "C" __declspec(dllexport) int EternalSonataSetCharacterName(int character,
                                                                   const char* name);

namespace eternalsonata {
namespace {

constexpr int kCharacterCount = ETERNALSONATA_CHARACTER_COUNT;
constexpr int kNativeCharacterCount = ETERNALSONATA_NATIVE_CHARACTER_COUNT;
constexpr int kFirstAddedCharacter = ETERNALSONATA_CHAR_FIRST_ADDED;
static_assert(kFirstAddedCharacter == kNativeCharacterCount + 1,
              "the added slots are the ids just past the retail cast");

struct Slot {
  bool defined = false;
  std::string name;
  int template_source = ETERNALSONATA_CHAR_ALLEGRETTO;
  int model_id = 0;  // 0 = the slot's own id
  bool has_starting_stats = false;
  EternalSonataCharacterStats starting_stats{};
};

std::mutex g_mutex;
Slot g_slots[kCharacterCount + 1];

// One bit per character id, set while that slot is defined. The id gates read
// this from the guest thread on every stat recompute, so it is deliberately a
// lock-free read rather than a trip through g_mutex.
std::atomic<uint32_t> g_defined_mask{0};

bool IsAddedSlot(int character) {
  return character >= kFirstAddedCharacter && character <= kCharacterCount;
}

void SetDefinedBit(int character, bool defined) {
  const uint32_t bit = 1u << character;
  if (defined) {
    g_defined_mask.fetch_or(bit, std::memory_order_release);
  } else {
    g_defined_mask.fetch_and(~bit, std::memory_order_release);
  }
}

// Reads a definition through its struct_size, so a mod built against an older
// header - which simply stops short of the newer fields - still works.
Slot SlotFromDefinition(const EternalSonataCharacterDefinition* definition) {
  EternalSonataCharacterDefinition copy{};
  const size_t size = definition->struct_size
                          ? std::min<size_t>(definition->struct_size, sizeof(copy))
                          : sizeof(copy);
  std::memcpy(&copy, definition, size);

  Slot slot;
  slot.defined = true;
  slot.name = copy.name ? copy.name : "";
  slot.template_source = copy.template_source ? copy.template_source
                                              : ETERNALSONATA_CHAR_ALLEGRETTO;
  slot.model_id = copy.model_id;
  slot.has_starting_stats = copy.apply_stats != 0;
  slot.starting_stats = copy.stats;
  return slot;
}

}  // namespace

bool IsCharacterDefined(int character) {
  if (character < 1 || character > kCharacterCount) {
    return false;
  }
  if (character <= kNativeCharacterCount) {
    return true;  // the retail cast is always there
  }
  return (g_defined_mask.load(std::memory_order_acquire) & (1u << character)) != 0;
}

int ModelIdForCharacter(int character) {
  if (IsAddedSlot(character)) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const Slot& slot = g_slots[character];
    if (slot.defined && slot.model_id > 0) {
      return slot.model_id;
    }
  }
  return character;
}

std::string AddedCharacterName(int character) {
  if (!IsAddedSlot(character)) {
    return std::string();
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_slots[character].defined ? g_slots[character].name : std::string();
}

bool TakeStartingStats(int character, EternalSonataCharacterStats* out) {
  if (!IsAddedSlot(character) || !out) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  Slot& slot = g_slots[character];
  if (!slot.defined || !slot.has_starting_stats) {
    return false;
  }
  *out = slot.starting_stats;
  slot.has_starting_stats = false;
  return true;
}

int AddedSlotCount() { return kCharacterCount - kNativeCharacterCount; }

int AddedSlotAt(int index) {
  if (index < 0 || index >= AddedSlotCount()) {
    return ETERNALSONATA_PARTY_ERR_INVALID_ARGUMENT;
  }
  return kFirstAddedCharacter + index;
}

int DefineCharacter(int character, const EternalSonataCharacterDefinition* definition) {
  if (!definition || !definition->name || !*definition->name) {
    return ETERNALSONATA_PARTY_ERR_INVALID_ARGUMENT;
  }
  if (!IsAddedSlot(character)) {
    // The retail cast is not definable: it already has content, and letting a
    // mod redefine it is what the removed clone system used to do.
    return ETERNALSONATA_PARTY_ERR_INVALID_CHARACTER;
  }

  Slot incoming = SlotFromDefinition(definition);
  if (incoming.template_source < 1 || incoming.template_source > kNativeCharacterCount) {
    return ETERNALSONATA_PARTY_ERR_INVALID_ARGUMENT;
  }

  {
    std::lock_guard<std::mutex> lock(g_mutex);
    Slot& slot = g_slots[character];
    // Redefining a slot is how a mod changes its own character. Two mods
    // fighting over one slot is a different thing, but neither the host nor the
    // mod can tell them apart from here, so a second definition wins and the
    // collision is logged rather than refused - a mod that does not care which
    // slot it gets should call DefineNextCharacter.
    if (slot.defined && slot.name != incoming.name) {
      REXLOG_WARN("party slots: character {} redefined, \"{}\" -> \"{}\"", character,
                     slot.name, incoming.name);
    }
    slot = incoming;
  }

  if (!SeedCharacterTemplate(character, incoming.template_source)) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_slots[character] = Slot{};
    return ETERNALSONATA_PARTY_ERR_UNAVAILABLE;
  }

  // The name goes through the ordinary rename path, so a defined slot resolves
  // its name everywhere the game draws one, exactly like a renamed retail
  // character does.
  EternalSonataSetCharacterName(character, incoming.name.c_str());

  // Last, so no gate opens on a slot whose template is not written yet.
  SetDefinedBit(character, true);
  REXLOG_INFO("party slots: character {} defined as \"{}\" (template from {}, model pc{:03d})",
              character, incoming.name, incoming.template_source,
              incoming.model_id ? incoming.model_id : character);
  return ETERNALSONATA_PARTY_OK;
}

int DefineNextCharacter(const EternalSonataCharacterDefinition* definition) {
  int candidate = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int character = kFirstAddedCharacter; character <= kCharacterCount; ++character) {
      if (!g_slots[character].defined) {
        candidate = character;
        break;
      }
    }
  }
  if (!candidate) {
    return ETERNALSONATA_PARTY_ERR_NO_SLOTS;
  }
  const int result = DefineCharacter(candidate, definition);
  return result < 0 ? result : candidate;
}

int UndefineCharacter(int character) {
  if (!IsAddedSlot(character)) {
    return ETERNALSONATA_PARTY_ERR_INVALID_CHARACTER;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_slots[character].defined) {
      return ETERNALSONATA_PARTY_ERR_SLOT_VACANT;
    }
  }

  // Shut the gates first: a character being taken out of the party still runs
  // through the game's own paths, but nothing new may reach for it afterwards.
  SetDefinedBit(character, false);
  if (EternalSonataGetCharacterPosition(character) > 0) {
    EternalSonataRemoveCharacterFromParty(character);
  }
  EternalSonataSetCharacterName(character, nullptr);
  ClearCharacterTemplate(character);

  std::lock_guard<std::mutex> lock(g_mutex);
  g_slots[character] = Slot{};
  REXLOG_INFO("party slots: character {} is vacant again", character);
  return ETERNALSONATA_PARTY_OK;
}

}  // namespace eternalsonata

// ---------------------------------------------------------------------------
// Public C ABI (see src/eternalsonata_party_api.h)
// ---------------------------------------------------------------------------

extern "C" __declspec(dllexport) int EternalSonataGetAddedSlotCount(void) {
  return eternalsonata::AddedSlotCount();
}

extern "C" __declspec(dllexport) int EternalSonataGetAddedSlot(int index) {
  return eternalsonata::AddedSlotAt(index);
}

extern "C" __declspec(dllexport) int EternalSonataIsCharacterDefined(int character) {
  return eternalsonata::IsCharacterDefined(character) ? 1 : 0;
}

extern "C" __declspec(dllexport) int EternalSonataDefineCharacter(
    int character, const EternalSonataCharacterDefinition* definition) {
  return eternalsonata::DefineCharacter(character, definition);
}

extern "C" __declspec(dllexport) int EternalSonataDefineNextCharacter(
    const EternalSonataCharacterDefinition* definition) {
  return eternalsonata::DefineNextCharacter(definition);
}

extern "C" __declspec(dllexport) int EternalSonataUndefineCharacter(int character) {
  return eternalsonata::UndefineCharacter(character);
}
