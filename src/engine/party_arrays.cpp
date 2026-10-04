// eternalsonata - The per-character party arrays, twelve entries wide.
//
// The relocation is made on the first access, from whichever guest thread
// gets there, by copying the retail arrays across, so it cannot miss state
// the game wrote first. Accesses outside the arrays but inside a remapped
// range (the gaps between them) keep their address.

#include "party_arrays.h"

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

#include <rex/logging.h>
#include <rex/memory/address_remap.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

uint32_t EternalSonataPartyRemap(uint32_t ea, const char* function, bool listed);

namespace eternalsonata {
namespace {

struct ArrayInfo {
  const char* name;
  uint32_t retail;
  uint32_t stride;
};

constexpr std::array<ArrayInfo, static_cast<size_t>(PartyArray::kCount)> kArrays{{
    {"position", 0x8243FC08u, 4},
    {"slotbytes", 0x8243FC30u, 1},
    {"charflags", 0x8243FCFCu, 1},
    {"stats_live", 0x8243FD08u, 48},
    {"stats_base", 0x8243FEE8u, 48},
    {"charwords", 0x824400C8u, 2},
    {"template", 0x82016150u, 136},
}};

// New arrays keep their retail address modulo 16, so an aligned vector access
// that stays inside an array stays inside it after the move.
constexpr uint32_t kSlotSize = 0x800;

std::once_flag g_once;
std::atomic<uint32_t> g_block{0};

uint32_t RelocatedBase(size_t i, uint32_t block) {
  return block + static_cast<uint32_t>(i) * kSlotSize + (kArrays[i].retail & 0xF);
}

void Relocate() {
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory)
    return;
  const uint32_t block = memory->SystemHeapAlloc(kSlotSize * kArrays.size(), 16);
  if (!block) {
    REXLOG_ERROR("party arrays: could not allocate the relocated arrays");
    return;
  }
  for (size_t i = 0; i < kArrays.size(); ++i) {
    const ArrayInfo& a = kArrays[i];
    static_assert(kPartyCharacterCount * 136 + 0xF <= kSlotSize);
    auto* dst = memory->TranslateVirtual<uint8_t*>(RelocatedBase(i, block));
    std::memset(dst, 0, kPartyCharacterCount * a.stride);
    std::memcpy(dst, memory->TranslateVirtual<uint8_t*>(a.retail),
                kRetailCharacterCount * a.stride);
  }
  g_block.store(block, std::memory_order_release);
  REXLOG_INFO("party arrays: relocated to {:08X}", block);
}

void ReportUnlisted(const char* function) {
  static std::mutex mutex;
  static std::unordered_set<std::string> seen;
  std::lock_guard lock(mutex);
  if (seen.emplace(function).second)
    REXLOG_WARN("party arrays: {} touches a party array but is not in config/party.toml",
                function);
}

}  // namespace

uint32_t PartyArrayAddress(PartyArray array, uint32_t index) {
  const size_t i = static_cast<size_t>(array);
  std::call_once(g_once, Relocate);
  const uint32_t block = g_block.load(std::memory_order_acquire);
  const uint32_t base = block ? RelocatedBase(i, block) : kArrays[i].retail;
  return base + index * kArrays[i].stride;
}

void InitPartyArrays(rex::Runtime* runtime) {
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory)
    return;
  static constexpr rex::memory::GuestAddressRange kRanges[] = {
      {0x8243FC08u, 0x8243FC3Au},
      {0x8243FCFCu, 0x8243FD06u},
      {0x8243FD08u, 0x824400DCu},
      {0x82016150u, 0x820166A0u},
  };
  rex::memory::SetGuestAddressRemap(memory, kRanges, [](uint32_t address) {
    return EternalSonataPartyRemap(address, "rexcrt", true);
  });
}

}  // namespace eternalsonata

uint32_t EternalSonataPartyRemap(uint32_t ea, const char* function, bool listed) {
  using namespace eternalsonata;
  for (size_t i = 0; i < kArrays.size(); ++i) {
    const ArrayInfo& a = kArrays[i];
    const uint32_t offset = ea - a.retail;
    if (offset >= kRetailCharacterCount * a.stride)
      continue;
    if (!listed)
      ReportUnlisted(function);
    std::call_once(g_once, Relocate);
    const uint32_t block = g_block.load(std::memory_order_acquire);
    return block ? RelocatedBase(i, block) + offset : ea;
  }
  return ea;
}
