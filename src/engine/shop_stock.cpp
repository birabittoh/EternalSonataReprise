// eternalsonata - The shop's stock list, thirty five items long.
//
// sub_8222C190 copies a shop's stock into word_82560114, 32 records of a u16
// item and a u16 nobody uses, with the item count at 0x82560198 and the shop
// id at 0x82560199 right behind them. The PS3 stocks up to 35 items, so the
// list moves to a guest buffer with room for them. Records past the 32nd
// would land on the count and id, which only the instructions below touch, so
// those are told apart by pc. Nothing else lives up to the sell list at
// 0x825601A2, which bounds the list at 35.
//
// In PS3 mode sub_8222C190 copies from the PS3's stock table instead: its
// records keep the 360's {id, count, items...} shape, 74 bytes long.

#include "shop_stock.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <mutex>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include "target.h"

namespace {

constexpr uint32_t kRecords = 0x82560114u;
constexpr uint32_t kCount = 0x82560198u;
constexpr uint32_t kId = 0x82560199u;
constexpr uint32_t kPad = 0x8256019Au;
constexpr uint32_t kMaxItems = 35u;
constexpr uint32_t kCountOffset = 0x90u;
constexpr uint32_t kBufferSize = 0x100u;

// Every instruction that loads or stores the count, the id or the byte after.
constexpr uint32_t kByteAccessors[] = {
    0x821FBD74u, 0x821FBD78u, 0x821FD5FCu, 0x821FD634u, 0x821FD6C0u, 0x821FD74Cu, 0x821FD7E8u,
    0x821FD884u, 0x821FD8E4u, 0x821FD96Cu, 0x821FD9F4u, 0x821FDA8Cu, 0x821FDB24u, 0x8222C278u,
    0x8222C294u, 0x8222C2A8u, 0x8222C2B0u, 0x8222C7E0u,
};

constexpr uint32_t kPs3ShopCount = 16;
constexpr uint32_t kPs3ShopSize = 74;
constexpr uint8_t kPs3Stock[][kPs3ShopSize] = {
#include "ps3_shop_stock.inc"
};
static_assert(std::size(kPs3Stock) == kPs3ShopCount);

std::once_flag g_buffer_once;
uint32_t g_buffer = 0;
std::once_flag g_stock_once;
uint32_t g_stock = 0;

uint32_t Allocate(uint32_t size) {
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  const uint32_t guest = memory ? memory->SystemHeapAlloc(size, 16) : 0;
  if (guest)
    std::memset(memory->TranslateVirtual(guest), 0, size);
  return guest;
}

}  // namespace

namespace eternalsonata {

uint32_t ShopStockAddress(uint32_t address, uint32_t pc) {
  std::call_once(g_buffer_once, [] {
    g_buffer = Allocate(kBufferSize);
    if (!g_buffer)
      REXLOG_ERROR("shop stock: could not allocate the stock list");
  });
  if (!g_buffer)
    return address;
  if (address >= kCount && address <= kPad &&
      std::find(std::begin(kByteAccessors), std::end(kByteAccessors), pc) !=
          std::end(kByteAccessors))
    return g_buffer + kCountOffset + (address - kCount);
  return g_buffer + (address - kRecords);
}

}  // namespace eternalsonata

// sub_8222C190's fill loop ends at the 32nd record (r8 = 0x82560194).
extern "C++" void EternalSonataShopStockEnd(PPCRegister& r8);

void EternalSonataShopStockEnd(PPCRegister& r8) {
  r8.u64 = r8.u32 + 4 * (kMaxItems - 32);
}

// The same loop's source, r10 = the 360's stock record + 4 for shop r28.
extern "C++" void EternalSonataShopStockSource(PPCRegister& r10, PPCRegister& r28);

void EternalSonataShopStockSource(PPCRegister& r10, PPCRegister& r28) {
  if (!eternalsonata::IsPs3Target())
    return;
  std::call_once(g_stock_once, [] {
    g_stock = Allocate(sizeof(kPs3Stock));
    if (g_stock)
      std::memcpy(rex::Runtime::instance()->memory()->TranslateVirtual(g_stock), kPs3Stock,
                  sizeof(kPs3Stock));
  });
  if (g_stock)
    r10.u64 = g_stock + kPs3ShopSize * (r28.u32 - 1) + 4;
}
