// eternalsonata - Score piece marks, any number at once (score_piece_marks.h).
//
// The Score Pieces menu (sub_8220A588 builds it, sub_8220DEC8 runs it) has a
// single Mark sprite at +0x180 that it moves to the marked row. The PS3 menu
// makes one per piece instead, and so does this: one per listed row, made
// with the stock sprite's own arguments and put in the same scroll group, so
// the list scroll carries them. The stock sprite is parked and the menu's
// marked piece (+0x184) kept at 0, which turns off every place the game
// would move it; the open and close slides move it, so they are replayed for
// the rest.

#include "score_piece_marks.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

REX_EXTERN(__imp__sub_821EF0E8);
REX_EXTERN(__imp__sub_82179F78);
REX_EXTERN(sub_821F6580);
REX_EXTERN(sub_82178A88);
REX_EXTERN(sub_8217BF28);
REX_EXTERN(sub_82178950);
REX_EXTERN(sub_821F7AA0);
REX_EXTERN(sub_821425D8);

namespace {

constexpr int kPieces = 32;
constexpr std::uint32_t kMarkAddr = 0x8243FCF4u;  // u8 piece number, 0 for none
constexpr std::uint32_t kListAddr = 0x8243FCD4u;  // u8[32], collected piece numbers
constexpr char kRecordKey[] = "score_pieces.marked";

std::atomic<std::uint32_t> g_marks{0};

std::uint8_t* GuestBase() {
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  return memory ? memory->TranslateVirtual(0) : nullptr;
}

std::uint8_t LowestMarked(std::uint32_t marks) {
  return marks ? static_cast<std::uint8_t>(std::countr_zero(marks) + 1) : 0;
}

// The PS3 record has bit n set for the nth row of the collected list, not for
// piece number n + 1, so the list is the way between the two.
std::uint32_t PiecesToRows(std::uint32_t pieces) {
  const std::uint8_t* base = GuestBase();
  std::uint32_t rows = 0;
  for (int row = 0; base && row < kPieces; ++row) {
    const std::uint8_t number = base[kListAddr + row];
    if (number >= 1 && number <= kPieces && (pieces >> (number - 1) & 1u))
      rows |= 1u << row;
  }
  return rows;
}

std::uint32_t RowsToPieces(std::uint32_t rows) {
  const std::uint8_t* base = GuestBase();
  std::uint32_t pieces = 0;
  for (int row = 0; base && row < kPieces; ++row) {
    const std::uint8_t number = base[kListAddr + row];
    if (number >= 1 && number <= kPieces && (rows >> row & 1u))
      pieces |= 1u << (number - 1);
  }
  return pieces;
}

void WriteMarkByte(std::uint32_t marks) {
  if (std::uint8_t* base = GuestBase())
    base[kMarkAddr] = LowestMarked(marks);
}

}  // namespace

namespace eternalsonata {

std::uint32_t ScorePieceMarks() { return g_marks.load(std::memory_order_relaxed); }

void SetScorePieceMarks(std::uint32_t marks) {
  g_marks.store(marks, std::memory_order_relaxed);
  WriteMarkByte(marks);
}

void SaveScorePieceMarks(SaveRecord& record) {
  char hex[9];
  std::snprintf(hex, sizeof(hex), "%08X", PiecesToRows(ScorePieceMarks()));
  record[kRecordKey] = hex;
}

// A save from before the record had the key keeps the one Mark it had.
void LoadScorePieceMarks(const SaveRecord& record) {
  const auto it = record.find(kRecordKey);
  if (it != record.end()) {
    g_marks.store(RowsToPieces(static_cast<std::uint32_t>(
                      std::strtoul(it->second.c_str(), nullptr, 16))),
                  std::memory_order_relaxed);
    WriteMarkByte(ScorePieceMarks());
    return;
  }
  const std::uint8_t* base = GuestBase();
  const std::uint8_t number = base ? base[kMarkAddr] : 0;
  g_marks.store(number >= 1 && number <= kPieces ? 1u << (number - 1) : 0u,
                std::memory_order_relaxed);
}

void ResetScorePieceMarks() { g_marks.store(0, std::memory_order_relaxed); }

}  // namespace eternalsonata

namespace score_piece_marks {
namespace {

constexpr std::uint32_t kMarkImage = 0x15Du;
constexpr std::uint32_t kObjects = 0x824CF500u;
constexpr std::uint32_t kUiRootAddr = 0x824400E4u;
constexpr std::uint32_t kScrollGroupAddr = 0x8243F234u;
constexpr std::uint32_t kSoundManagerAddr = 0x8243D89Cu;
constexpr std::uint32_t kSoundMark = 5u;

// The menu: the stock Mark, the marked piece and whether one is shown, then
// the rows as piece number - 1 and their count.
constexpr std::uint32_t kMenuMark = 0x180u;
constexpr std::uint32_t kMenuMarked = 0x184u;
constexpr std::uint32_t kMenuMarkShown = 0x185u;
constexpr std::uint32_t kMenuRows = 0x190u;
constexpr std::uint32_t kMenuRowCount = 0x210u;

// Where sub_8220A588 puts row i before the open slide lifts the list by 740,
// and the x the Mark sits at, or is parked at.
constexpr float kBuildTop = 1075.0f;
constexpr float kRowPitch = 105.0f;
constexpr float kShownX = 85.0f;
constexpr float kParkedX = 1600.0f;
// Rows on screen once open: 335, 440 and 545.
constexpr float kVisibleTop = 300.0f;
constexpr float kVisibleBottom = 600.0f;

// A screen object's clip: enabled flag, its dirty flag, then the rectangle
// the rows use (top, bottom, left, right).
constexpr std::uint32_t kClipOn = 0x94u;
constexpr std::uint32_t kClipDirty = 0x95u;
constexpr std::uint32_t kClipRect = 0x98u;
constexpr std::uint32_t kRowClip[] = {335u, 665u, 65u, 1215u};
constexpr std::uint32_t kObjectNode = 0x30u;
constexpr std::uint32_t kMarkPriority = 0x16u;

constexpr std::uint32_t kNone = 0xFFFFFFFFu;

struct View {
  std::uint32_t menu = 0;
  std::uint32_t stock = kNone;
  int rows = 0;
  std::array<std::uint32_t, kPieces> marks{};  // by row
  std::uint32_t parked = 0;                   // rows hidden for the close slide
};
View g_view;

std::uint32_t g_building = 0;  // the menu sub_8220A588 is building
bool g_made = false;
std::array<std::uint32_t, kPieces> g_made_marks{};
std::uint32_t g_sliding = 0;  // the menu whose open or close slide is running
std::uint32_t g_scratch = 0;

bool Valid(std::uint32_t handle) { return handle != kNone && handle != 0; }

std::uint32_t Scratch() {
  if (!g_scratch) {
    if (auto* mem = rex::system::kernel_memory())
      g_scratch = mem->SystemHeapAlloc(32, 0x20);
  }
  return g_scratch;
}

std::uint32_t Resolve(PPCContext& ctx, std::uint8_t* base, std::uint32_t handle) {
  PPCContext call = ctx;
  call.r3.u64 = REX_LOAD_U32(kUiRootAddr);
  call.r4.u64 = handle;
  sub_821F6580(call, base);
  return call.r3.u32;
}

struct Vec {
  float x, y, z;
};

Vec Position(PPCContext& ctx, std::uint8_t* base, std::uint32_t handle) {
  const std::uint32_t out = Scratch();
  PPCContext call = ctx;
  call.r3.u64 = out;
  call.r4.u64 = kObjects;
  call.r5.u64 = handle;
  sub_8217BF28(call, base);
  return {std::bit_cast<float>(REX_LOAD_U32(out)), std::bit_cast<float>(REX_LOAD_U32(out + 4)),
          std::bit_cast<float>(REX_LOAD_U32(out + 8))};
}

std::uint32_t StoreVec(std::uint8_t* base, const Vec& v) {
  const std::uint32_t at = Scratch() + 16;
  REX_STORE_U32(at, std::bit_cast<std::uint32_t>(v.x));
  REX_STORE_U32(at + 4, std::bit_cast<std::uint32_t>(v.y));
  REX_STORE_U32(at + 8, std::bit_cast<std::uint32_t>(v.z));
  return at;
}

void Place(PPCContext& ctx, std::uint8_t* base, std::uint32_t handle, float x, float y) {
  PPCContext call = ctx;
  call.r3.u64 = kObjects;
  call.r4.u64 = handle;
  call.r5.u64 = StoreVec(base, {x, y, 0.0f});
  call.r6.u64 = 0;
  call.r7.u64 = 0;
  call.r8.s64 = -1;
  sub_82178A88(call, base);
}

void SetClip(std::uint8_t* base, std::uint32_t object, bool on) {
  const std::uint8_t flag = on ? 1 : 0;
  if (REX_LOAD_U8(object + kClipOn) != flag)
    REX_STORE_U8(object + kClipDirty, 1);
  REX_STORE_U8(object + kClipOn, flag);
}

bool Marked(std::uint32_t piece) {
  return piece < kPieces && (eternalsonata::ScorePieceMarks() >> piece & 1u);
}

int CollectedCount(std::uint8_t* base) {
  int count = 0;
  while (count < kPieces && REX_LOAD_U8(kListAddr + count))
    ++count;
  return count;
}

// The rest of what sub_8220A588 does to the stock Mark: the rows' clip, the
// draw priority and the scroll group. Unlike it, the clip is on from the
// start, so rows below the first three do not show through the open slide.
void Adopt(PPCContext& ctx, std::uint8_t* base, std::uint32_t menu) {
  g_view = {};
  g_view.menu = menu;
  g_view.stock = REX_LOAD_U32(menu + kMenuMark);
  g_view.rows = std::min<int>(static_cast<int>(REX_LOAD_U32(menu + kMenuRowCount)), kPieces);
  g_view.marks = g_made_marks;
  for (int row = 0; row < g_view.rows; ++row) {
    const std::uint32_t mark = g_view.marks[row];
    if (!Valid(mark))
      continue;
    if (const std::uint32_t object = Resolve(ctx, base, mark)) {
      for (std::uint32_t i = 0; i < 4; ++i)
        REX_STORE_U32(object + kClipRect + 4 * i, kRowClip[i]);
      SetClip(base, object, true);
      PPCContext call = ctx;
      call.r3.u64 = kObjects;
      call.r4.u64 = REX_LOAD_U32(object + kObjectNode);
      call.r5.u64 = kMarkPriority;
      call.r6.u64 = 1;
      call.r7.u64 = 0;
      call.r8.s64 = -1;
      sub_82178950(call, base);
    }
    PPCContext call = ctx;
    call.r3.u64 = REX_LOAD_U32(kScrollGroupAddr);
    call.r4.u64 = mark;
    sub_821F7AA0(call, base);
    const std::uint32_t piece = REX_LOAD_U32(menu + kMenuRows + 4u * row);
    Place(ctx, base, mark, Marked(piece) ? kShownX : kParkedX, kBuildTop + kRowPitch * row);
  }
  if (Valid(g_view.stock))
    Place(ctx, base, g_view.stock, kParkedX, kBuildTop);
  REX_STORE_U8(menu + kMenuMarked, 0);
  REX_STORE_U8(menu + kMenuMarkShown, eternalsonata::ScorePieceMarks() ? 1 : 0);
}

template <typename Fn>
void ForEachMark(Fn&& fn) {
  for (int row = 0; row < g_view.rows; ++row) {
    if (Valid(g_view.marks[row]))
      fn(row, g_view.marks[row]);
  }
}

}  // namespace

void ImageMade(PPCContext& before, std::uint8_t* base, std::uint32_t image_id,
               std::uint32_t handle) {
  if (!g_building || g_made || image_id != kMarkImage || !Valid(handle))
    return;
  g_made = true;
  g_made_marks.fill(kNone);
  const int count = CollectedCount(base);
  for (int row = 0; row < count; ++row) {
    PPCContext call = before;
    __imp__sub_821EF0E8(call, base);
    g_made_marks[row] = call.r3.u32;
    if (!Valid(call.r3.u32)) {
      REXLOG_WARN("[score marks] no sprite for row {} of {}", row, count);
      break;
    }
  }
}

// The slides read the stock Mark's position and send it 740 up or down; every
// other Mark makes the same move from where it is.
void MirrorSlide(PPCContext& ctx, std::uint8_t* base) {
  if (!g_sliding || g_sliding != g_view.menu || ctx.r4.u32 != g_view.stock || !Scratch())
    return;
  const Vec from = Position(ctx, base, g_view.stock);
  const std::uint32_t target = ctx.r5.u32;
  const Vec delta{std::bit_cast<float>(REX_LOAD_U32(target)) - from.x,
                  std::bit_cast<float>(REX_LOAD_U32(target + 4)) - from.y,
                  std::bit_cast<float>(REX_LOAD_U32(target + 8)) - from.z};
  ForEachMark([&](int, std::uint32_t mark) {
    const Vec at = Position(ctx, base, mark);
    PPCContext call = ctx;
    call.r4.u64 = mark;
    call.r5.u64 = StoreVec(base, {at.x + delta.x, at.y + delta.y, at.z + delta.z});
    __imp__sub_82179F78(call, base);
  });
}

}  // namespace score_piece_marks

using namespace score_piece_marks;

REX_EXTERN(__imp__sub_8220A588);
REX_EXTERN(__imp__sub_8220C988);
REX_EXTERN(__imp__sub_8220DB50);
REX_EXTERN(__imp__sub_8220E110);
REX_EXTERN(__imp__sub_8220E550);
REX_EXTERN(__imp__sub_8220DEC8);

// sub_8220A588(menu) builds the screen, the Mark sprite among it.
REX_HOOK_RAW(sub_8220A588) {
  const std::uint32_t menu = ctx.r3.u32;
  g_building = menu;
  g_made = false;
  __imp__sub_8220A588(ctx, base);
  g_building = 0;
  if (g_made && Scratch())
    Adopt(ctx, base, menu);
  g_made = false;
}

// The open slide, also run when the list comes back from a piece's view.
REX_HOOK_RAW(sub_8220C988) {
  const std::uint32_t menu = ctx.r3.u32;
  if (menu == g_view.menu && Scratch()) {
    ForEachMark([&](int row, std::uint32_t mark) {
      if (g_view.parked >> row & 1u)
        Place(ctx, base, mark, kShownX, Position(ctx, base, mark).y);
    });
    g_view.parked = 0;
  }
  g_sliding = menu;
  __imp__sub_8220C988(ctx, base);
  g_sliding = 0;
}

// The close slide turns the stock Mark's clip off so it can leave the list
// window. Marks on screen do the same; the rest would cross the screen on
// their way down, so they are parked until the list opens again.
REX_HOOK_RAW(sub_8220DB50) {
  const std::uint32_t menu = ctx.r3.u32;
  if (menu == g_view.menu && Scratch()) {
    g_view.parked = 0;
    ForEachMark([&](int row, std::uint32_t mark) {
      const Vec at = Position(ctx, base, mark);
      if (at.x != kShownX)
        return;
      if (at.y >= kVisibleTop && at.y < kVisibleBottom) {
        if (const std::uint32_t object = Resolve(ctx, base, mark))
          SetClip(base, object, false);
      } else {
        Place(ctx, base, mark, kParkedX, at.y);
        g_view.parked |= 1u << row;
      }
    });
  }
  g_sliding = menu;
  __imp__sub_8220DB50(ctx, base);
  g_sliding = 0;
}

// sub_8220E110(menu, direction) runs before a scroll and clips the stock Mark
// again.
REX_HOOK_RAW(sub_8220E110) {
  const std::uint32_t menu = ctx.r3.u32;
  const bool scrolling = ctx.r4.u32 != 0;
  __imp__sub_8220E110(ctx, base);
  if (menu != g_view.menu || !scrolling)
    return;
  PPCContext call = ctx;
  ForEachMark([&](int, std::uint32_t mark) {
    if (const std::uint32_t object = Resolve(call, base, mark))
      SetClip(base, object, true);
  });
}

// sub_8220E550(menu, piece - 1) is Mark: it moves the one sprite to the row or
// takes it off. Here it toggles that piece's own.
REX_HOOK_RAW(sub_8220E550) {
  const std::uint32_t menu = ctx.r3.u32;
  const std::uint32_t piece = ctx.r4.u32;
  if (menu != g_view.menu || piece >= kPieces || !Scratch()) {
    __imp__sub_8220E550(ctx, base);
    return;
  }
  PPCContext call = ctx;
  call.r3.u64 = REX_LOAD_U32(kSoundManagerAddr);
  call.r4.u64 = kSoundMark;
  call.r5.u64 = 0;
  call.r6.u64 = 0;
  sub_821425D8(call, base);

  const std::uint32_t marks = eternalsonata::ScorePieceMarks() ^ (1u << piece);
  eternalsonata::SetScorePieceMarks(marks);
  REX_STORE_U8(menu + kMenuMarkShown, marks ? 1 : 0);
  for (int row = 0; row < g_view.rows; ++row) {
    if (REX_LOAD_U32(menu + kMenuRows + 4u * row) != piece || !Valid(g_view.marks[row]))
      continue;
    const std::uint32_t mark = g_view.marks[row];
    Place(ctx, base, mark, Marked(piece) ? kShownX : kParkedX, Position(ctx, base, mark).y);
    break;
  }
}

// The menu's tick. Closing writes its marked piece, kept at 0 here, back to
// byte_8243FCF4, which is put right again after.
REX_HOOK_RAW(sub_8220DEC8) {
  __imp__sub_8220DEC8(ctx, base);
  const std::uint8_t lowest = LowestMarked(eternalsonata::ScorePieceMarks());
  if (REX_LOAD_U8(kMarkAddr) != lowest)
    REX_STORE_U8(kMarkAddr, lowest);
}
