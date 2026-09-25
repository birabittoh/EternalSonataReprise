// eternalsonata - Script natives only the PS3 executable has.
//
// PS3 scripts import party natives 5026..5033 (the twelve character roster,
// docs/ps3-assets.md section 2) that the 360's 5000 table (26 entries) lacks.
// Unregistered ids are worse than missing: sub_820FF748's range check is
// inclusive, so 5026 reads past off_8240CA88 into the battle native table.
// Until their behaviour is known, each resolves to a host stub returning 0,
// which reads as "not in the party" to every call site seen so far.

#include "generated/eternalsonata_init.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <utility>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

namespace {

constexpr uint32_t kFirstId = 5026;
constexpr uint32_t kCount = 8;

std::array<std::atomic<bool>, kCount> g_reported{};
uint32_t g_table = 0;

template <uint32_t Index>
void Ps3NativeStub(PPCContext& ctx, uint8_t* base) {
  (void)base;
  if (!g_reported[Index].exchange(true))
    REXLOG_WARN("ps3 natives: script called native {}, which is stubbed to 0", kFirstId + Index);
  ctx.r3.u64 = 0;
}

template <uint32_t... I>
constexpr std::array<PPCFunc*, kCount> MakeStubs(std::integer_sequence<uint32_t, I...>) {
  return {&Ps3NativeStub<I>...};
}

uint32_t BuildTable() {
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  auto* dispatcher = runtime ? runtime->function_dispatcher() : nullptr;
  if (!memory || !dispatcher)
    return 0;
  const uint32_t table = memory->SystemHeapAlloc(4 * kCount, 0x20);
  if (!table)
    return 0;
  static constexpr auto stubs = MakeStubs(std::make_integer_sequence<uint32_t, kCount>{});
  for (uint32_t i = 0; i < kCount; ++i) {
    const uint32_t thunk = dispatcher->AllocateThunk(stubs[i], 0);
    if (!thunk)
      return 0;
    rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual(table + 4 * i), thunk);
  }
  return table;
}

}  // namespace

// sub_820F91A8 (named MEMORY_HEAP__Init in config/rtti_names.toml) registers
// the field native tables. The extra table is added right after; sub_820FF028
// ignores a table it already holds, so a second call is harmless.
REX_EXTERN(__imp__MEMORY_HEAP__Init);

REX_HOOK_RAW(MEMORY_HEAP__Init) {
  PPCContext call = ctx;
  __imp__MEMORY_HEAP__Init(ctx, base);
  if (!g_table && !(g_table = BuildTable())) {
    REXLOG_ERROR("ps3 natives: could not build the stub table");
    return;
  }
  // The table's count is inclusive of its last id, as sub_820FF748 checks it.
  call.r3.u64 = g_table;
  call.r4.u64 = kCount - 1;
  call.r5.u64 = kFirstId;
  sub_820FF028(call, base);
}
