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

  // Characters 11 and 12 start as copies of character 1 with their own id, so
  // that a cast of twelve is playable before any mod supplies real data. The id
  // at +0 is a big-endian u16 and is what the entity lookup matches on, so it is
  // the one field that must not be a copy.
  for (uint32_t i = kTemplateSeedCount; i < kRelocatedCharacterCount; ++i) {
    uint8_t* entry = dest + i * kTemplateStride;
    std::memcpy(entry, source, kTemplateStride);
    const uint16_t id = static_cast<uint16_t>(i + 1);
    entry[0] = static_cast<uint8_t>(id >> 8);
    entry[1] = static_cast<uint8_t>(id & 0xFF);
  }

  REXLOG_INFO("party relocation: seeded {} template entries at {:#010x} "
              "({} copied from {:#010x}, {} placeholder)",
              kRelocatedCharacterCount, kTemplateBase, kTemplateSeedCount,
              kTemplateSource, kRelocatedCharacterCount - kTemplateSeedCount);
  return true;
}

}  // namespace eternalsonata
