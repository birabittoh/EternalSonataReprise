// eternalsonata - Guest memory for the relocated per-character party arrays.
// See party_relocation.h for what moves where and why.

#include "party_relocation.h"

#include <cstring>

#include <rex/logging.h>
#include <rex/runtime.h>

namespace eternalsonata {
namespace {

// Kept so the count hooks can reach guest memory. They run on the guest thread
// long after setup, and a mid-ASM hook is handed registers and nothing else.
rex::memory::Memory* g_memory = nullptr;

}  // namespace

uint8_t* PartyGuestPointer(uint32_t guest_address) {
  return g_memory ? g_memory->TranslateVirtual<uint8_t*>(guest_address) : nullptr;
}

bool ReservePartyRelocationMemory(rex::Runtime* runtime) {
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory) {
    REXLOG_ERROR("party relocation: no memory subsystem");
    return false;
  }

  auto* heap = memory->LookupHeap(kRelocationBase);
  if (!heap) {
    REXLOG_ERROR("party relocation: no heap covers {:#010x}", kRelocationBase);
    return false;
  }

  // Reserve and commit in one call. A failure here means something else already
  // owns the page, which for an address this far past the image would mean the
  // layout assumption in the header no longer holds; report it rather than
  // silently running with hooks pointing at nothing.
  if (!heap->AllocFixed(kRelocationBase, kRelocationSize, heap->page_size(),
                        rex::memory::kMemoryAllocationReserve |
                            rex::memory::kMemoryAllocationCommit,
                        rex::memory::kMemoryProtectRead |
                            rex::memory::kMemoryProtectWrite)) {
    REXLOG_ERROR("party relocation: could not commit {:#x} bytes at {:#010x}",
                  kRelocationSize, kRelocationBase);
    return false;
  }

  // The arrays the game clears per element are covered by the reset paths, but
  // the two new entries per array are never written by guest code that has not
  // been taught about them yet, so start from zero rather than whatever the
  // page happens to hold.
  auto* host = memory->TranslateVirtual<uint8_t*>(kRelocationBase);
  if (!host) {
    REXLOG_ERROR("party relocation: {:#010x} did not translate", kRelocationBase);
    return false;
  }
  std::memset(host, 0, kRelocationSize);

  if (!SeedTemplateTable(memory)) {
    return false;
  }

  g_memory = memory;

  // Only after the copies exist: the seed above reads the template table out of
  // the image, and the poison must not run before anything that still depends on
  // the old addresses holding their original contents.
  if (!PoisonVacatedPartyBlock(memory)) {
    return false;
  }

  REXLOG_INFO("party relocation: reserved {:#x} bytes at {:#010x} for {} characters",
               kRelocationSize, kRelocationBase, kRelocatedCharacterCount);
  return true;
}

bool SeedTemplateTable(rex::memory::Memory* memory) {
  // The other relocated arrays are .bss state the game clears for itself. This
  // one is constant source data that no guest code ever writes, so a zeroed copy
  // would give every character zero growth curves and zero starting stats.
  const auto* source = memory->TranslateVirtual<const uint8_t*>(kTemplateSource);
  auto* dest = memory->TranslateVirtual<uint8_t*>(kTemplateBase);
  if (!source || !dest) {
    REXLOG_ERROR("party relocation: template {:#010x} -> {:#010x} did not translate",
                  kTemplateSource, kTemplateBase);
    return false;
  }

  std::memcpy(dest, source, kTemplateSeedSize);

  // The added slots' entries stay zeroed. They are vacant: no mod has claimed
  // them yet, the id gates in party_bounds.cpp are shut for them, and nothing
  // reads a zeroed entry until a definition fills it in. Seeding a placeholder
  // here instead would be the host inventing a character, which is exactly what
  // the slots exist to avoid.
  std::memset(dest + kTemplateSeedSize, 0,
              (kRelocatedCharacterCount - kTemplateSeedCount) * kTemplateStride);

  REXLOG_INFO("party relocation: seeded {} template entries at {:#010x} from {:#010x}, "
              "{} slots left vacant",
              kTemplateSeedCount, kTemplateBase, kTemplateSource,
              kRelocatedCharacterCount - kTemplateSeedCount);
  return true;
}

namespace {

// Host pointer to added slot `character`'s template entry, or nullptr if the
// character is not an added slot or the page is not reserved yet.
uint8_t* AddedTemplateEntry(int character) {
  if (character <= static_cast<int>(kTemplateSeedCount) ||
      character > static_cast<int>(kRelocatedCharacterCount)) {
    return nullptr;
  }
  return PartyGuestPointer(kTemplateBase + kTemplateStride * (character - 1));
}

}  // namespace

bool SeedCharacterTemplate(int character, int source) {
  if (source < 1 || source > static_cast<int>(kTemplateSeedCount)) {
    return false;
  }
  uint8_t* entry = AddedTemplateEntry(character);
  const uint8_t* from =
      PartyGuestPointer(kTemplateSource + kTemplateStride * (source - 1));
  if (!entry || !from) {
    REXLOG_ERROR("party slots: no template memory for character {}", character);
    return false;
  }

  std::memcpy(entry, from, kTemplateStride);
  // The id at +0 is a big-endian u16 and is what the entity lookup matches on,
  // so it is the one field that must not be a copy.
  const uint16_t id = static_cast<uint16_t>(character);
  entry[0] = static_cast<uint8_t>(id >> 8);
  entry[1] = static_cast<uint8_t>(id & 0xFF);
  return true;
}

bool ClearCharacterTemplate(int character) {
  uint8_t* entry = AddedTemplateEntry(character);
  if (!entry) {
    return false;
  }
  std::memset(entry, 0, kTemplateStride);
  return true;
}

bool PoisonVacatedPartyBlock(rex::memory::Memory* memory) {
  for (const auto& range : kPoisonRanges) {
    auto* host = memory->TranslateVirtual<uint8_t*>(range.begin);
    if (!host) {
      REXLOG_ERROR("party relocation: poison range {:#010x} did not translate",
                   range.begin);
      return false;
    }
    std::memset(host, kPoisonByte, range.end - range.begin);
  }
  REXLOG_INFO("party relocation: poisoned the vacated block with {:#04x}; "
              "any hit reported after this is a site the sweep missed",
              kPoisonByte);
  return true;
}

uint32_t CheckPartyPoison() {
  if (!g_memory) {
    return 0;
  }

  // One report per distinct byte, tracked here rather than by repairing the
  // byte in place. Repairing looks simpler and costs the main reason to poison
  // the block in the first place: a host write into the watched range trips any
  // hardware watchpoint set on it, so the first thing an lldb session catches is
  // this function rather than the guest instruction being hunted. Nothing here
  // writes guest memory now, so a watchpoint over kPoisonRanges sees only the
  // guest.
  static bool reported[kPoisonTotalSize] = {};

  uint32_t hits = 0;
  uint32_t base = 0;
  for (const auto& range : kPoisonRanges) {
    const uint32_t size = range.end - range.begin;
    const auto* host = g_memory->TranslateVirtual<const uint8_t*>(range.begin);
    if (!host) {
      base += size;
      continue;
    }
    for (uint32_t i = 0; i < size; ++i) {
      if (host[i] == kPoisonByte || reported[base + i]) {
        continue;
      }
      REXLOG_ERROR("party relocation: {:#010x} was written ({:#04x}); an "
                   "instruction still addresses the vacated party block. "
                   "Re-run scripts/party_relocation_scan.py and check what "
                   "reaches this address.",
                   range.begin + i, host[i]);
      reported[base + i] = true;
      ++hits;
    }
    base += size;
  }
  return hits;
}

}  // namespace eternalsonata
