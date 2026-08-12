#include "generated/eternalsonata_init.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include <rex/cvar.h>

#include "eternalsonata_hooks_internal.h"
#include "settings.h"

// frame_rate cvar, needed by FrameRateGetIndex below. Defined (and persisted)
// in settings.cpp.
REXCVAR_DECLARE(std::string, frame_rate);
REXCVAR_DECLARE(bool, menu_scan);

// ---------------------------------------------------------------------------
// Native option rows in the Options screen
// ---------------------------------------------------------------------------
//
// Screens are static display lists walked by sub_821F2F38 (full derivation in
// docs/debug-hooks.md §14): a stream of variable-length records, each tagged
// at +0 with `type = index * 100`, dispatched through word_82082428 and ended
// by a record whose type is 0xFFFF. A text label is type 200, 0x28 bytes, with
// a BTX string id at +4 and absolute X/Y at +8/+0xC.
//
// Because every record carries its own absolute position, rows can simply be
// appended: we copy the Options list, add a label + N value text records per
// row before a fresh terminator, and point the interpreter at the copy. The
// records use synthetic string ids that no BTX block defines, and the
// sub_8223B780 hook below answers them directly - so no game data is repacked
// and no asset has to be rebuilt.
//
// Rows are table-driven (kOptionRows below): each has a label, an ordered
// list of values (drawn side by side, exactly as the stock two-option rows
// do), and get/set callbacks that resolve to whichever index is active. A
// value can either author its own literal text (for values with no stock
// analogue, e.g. "60 FPS") or reuse a stock BTX id (for "Si"/"NO", which stay
// localised for free). See docs/debug-hooks.md §14 for the underlying RE.

namespace {

// The Options display list is duplicated once per language - same byte
// layout, different address - rather than sharing one list with
// language-independent string ids the way BTX text lookups do. Live-tested
// 2026-08-06 by watching sub_821F2F38's actual argument per language (ids
// found empirically, not derivable from the language byte's own switch below -
// this is a different value entirely, an argument computed by the screen's
// caller). Indexed by LanguageIndex() below: en, de, fr, es, it.
constexpr u32 kOptionsListByLang[5] = {
    0x8202F388u,  // en
    0x820723F8u,  // de
    0x82068648u,  // fr
    0x8206D520u,  // es
    0x8205E880u,  // it
};
constexpr u32 kOptionsListBytes = 0x868u;  // up to, excluding, its terminator

// Splice point: just past the whole Sottotitoli row group - its label
// (0x8205EF64), the "Si" and "NO" value records, and the type-600 underline -
// i.e. guest 0x8205EFEC. Sottotitoli is the closest analogue to what we are
// adding (a boolean row), so inserting here puts our records in exactly the
// drawing state the real rows are in. Appending at the end of the list instead
// made the label render offset from its own cursor position.
constexpr u32 kInsertOffset = 0x76Cu;
constexpr u32 kTextRecord = 200u;          // type 200 = text label
constexpr u32 kTextRecordBytes = 0x28u;
// Type 100 is a whole family: its handler at 0x821F35DC sub-dispatches on
// `type - 100` across 21 shapes (word_820823E0). Plain 100 is the highlight
// bar, {100, sprite, height, y, 1000, 1000, -1}, 0x1C bytes.
//
// `+4` is NOT an X coordinate. The handler at 0x821F3610 loads it with `lwz`
// and passes it to sub_821EF0E8 as an integer, while `+8` and `+0xC` go
// through fcfid into floats - so +4 is a sprite/resource id, and a bar carries
// no authored X at all (the row handler positions it). Writing a coordinate
// there makes sub_821EF0E8 fail, and it stores its -1 into the id array while
// still bumping the count - which crashes the per-frame walk a few seconds
// later. Reuse the game's own sprite id.
constexpr u32 kIconRecord = 100u;
constexpr u32 kIconRecordBytes = 0x1Cu;

// Type 600 is the grey rule between rows: {600, x, y, width}, 0x10 bytes,
// emitted last in a row's group. The stock Sottotitoli one at 0x75C is
// {600, 120, 328, 950} against a row whose text sits at y=285 - so a rule is
// drawn 43px below its own row's text and spans the full menu width, whatever
// that row's label and values are.
//
// They separate rather than underline: every stock row group ends with one
// EXCEPT the last row of a section (the row at y=335, the one directly above
// ours, has no 600 record). So each rule is really "above the next row", which
// is why we emit ours above each row rather than below - that gives the
// previously-last stock row the separator it now needs, and correctly leaves
// no rule under our own bottom row.
//
// A 600 record creates no entry in the screen's id array (that is the "creates
// nothing at all" note above - it is a draw command, not an object), which is
// why adding these is safe: unlike a type-100 record they cannot renumber the
// highlight bars.
constexpr u32 kSepRecordOffset = 0x75Cu;
constexpr u32 kSepRecordBytes = 0x10u;
constexpr u32 kSepYOffset = 0x08u;  // y within the record

// The bar is a **type-110** record - the 100 family, subtype 10 - at list
// offset 0x6C4: {110, 234, 490, 285, 1000, 1000, -1}. Sprite 234, x 490 (the
// left option's column) and y 285 (the row's own display y). It sits *before*
// the row's text, because the bar draws behind it.
//
// Two wrong turns, recorded so they are not repeated:
//   - type 600 ({600, 120, 328, 950} at 0x75C) creates nothing at all - the id
//     count did not even move. It is the underline, as originally named.
//   - type 100 subtype 0 ({100, 350, ...} at 0x76C) is the small subtitle
//     ICON, one per row. Right family, wrong subtype.
// Only the 100 family creates entries in the id array; text (200) and 600 do
// not, which is why our three text records never disturbed the row indices.
// Clone the whole stock block, not just the bar record. A bar on its own comes
// out visibly darker than the stock ones: the {2101, 2100, 1} record before it
// sets the draw state the type-100 family handler branches on (the r27 test at
// 0x821F3610), and the {2} after it restores that state. Emitting the bar
// outside the pair leaves it drawing with whatever blend the preceding icon
// record left behind.
constexpr u32 kBarRecordOffset = 0x6B8u;  // {2101,2100,1} + bar + {2}
constexpr u32 kBarRecordBytes = 0x2Cu;
constexpr int32_t kBarShrinkFixup = 7;    // see below
constexpr u32 kBarBlockYOffset = 0x18u;   // the bar record's y within the block
// +0x10 / +0x14 of a type-100-family record are a size scale in thousandths
// (the stock records carry 1000, and the two at list offset 0x580 carry 949).
constexpr u32 kBarBlockWOffset = 0x1Cu;
constexpr u32 kBarBlockHOffset = 0x20u;
// Bar placement, taken from the game rather than measured.
//
// sub_82200FE8 is the Options screen init, and it places every bar itself with
// sub_82178A88 - an instant set - so the y authored in the display-list record
// never decides where a bar ends up. That is why the record's y could not be
// made to agree with the runtime one: there are two coordinate spaces, and the
// record is in neither.
//
//   slot   init x                         init y   handler y (sub_82201620)
//   +84    base + 200*(1-byte_8243FBFC)      895      155
//   +88    base + 200*dword_8243F368         945      205
//   +92    base + 200*(BYTE2(FC04)^1)       1205      465
//   +120   base + 200*(1-BYTE1(FC04))       1155      415
//
// The two spaces differ by a constant 740 on every row. Rows follow Subtitles
// (415) and Voce (465) on the same 50px pitch, so row r's handler-space y is
// 515 + 50*r and its init-space y is that + 740. The x formula is the same in
// both spaces: base + 200 * value_index.
//
// Bar size: 650 x 800 thousandths of the stock bar, measured against the real
// rows. +0x10 / +0x14 of a type-100-family record are that scale (the stock
// records carry 1000; the two at list offset 0x580 carry 949).
// Dumping the stock Subtitles bar object next to ours (see DumpHighlightBars)
// showed them identical but for two things: y, differing by exactly the 100 of
// two row pitches, and the scale at +0x4C/+0x50 - 1.0/1.0 stock against our
// 0.65/0.80. So the leftover "few pixels too high" is entirely the height
// scale: the sprite is anchored at its top, so shrinking it lifts the bottom
// edge and the bar's centre rises by (1 - scale) * height / 2. The correction
// is 7px, settled against the real rows - close to the 5px a 50px-tall bar
// would predict, so the sprite is a little taller than the row pitch. Applied
// to every y value so they stay in step.
constexpr int32_t kBarHeight = 800;

// Moving the bar. sub_82201620 does exactly this on every value change:
//   sub_82179F78(&dword_824CF500, id, &{x, y, 0}, 0.0, 14.0, 14.0, 14.0)
// with r10 = 15 (the call site at 0x82201B34 sets it; it lands in the params
// block as a mode field, so it is not optional). The two float constants are
// flt_82016120 = 0.0 and flt_820AAC3C = 14.0, read out of the image.
//
// x = base + 200 * value_index, where base is language dependent, and the
// runtime y is the record's display y + 130 (stock: record 285 -> move 415).
constexpr u32 kTextRegistry = 0x824CF500u;
constexpr u32 kBarMoveMode = 15u;
constexpr double kBarMoveSpeed = 14.0;
constexpr int32_t kBarColumnStride = 200;
constexpr u32 kLanguage = 0x8243D370u;

constexpr u32 kListTerminator = 0x0000FFFFu;

// Row layout: 50px pitch starting just below Voce (285, 335), matching the
// stock rows' own pitch.
constexpr int32_t kRowY0 = 385;
constexpr int32_t kRowYStep = 50;
constexpr int32_t kRowXLabel = 120;   // same in every language (verified: label x doesn't shift)
// The value column's *base* x is per-language (Italian/French 490, English
// 440, ... - confirmed 2026-08-06 by diffing the stock Sottotitoli record
// across languages), so it is read at runtime from that very record rather
// than hardcoded; hardcoding it (as this constant briefly was) is what put
// every non-Italian/French row's values visibly right of the real column.
// The 200px stride between columns does not shift, only the base does.
constexpr u32 kSottotitoliValue1Offset = 0x70Cu;  // "Si" text record, right after Sottotitoli's label
constexpr u32 kMaxRowValues = 3;

// Synthetic BTX ids, far above any real entry (the xex block defines 211) so
// they can never collide with a genuine lookup. Row r's label is
// kRowSidBase + kRowSidStride*r; its value i is one past that, +i.
constexpr u32 kRowSidBase = 900u;
constexpr u32 kRowSidStride = 10u;

// Row labels are authored text (there is no stock BTX analogue for
// "Resolution"/"Frame Rate"/"Fullscreen"), so unlike the boolean values above
// they cannot ride the game's own localisation for free - each language needs
// its own literal. Index matches LanguageIndex() below: en, de, fr, es, it.
//
// Accents are written as raw CP1252/Latin-1 byte escapes rather than UTF-8
// source characters: the stock EFIGS text in the game's own BTX blocks is
// single-byte, and writing multi-byte UTF-8 into a single-byte text record
// would render as two garbled glyphs instead of one accented one. Verify
// in-game per language - this is the one part of the row that cannot be
// cross-checked against a stock string the way the boolean values are.
struct LocalizedLabel {
  const char* text[5];
};

// Which language is active is derived from *which of the 5 known list
// addresses matched* (see kOptionsListByLang), not from dword_8243D370's own
// switch - that byte's numbering doesn't correspond to XLanguage kernel ids in
// any way that was confirmed reliable (verified Italian, guessed the other
// three, and two of those guesses were wrong - see 2026-08-06 in
// docs/debug-hooks.md). Address matching sidesteps the guess entirely: the
// list address is what sub_821F2F38 actually branches on, so matching against
// it directly is ground truth, not an inference from an unrelated byte.
int OptionsListIndex(u32 list_addr) {
  for (int i = 0; i < static_cast<int>(std::size(kOptionsListByLang)); ++i) {
    if (kOptionsListByLang[i] == list_addr) {
      return i;
    }
  }
  return -1;
}

constexpr LocalizedLabel kLabelResolution = {
    {"Resolution", "Aufl\xF6sung", "R\xE9solution", "Resoluci\xF3n", "Risoluzione"}};
constexpr LocalizedLabel kLabelFrameRate = {
    {"Frame Rate", "Bildrate", "Fr\xE9quence", "Fotogramas", "Framerate"}};

// One entry per value a row can hold. `literal` non-null means we author that
// exact text ourselves (used where there is no stock analogue, e.g. "60
// FPS"); `literal == nullptr` means reuse the stock BTX string `btx_id`
// (localised for free) - that is how the boolean rows get "Si"/"NO". Values
// here (FPS figures, resolution names) are conventionally left untranslated
// even in localised menus, so unlike labels they need no per-language table.
struct OptionValue {
  const char* literal;
  u32 btx_id;
};

struct OptionRow {
  const LocalizedLabel* label;
  const OptionValue* values;
  u8 value_count;
  int (*get_index)();                    // active value, 0-based
  void (*set_index)(u8* base, int idx);  // apply a newly selected value
  // Highlight bar width, thousandths of the stock bar (see kBarWidth). Wider
  // literal values (e.g. "1080p") need a wider bar than the stock "Si"/"NO"
  // width to actually cover the text.
  int32_t bar_width;
};

int FrameRateGetIndex();
void FrameRateSetIndex(u8* base, int idx);
int ResolutionGetIndex();
void ResolutionSetIndex(u8* base, int idx);

constexpr OptionValue kFrameRateValues[2] = {{"30 FPS", 0}, {"60 FPS", 0}};
constexpr const char* kFrameRateIds[2] = {"30", "60"};
constexpr OptionValue kResolutionValues[3] = {{"720p", 0}, {"1080p", 0}, {"1440p", 0}};
constexpr const char* kResolutionIds[3] = {"720p", "1080p", "1440p"};

constexpr int32_t kWideBarWidth = 900;         // fits "30 FPS"/"60 FPS"-length text
constexpr int32_t kResolutionBarWidth = 750;   // fits "1080p"/"1440p", narrower than kWideBarWidth

// Matches the ImGui overlay's Resolution row (settings.cpp): don't offer a
// preset wider than the user's actual display. Not constexpr since it
// queries the display, so kOptionRows below can't be constexpr either -- its
// value_count is fixed once at static-init time, same as the overlay's
// per-frame computation would settle on for a display that doesn't change
// resolution mid-session.
u8 ResolutionRowValueCount() {
  return static_cast<u8>(
      std::min<int>(static_cast<int>(std::size(kResolutionValues)),
                    eternalsonata::AllowedResolutionCount()));
}

const OptionRow kOptionRows[] = {
    {&kLabelResolution, kResolutionValues, ResolutionRowValueCount(), &ResolutionGetIndex,
     &ResolutionSetIndex, kResolutionBarWidth},
    {&kLabelFrameRate, kFrameRateValues, 2, &FrameRateGetIndex, &FrameRateSetIndex,
     kWideBarWidth},
};
const u32 kOptionRowCount = static_cast<u32>(std::size(kOptionRows));

// per-row bar registry id, all "unset" until the Options screen resolves them
u32 g_bar_id[kOptionRowCount] = {0xFFFFFFFFu, 0xFFFFFFFFu};
u32 g_label_addr[kOptionRowCount] = {};  // per-row label string
u32 g_value_addr[kOptionRowCount][kMaxRowValues] = {};  // per-row literal value strings
u32 g_bar_vec = 0;  // shared guest scratch for the {x, y, z} move argument

u32 g_fs_list = 0;   // guest address of the extended display list
bool g_fs_failed = false;
bool g_options_active = false;  // Options screen was the last one built
bool g_bar_dumped = false;      // bar objects already dumped for this entry

// Selection geometry, measured at runtime (see docs §14). The Options screen's
// bottom group is id 2, holding two sub-items at screen y 430 and 480; screen
// y is display y + 145, so row r sits at 530 + 50*r.
constexpr u32 kOptGroupId = 2;
constexpr u32 kOptGroupCount = 2;   // stock rows the group ships with
constexpr int32_t kOptRow0Y = 430;  // Sottotitoli, used to identify the group
constexpr int32_t kOptRow1Y = 480;  // Voce
constexpr u8 kSubtitleRowIndex = 0;  // Sottotitoli, the reference two-option row

// Pad state. sub_821281B8 polls an array of 4 pad objects at 0x824BB418,
// stride 464; sub_82128310 fills each one: +424 = buttons held this frame,
// +8 = previous, +428 = newly pressed this frame, +432 = pressed-or-repeat,
// +436 = released. We want the +428 edge so one press is one toggle.
constexpr u32 kPad0 = 0x824BB418u;
constexpr u32 kPadPressed = 428u;
constexpr u32 kBtnDPadLeft = 0x0004u;
constexpr u32 kBtnDPadRight = 0x0008u;
constexpr u32 kBtnA = 0x1000u;
// sub_82128310 also folds the left stick into synthetic bits; confirmed live:
// 0x10000/0x20000 = up/down, 0x40000/0x80000 = left/right. Matching only the
// d-pad meant stick input never reached the handler.
constexpr u32 kBtnStickLeft = 0x40000u;
constexpr u32 kBtnStickRight = 0x80000u;
constexpr u32 kLeftMask = kBtnDPadLeft | kBtnStickLeft;
constexpr u32 kRightMask = kBtnDPadRight | kBtnStickRight;

REX_IMPORT(__imp__sub_82179F78, g_move_object,
           void(double, double, double, double, u32, u32, u32, u32, u32, u32, u32, u32));

// sub_82178A88(&registry, id, &{x,y,z}, 0, 0, -1) - the instant placement the
// screen init uses, with no animation.
REX_IMPORT(__imp__sub_82178A88, g_set_object_pos, void(u32, u32, u32, u32, u32, u32));

// sub_821F6580(root, id) -> object. The id->object resolver sub_82200FE8 itself
// uses on these same ids.
REX_IMPORT(__imp__sub_821F6580, g_resolve_object, u32(u32, u32));

// Slides row `row`'s highlight bar onto value `value_index`. Called on
// Options entry (so each bar starts on the active value) and on every
// change. `move` picks the mechanism: the animated slide the row handler
// uses on a value change, or the instant set the screen init uses when
// Options opens.
void MoveOptionBar(u8* base, u32 row, int value_index, bool move) {
  if (row >= kOptionRowCount || g_bar_id[row] == 0xFFFFFFFFu || !g_bar_vec) {
    return;
  }
  // Mirror sub_82201620's own base-X selection. Its third case (language >= 7)
  // reads an uninitialised local, so it is deliberately not reproduced.
  const u32 lang = REX_LOAD_U32(kLanguage);
  const int32_t base_x = (lang >= 3 && lang < 5) ? 530 : 480;
  const int32_t x = base_x + value_index * kBarColumnStride;
  const int32_t handler_y =
      kRowY0 + kRowYStep * static_cast<int32_t>(row) + 130 + kBarShrinkFixup;
  const int32_t y = move ? handler_y : handler_y + 740;

  const auto put = [&](u32 off, float v) {
    u32 bits;
    std::memcpy(&bits, &v, sizeof(bits));
    REX_STORE_U32(g_bar_vec + off, bits);
  };
  put(0, static_cast<float>(x));
  put(4, static_cast<float>(y));
  put(8, 0.0f);

  if (move) {
    g_move_object(0.0, kBarMoveSpeed, kBarMoveSpeed, kBarMoveSpeed, kTextRegistry,
                  g_bar_id[row], g_bar_vec, 0, 0, 0, 0, kBarMoveMode);
  } else {
    g_set_object_pos(kTextRegistry, g_bar_id[row], g_bar_vec, 0, 0, 0xFFFFFFFFu);
  }
}

int FrameRateGetIndex() {
  const std::string cur = REXCVAR_GET(frame_rate);
  for (int i = 0; i < static_cast<int>(std::size(kFrameRateIds)); ++i) {
    if (cur == kFrameRateIds[i]) {
      return i;
    }
  }
  return 0;
}

void FrameRateSetIndex(u8* base, int idx) {
  eternalsonata::SetFrameRateSetting(kFrameRateIds[idx]);
  REXLOG_INFO("[options] frame_rate -> {}", kFrameRateIds[idx]);
}

int ResolutionGetIndex() {
  const auto* entry = rex::cvar::GetFlagInfo("resolution");
  const std::string cur = entry ? entry->getter() : std::string();
  for (int i = 0; i < static_cast<int>(std::size(kResolutionIds)); ++i) {
    if (cur == kResolutionIds[i]) {
      return i;
    }
  }
  return 0;
}

void ResolutionSetIndex(u8* base, int idx) {
  eternalsonata::SetResolutionSetting(kResolutionIds[idx]);
  REXLOG_INFO("[options] resolution -> {}", kResolutionIds[idx]);
}

void WriteGuestString(u8* base, u32 at, const char* s) {
  for (u32 i = 0;; ++i) {
    REX_STORE_U8(at + i, static_cast<u8>(s[i]));
    if (!s[i]) {
      return;
    }
  }
}

void WriteTextRecord(u8* base, u32 at, u32 id, int32_t x, int32_t y) {
  REX_STORE_U32(at + 0x00, kTextRecord);
  REX_STORE_U32(at + 0x04, id);
  REX_STORE_U32(at + 0x08, static_cast<u32>(x));
  REX_STORE_U32(at + 0x0C, static_cast<u32>(y));
  REX_STORE_U32(at + 0x10, 300);  // width, as on every other row
  REX_STORE_U32(at + 0x14, 48);   // height
  REX_STORE_U32(at + 0x18, 0);
  REX_STORE_U32(at + 0x1C, kTextRecordBytes);
  REX_STORE_U32(at + 0x20, 0xFFFFFFFFu);
  REX_STORE_U32(at + 0x24, 1);
}

// Clone the stock Subtitles bar record verbatim and move it to our row's y.
// Cloning rather than hand-writing keeps every field we have not identified
// (notably the width at +0xC) at whatever the game already uses.
void WriteBarRecord(u8* base, u32 at, u32 src_list, int32_t y, int32_t width) {
  std::memcpy(REX_RAW_ADDR(at), REX_RAW_ADDR(src_list + kBarRecordOffset),
              kBarRecordBytes);
  REX_STORE_U32(at + kBarBlockYOffset, static_cast<u32>(y));
  REX_STORE_U32(at + kBarBlockWOffset, static_cast<u32>(width));
  REX_STORE_U32(at + kBarBlockHOffset, static_cast<u32>(kBarHeight));
}

// Clone the stock row separator and move it to our row's y, same reasoning as
// WriteBarRecord: cloning keeps x and width at whatever the game already uses
// (120 / 950 in every language) instead of hardcoding them here.
void WriteSeparatorRecord(u8* base, u32 at, u32 src_list, int32_t y) {
  std::memcpy(REX_RAW_ADDR(at), REX_RAW_ADDR(src_list + kSepRecordOffset),
              kSepRecordBytes);
  REX_STORE_U32(at + kSepYOffset, static_cast<u32>(y));
}

int32_t RowBarDisplayY(u32 row) {
  return kRowY0 + kRowYStep * static_cast<int32_t>(row) + kBarShrinkFixup;
}

// Builds the extended list: every kOptionRows entry gets a label + N value
// text records at its own 50px-pitch row, then every row's bar record,
// appended after the stock icon record (see the ordering rule below).
//
// The contents are rewritten on every Options entry rather than built once.
// That costs nothing (it is a memcpy of under 0x900 bytes) and keeps each
// row's value text in step with its cvar, since the interpreter only reads
// the list at screen construction.
void EnsureOptionRows(u8* base, int lang_idx) {
  if (g_fs_failed) {
    return;
  }
  auto* mem = rex::system::kernel_memory();
  if (!mem) {
    g_fs_failed = true;
    return;
  }

  u32 bytes = kOptionsListBytes + kIconRecordBytes + static_cast<u32>(sizeof(u32));
  for (const OptionRow& row : kOptionRows) {
    bytes += kTextRecordBytes + row.value_count * kTextRecordBytes + kSepRecordBytes +
             kBarRecordBytes;
  }

  if (!g_fs_list) {
    g_fs_list = mem->SystemHeapAlloc(bytes, 0x20);
    if (!g_fs_list) {
      g_fs_failed = true;
      REXLOG_WARN("[options] native rows: guest allocation failed");
      return;
    }
    for (u32 r = 0; r < kOptionRowCount; ++r) {
      g_label_addr[r] = mem->SystemHeapAlloc(64, 0x20);
      if (!g_label_addr[r]) {
        g_fs_failed = true;
        REXLOG_WARN("[options] native rows: guest allocation failed");
        return;
      }
      for (u32 v = 0; v < kOptionRows[r].value_count; ++v) {
        const OptionValue& val = kOptionRows[r].values[v];
        if (!val.literal) {
          continue;
        }
        g_value_addr[r][v] = mem->SystemHeapAlloc(64, 0x20);
        if (!g_value_addr[r][v]) {
          g_fs_failed = true;
          REXLOG_WARN("[options] native rows: guest allocation failed");
          return;
        }
        WriteGuestString(base, g_value_addr[r][v], val.literal);
      }
    }
    g_bar_vec = mem->SystemHeapAlloc(16, 0x20);
  }
  const u32 list = g_fs_list;

  // The source Options list is per-language (see kOptionsListByLang) - same
  // byte layout at a different address - so every copy below reads from
  // whichever one is active right now rather than a single fixed address.
  const u32 src_list = kOptionsListByLang[lang_idx];
  const int32_t value_base_x =
      static_cast<int32_t>(REX_LOAD_U32(src_list + kSottotitoliValue1Offset + 8));
  // How far below a row's text its separator sits, taken from the stock
  // Sottotitoli group (text y=285, rule y=328) rather than hardcoded, so it
  // tracks the game if the row metrics differ per language.
  const int32_t stock_row_y =
      static_cast<int32_t>(REX_LOAD_U32(src_list + kSottotitoliValue1Offset + 0x0C));
  const int32_t stock_sep_y =
      static_cast<int32_t>(REX_LOAD_U32(src_list + kSepRecordOffset + kSepYOffset));
  const int32_t sep_dy = stock_sep_y - stock_row_y;

  // Labels are rewritten every entry (not just on first allocation) so a
  // language change while playing takes effect the next time Options opens,
  // matching the game's own text - and matching how the values below are
  // already rebuilt every entry to stay in step with their cvars.
  for (u32 r = 0; r < kOptionRowCount; ++r) {
    WriteGuestString(base, g_label_addr[r], kOptionRows[r].label->text[lang_idx]);
  }

  // Insert, do not append. Records past the last row set drawing state, so a
  // row appended at the very end inherits that trailing state and renders in
  // the wrong place (observed: label offset from the cursor, which sat
  // correctly at y=530). Splicing the records in immediately after the
  // Sottotitoli record keeps them in the same state as the real rows.
  std::memcpy(REX_RAW_ADDR(list), REX_RAW_ADDR(src_list), kInsertOffset);
  u32 at = list + kInsertOffset;

  // Mirror the Sottotitoli row's layout for every row: label at X=120 and
  // every value drawn side by side from the language's real value column,
  // 200px apart. Each row also gets the separator that belongs *above* it -
  // drawn where the preceding row's rule would sit, one row pitch up - so the
  // stock row we now follow gets separated from us and our bottom row is left
  // without a rule under it, matching how the stock sections end.
  for (u32 r = 0; r < kOptionRowCount; ++r) {
    const int32_t y = kRowY0 + kRowYStep * static_cast<int32_t>(r);
    WriteTextRecord(base, at, kRowSidBase + kRowSidStride * r, kRowXLabel, y);
    at += kTextRecordBytes;
    for (u32 v = 0; v < kOptionRows[r].value_count; ++v) {
      WriteTextRecord(base, at, kRowSidBase + kRowSidStride * r + 1 + v,
                       value_base_x + static_cast<int32_t>(v) * kBarColumnStride, y);
      at += kTextRecordBytes;
    }
    WriteSeparatorRecord(base, at, src_list, y - kRowYStep + sep_dy);
    at += kSepRecordBytes;
  }

  // The stock bar record sits at exactly kInsertOffset, and it is the *last*
  // object-creating record in the list - everything after it is the
  // 1500/1502 selection block. That matters: sub_82201620 indexes the id array
  // at screen+0x4C **positionally** (elements 2/3/4 and 11), so a bar inserted
  // anywhere earlier would renumber the stock rows and move their highlights.
  // Copy the stock record through first, then append ours, so each row's bar
  // takes the next free index and nothing shifts.
  std::memcpy(REX_RAW_ADDR(at), REX_RAW_ADDR(src_list + kInsertOffset),
              kIconRecordBytes);
  at += kIconRecordBytes;
  for (u32 r = 0; r < kOptionRowCount; ++r) {
    WriteBarRecord(base, at, src_list, RowBarDisplayY(r), kOptionRows[r].bar_width);
    at += kBarRecordBytes;
  }
  const u32 rest = kInsertOffset + kIconRecordBytes;
  std::memcpy(REX_RAW_ADDR(at), REX_RAW_ADDR(src_list + rest),
              kOptionsListBytes - rest);
  at += kOptionsListBytes - rest;
  REX_STORE_U32(at, kListTerminator);

  REXLOG_INFO("[options] {} native rows built (list=0x{:08X})", kOptionRowCount, list);
}

}  // namespace

// sub_8223B780(blob, string_id) -> char*: the BTX text lookup. Answer our
// synthetic ids ourselves and let every real id fall through untouched.
REX_EXTERN(__imp__sub_8223B780);

REX_HOOK_RAW(sub_8223B780) {
  const u32 sid = ctx.r4.u32;
  if (sid >= kRowSidBase) {
    const u32 rel = sid - kRowSidBase;
    const u32 row = rel / kRowSidStride;
    const u32 sub = rel % kRowSidStride;
    if (row < kOptionRowCount) {
      const OptionRow& def = kOptionRows[row];
      if (sub == 0) {
        if (g_label_addr[row]) {
          ctx.r3.u32 = g_label_addr[row];
          return;
        }
      } else if (sub - 1 < def.value_count) {
        const u32 v = sub - 1;
        const OptionValue& val = def.values[v];
        if (val.literal) {
          if (g_value_addr[row][v]) {
            ctx.r3.u32 = g_value_addr[row][v];
            return;
          }
        } else {
          // Reuse the game's own string so it reads correctly in whatever
          // language is active: rewrite r4 and let the stock lookup do the
          // work, keeping localisation free.
          //
          // NOTE: an earlier attempt marked the inactive option by prefixing
          // the game's "<g>" tag (seen in strings like "<g>You have no score
          // pieces."). That tag is NOT interpreted here - it rendered
          // literally, which is why the dimmed option appeared shifted three
          // characters right. Every option is therefore drawn plain;
          // indicating which one is active still needs the game's own
          // highlight mechanism (see docs §14, open work).
          ctx.r4.u32 = val.btx_id;
          __imp__sub_8223B780(ctx, base);
          return;
        }
      }
    }
  }
  __imp__sub_8223B780(ctx, base);
}

// sub_821F2F38(a1, list, ...): the display-list interpreter. Swap the Options
// list for our extended copy; every other screen is left alone.
REX_EXTERN(__imp__sub_821F2F38);

REX_HOOK_RAW(sub_821F2F38) {
  // The interpreter runs once per screen build, so the list it is handed is a
  // reliable "which screen is up" signal for the selection patch below.
  // Measured: this fires exactly once per Options entry, not per frame - so
  // the display list is a build step and nothing in it can animate. Anything
  // that moves while the screen is up (the cursor, the value highlight) is a
  // live object driven elsewhere.
  const int lang_idx = OptionsListIndex(ctx.r4.u32);
  g_options_active = (lang_idx >= 0);
  g_bar_dumped = false;  // re-dump the bar objects on each Options entry
  if (g_options_active) {
    EnsureOptionRows(base, lang_idx);
    if (g_fs_list) {
      ctx.r4.u32 = g_fs_list;
    }
  }
  __imp__sub_821F2F38(ctx, base);
}

// ---------------------------------------------------------------------------
// Value-highlight hunt: live memory differ
// ---------------------------------------------------------------------------
//
// The bar that marks a two-option row's active choice is still unidentified
// (docs/debug-hooks.md §14). Every static lead was ruled out, and the one
// targeted diff that was tried only covered 0x800 bytes of the 0x824D0440
// config struct - which does not even hold the Subtitles setting, or that diff
// would have caught it. So do it properly: snapshot *all* committed guest
// memory in state A, again in state B, and intersect across repeated toggles.
// Whatever survives is the state the highlight is driven by; from there a
// watchpoint on the survivor finds the code that reads it.
//
// Workflow (keys are polled here because this hook runs every menu frame):
//   F9  - capture state A   (e.g. Sottotitoli = Si)
//   F10 - capture state B   (e.g. Sottotitoli = NO)
//   F11 - report surviving candidates
//   F12 - reset the hunt
// Alternate F9/F10 across several toggles; the candidate set collapses fast.
//
// This is a debug tool, inert unless those keys are pressed.

namespace {

// Guest virtual memory, plus the 4 KiB view of the physical heap. The
// 0xA0000000 and 0xC0000000 views alias the same physical pages as 0xE0000000,
// so scanning them would only produce duplicate hits.
constexpr struct {
  u32 lo, hi;
} kScanRanges[] = {
    // TEMP (player-position hunt, see chat): narrowed from
    // {0x82000000, 0xA0000000}/{0xE0000000, 0xFB000000} to the small
    // known-globals cluster (party level 0x8243F3EC, scene mode 0x824C74C4,
    // area caches 0x8244Bxxx, console state 0x8244Cxxx/Dxxx, ConsoleSetting
    // 0x82565xxx) so a real area transition -- which touches nearly all of
    // the full range's 330 MiB -- doesn't blow the candidate set up to
    // millions of entries again. Revert to the wide range once this hunt is
    // done; it's still needed for other menu-scan uses.
    {0x8243F000u, 0x82566000u},
};
constexpr u32 kScanPage = 0x1000u;

struct ScanCandidate {
  u32 addr;
  u32 a;  // raw big-endian, as stored in guest memory
  u32 b;
};

std::vector<u32> g_scan_pages;      // committed page bases, collected once
std::vector<u32> g_scan_snap;       // state-A snapshot, kScanPage/4 dwords each
std::vector<ScanCandidate> g_scan_cands;
bool g_scan_have_a = false;
bool g_scan_have_cands = false;
size_t g_scan_last_size = 0;  // convergence tracking for the auto-report
int g_scan_stable = 0;
bool g_scan_reported = false;

// Collects the committed pages once. Uncommitted pages must be skipped: the
// guest address space is reserved as one big host range, so touching a page
// that was never committed faults.
void ScanCollectPages() {
  auto* mem = rex::system::kernel_memory();
  if (!mem) {
    return;
  }
  g_scan_pages.clear();
  for (const auto& r : kScanRanges) {
    for (u32 p = r.lo; p < r.hi; p += kScanPage) {
      auto* heap = mem->LookupHeap(p);
      if (!heap) {
        continue;
      }
      u32 protect = 0;
      if (!heap->QueryProtect(p, &protect) || protect == 0) {
        continue;
      }
      g_scan_pages.push_back(p);
    }
  }
  REXLOG_INFO("[scan] {} committed pages ({} MiB)", g_scan_pages.size(),
              (g_scan_pages.size() * kScanPage) >> 20);
}

// REX_RAW_ADDR, not `base + addr`: the physical views above 0xE0000000 carry a
// +0x1000 host offset on Windows.
inline u32 ScanRead(u8* base, u32 addr) {
  u32 v;
  std::memcpy(&v, REX_RAW_ADDR(addr), sizeof(v));
  return v;  // kept raw (big-endian); only swapped when reported
}

void ScanCaptureA(u8* base) {
  if (g_scan_pages.empty()) {
    ScanCollectPages();
    if (g_scan_pages.empty()) {
      return;
    }
  }
  if (g_scan_have_cands) {
    // Filter: a real candidate must return to its state-A value.
    const size_t before = g_scan_cands.size();
    std::erase_if(g_scan_cands, [&](const ScanCandidate& c) {
      return ScanRead(base, c.addr) != c.a;
    });
    REXLOG_INFO("[scan] A: {} -> {} candidates", before, g_scan_cands.size());
    return;
  }
  g_scan_snap.resize(g_scan_pages.size() * (kScanPage / sizeof(u32)));
  for (size_t i = 0; i < g_scan_pages.size(); ++i) {
    std::memcpy(&g_scan_snap[i * (kScanPage / sizeof(u32))],
                REX_RAW_ADDR(g_scan_pages[i]), kScanPage);
  }
  g_scan_have_a = true;
  REXLOG_INFO("[scan] baseline A captured");
}

void ScanCaptureB(u8* base) {
  if (!g_scan_have_a) {
    REXLOG_WARN("[scan] press F9 for a state-A baseline first");
    return;
  }
  if (g_scan_have_cands) {
    const size_t before = g_scan_cands.size();
    std::erase_if(g_scan_cands, [&](const ScanCandidate& c) {
      return ScanRead(base, c.addr) != c.b;
    });
    REXLOG_INFO("[scan] B: {} -> {} candidates", before, g_scan_cands.size());
    return;
  }
  // First B: everything that moved since the baseline becomes a candidate.
  constexpr size_t kDwords = kScanPage / sizeof(u32);
  for (size_t i = 0; i < g_scan_pages.size(); ++i) {
    const u32 page = g_scan_pages[i];
    const u32* snap = &g_scan_snap[i * kDwords];
    for (size_t j = 0; j < kDwords; ++j) {
      const u32 now = ScanRead(base, page + static_cast<u32>(j * 4));
      if (now != snap[j]) {
        g_scan_cands.push_back({page + static_cast<u32>(j * 4), snap[j], now});
      }
    }
  }
  g_scan_have_cands = true;
  g_scan_snap.clear();
  g_scan_snap.shrink_to_fit();
  REXLOG_INFO("[scan] {} initial candidates", g_scan_cands.size());
}

// Dumps every survivor to a file - the set converges to a few hundred, which is
// far too many for the log but trivial to grep offline. Values are shown as
// hex, signed int and float, because a menu coordinate could plausibly be any
// of the three.
void ScanReport(u8* base) {
  FILE* f = std::fopen("logs/scan_candidates.txt", "w");
  if (!f) {
    REXLOG_WARN("[scan] cannot open logs/scan_candidates.txt");
    return;
  }
  std::fprintf(f, "# addr        A(hex)     A(int)   A(float)      B(hex)     B(int)   B(float)\n");
  for (const auto& c : g_scan_cands) {
    const u32 a = __builtin_bswap32(c.a);
    const u32 b = __builtin_bswap32(c.b);
    float af, bf;
    std::memcpy(&af, &a, 4);
    std::memcpy(&bf, &b, 4);
    std::fprintf(f, "0x%08X  0x%08X %10d %12g   0x%08X %10d %12g\n", c.addr, a,
                 static_cast<int32_t>(a), af, b, static_cast<int32_t>(b), bf);
  }
  std::fclose(f);
  REXLOG_INFO("[scan] {} candidates written to logs/scan_candidates.txt",
              g_scan_cands.size());
}

void ScanReset() {
  g_scan_cands.clear();
  g_scan_snap.clear();
  g_scan_snap.shrink_to_fit();
  g_scan_have_a = false;
  g_scan_have_cands = false;
  g_scan_last_size = 0;
  g_scan_stable = 0;
  g_scan_reported = false;
  REXLOG_INFO("[scan] reset");
}

// Auto-capture, driven by the Subtitles row itself. A two-option row is
// idempotent per direction: left always selects the left option, right the
// right one. So "left pressed" is unambiguously state A and "right pressed" is
// state B, no matter what order they come in - the user just parks the cursor
// on Subtitles and alternates left/right, and the candidate set collapses on
// its own. Captures are deferred a few frames so the value (and the bar) have
// settled before the snapshot.
int g_scan_pending = 0;   // frames left before the deferred capture
bool g_scan_pending_a = false;

void ScanOnRowInput(u8* base, bool left) {
  g_scan_pending = 4;
  g_scan_pending_a = left;
}

}  // namespace

namespace eternalsonata_hooks {

void ScanTick(u8* base) {
  if (g_scan_pending && --g_scan_pending == 0) {
    if (g_scan_pending_a) {
      ScanCaptureA(base);
    } else {
      ScanCaptureB(base);
    }
    // Auto-report once the set stops shrinking. Waiting for some small
    // threshold is wrong - the true answer set has a floor (a few hundred
    // addresses genuinely alternate with the setting), so "converged" is the
    // real signal, not "small". Dumping again on every later capture would
    // also let a stray press overwrite a good dump with an empty one.
    if (g_scan_have_cands) {
      g_scan_stable = (g_scan_cands.size() == g_scan_last_size) ? g_scan_stable + 1 : 0;
      g_scan_last_size = g_scan_cands.size();
      if (g_scan_stable == 3 && !g_scan_reported && !g_scan_cands.empty()) {
        g_scan_reported = true;
        ScanReport(base);
      }
    }
  }
}

// Edge-detected hotkeys. GetAsyncKeyState is fine to call from the guest
// thread; nothing here runs unless a key is actually pressed.
void ScanPollKeys(u8* base) {
#ifdef _WIN32
  static const struct {
    int vk;
    void (*fn)(u8*);
  } kKeys[] = {
      {VK_F9, &ScanCaptureA},
      {VK_F10, &ScanCaptureB},
      {VK_F11, &ScanReport},
      {VK_F12, [](u8*) { ScanReset(); }},
  };
  static bool s_down[std::size(kKeys)] = {};
  for (size_t i = 0; i < std::size(kKeys); ++i) {
    const bool down = (GetAsyncKeyState(kKeys[i].vk) & 0x8000) != 0;
    if (down && !s_down[i]) {
      kKeys[i].fn(base);
    }
    s_down[i] = down;
  }
#else
  (void)base;
#endif
}

}  // namespace eternalsonata_hooks

// ---------------------------------------------------------------------------
// Highlight-bar dump
// ---------------------------------------------------------------------------
//
// sub_82201620 (the Options row input handler) reaches a row's highlight bar as
// `*(u32*)(root[709] + N)` with N = 84/88/92 for the page-1 rows, 120 for
// Subtitles and 92 for Voice - where `root` is dword_824400E4 and `[709]` is
// the same `4 * (page + 709)` slot sub_821F2F38 allocates the 1744-byte screen
// object into. Walk that chain and dump what is actually there: the object's
// vtable identifies its class, which is what a new bar would have to be built
// as. Runs once per Options entry, gated behind menu_scan.
namespace {

constexpr u32 kUiRoot = 0x824400E4u;
constexpr u32 kScreenSlotBase = 709u;
constexpr u32 kBarOffsets[] = {84, 88, 92, 120};

inline bool GuestPtr(u32 v) { return v >= 0x82000000u && v < 0xFB000000u; }

void DumpHighlightBars(u8* base) {
  const u32 root = REX_LOAD_U32(kUiRoot);
  if (!GuestPtr(root)) {
    return;
  }
  const u32 page = REX_LOAD_U8(root + 2833);
  const u32 screen = REX_LOAD_U32(root + 4 * (page + kScreenSlotBase));
  REXLOG_INFO("[bar] root=0x{:08X} page={} screen=0x{:08X}", root, page, screen);
  if (!GuestPtr(screen)) {
    return;
  }

  // The four "bar" offsets are not separate fields at all: 84/88/92/120 are
  // simply elements 2/3/4/11 of one array of registry ids at +0x4C, filled in
  // display-list order. Log the array and its count byte directly, so a
  // controlled A/B can be read straight out of the log.
  u32 n = 0;
  std::string ids;
  for (; n < 24; ++n) {
    const u32 id = REX_LOAD_U32(screen + 0x4C + 4 * n);
    if (id == 0xFFFFFFFFu) {
      break;
    }
    ids += fmt::format("{:02X} ", id);
  }
  REXLOG_INFO("[bar] count@0x17C={} ids@0x4C({})= {}",
              REX_LOAD_U8(screen + 0x17C), n, ids);

  // Resolve the stock Subtitles bar and row 0's (Resolution's), and dump
  // both. Comparing the two objects field by field is how the residual
  // vertical offset gets fixed exactly: whatever field differs by something
  // other than the 50px row pitch is the one the height scale is disturbing.
  const u32 stock_id = REX_LOAD_U32(screen + 120);
  for (int which = 0; which < 2; ++which) {
    const u32 id = which ? g_bar_id[0] : stock_id;
    if (id == 0xFFFFFFFFu) {
      continue;
    }
    const u32 obj = g_resolve_object(root, id);
    REXLOG_INFO("[bar] {} bar id=0x{:X} obj=0x{:08X}", which ? "ours" : "stock",
                id, obj);
    if (!GuestPtr(obj)) {
      continue;
    }
    for (u32 o = 0; o < 0x80; o += 0x10) {
      REXLOG_INFO("[bar]   +0x{:02X}: {:08X} {:08X} {:08X} {:08X}", o,
                  REX_LOAD_U32(obj + o), REX_LOAD_U32(obj + o + 4),
                  REX_LOAD_U32(obj + o + 8), REX_LOAD_U32(obj + o + 12));
    }
  }

  for (const u32 off : kBarOffsets) {
    const u32 obj = REX_LOAD_U32(screen + off);
    if (!GuestPtr(obj)) {
      REXLOG_INFO("[bar] screen+{:3} = 0x{:08X} (not an object)", off, obj);
      continue;
    }
    // +0 is the vtable on every object in this UI (sub_820E64F0 sets it), so
    // it is the class fingerprint - and it is a static address, which means it
    // can be looked up in IDA to find the constructor and thence the record
    // handler that builds one.
    const u32 vtable = REX_LOAD_U32(obj);
    REXLOG_INFO("[bar] screen+{:3} -> obj=0x{:08X} vtable=0x{:08X}", off, obj,
                vtable);
    for (u32 row = 0; row < 0x60; row += 0x10) {
      REXLOG_INFO("[bar]   +0x{:02X}: {:08X} {:08X} {:08X} {:08X}", row,
                  REX_LOAD_U32(obj + row), REX_LOAD_U32(obj + row + 4),
                  REX_LOAD_U32(obj + row + 8), REX_LOAD_U32(obj + row + 12));
    }
  }

  // Dump the whole 1744-byte screen object. The four known slots hold small
  // integers, not pointers - they are registry ids (sub_82179F78 forwards its
  // second argument straight to sub_821771F8 as an id), so the rest of the
  // struct is where any *other* per-row id lives. Diffing this file with and
  // without the probe bar record is what identifies our row's id.
  static int s_dump_index = 0;
  char path[128];
  std::snprintf(path, sizeof(path), "logs/screen_dump_%d.txt", s_dump_index++);
  FILE* f = std::fopen(path, "w");
  if (!f) {
    return;
  }
  std::fprintf(f, "# screen object 0x%08X (1744 bytes)\n", screen);
  for (u32 o = 0; o < 1744; o += 16) {
    std::fprintf(f, "+0x%04X  %08X %08X %08X %08X\n", o, REX_LOAD_U32(screen + o),
                 REX_LOAD_U32(screen + o + 4), REX_LOAD_U32(screen + o + 8),
                 REX_LOAD_U32(screen + o + 12));
  }
  std::fclose(f);
  REXLOG_INFO("[bar] screen object written to {}", path);
}

}  // namespace

// sub_821F62B8 is the per-frame cursor update for the menu. We piggyback on it
// to keep the native rows selectable (the screen resets the group's count on
// every rebuild) and to handle input while the cursor sits on one of them.
REX_EXTERN(__imp__sub_821F62B8);

REX_HOOK_RAW(sub_821F62B8) {
  __imp__sub_821F62B8(ctx, base);

  if (!g_options_active || !g_fs_list) {
    return;
  }
  if (!g_bar_dumped) {
    g_bar_dumped = true;
    // Bar records are appended in row order after every stock object-creating
    // record, so our ids are always the last kOptionRowCount entries of the
    // array at screen+0x4C, in row order.
    const u32 root = REX_LOAD_U32(kUiRoot);
    if (root >= 0x82000000u && root < 0xFB000000u) {
      const u32 page = REX_LOAD_U8(root + 2833);
      const u32 screen = REX_LOAD_U32(root + 4 * (page + kScreenSlotBase));
      if (screen >= 0x82000000u && screen < 0xFB000000u) {
        u32 ids[32];
        u32 n = 0;
        for (; n < std::size(ids); ++n) {
          const u32 id = REX_LOAD_U32(screen + 0x4C + 4 * n);
          if (id == 0xFFFFFFFFu) {
            break;
          }
          ids[n] = id;
        }
        if (n >= kOptionRowCount) {
          for (u32 r = 0; r < kOptionRowCount; ++r) {
            g_bar_id[r] = ids[n - kOptionRowCount + r];
          }
        }
        REXLOG_INFO("[options] bar ids: {:x} {:x} {:x}", g_bar_id[0], g_bar_id[1],
                    g_bar_id[2]);
      }
    }
    // Place every bar the way the screen init places the stock ones:
    // instantly, in init space. No settling delay - sub_82178A88 is not an
    // animation.
    for (u32 r = 0; r < kOptionRowCount; ++r) {
      MoveOptionBar(base, r, kOptionRows[r].get_index(), /*move=*/false);
    }
    if (REXCVAR_GET(menu_scan)) {
      DumpHighlightBars(base);
    }
  }
  const u32 menu = REX_LOAD_U32(0x824400E8u);
  if (menu < 0x82000000u || menu >= 0xFB000000u) {
    return;
  }

  // Find the Options screen's bottom group and give it kOptionRowCount more
  // rows. Each node is {id @ +0, subitem block @ +4, subitem pointer array @
  // +8, count byte @ +0x0C (mirrored at +0x0D), current index @ +0x2C, next @
  // +0x30}. The array is pre-allocated with 10 slots; unused ones are parked
  // at the sentinel (10000 + i, 10000 + i), so no allocation is needed - only
  // a position and a bigger count.
  //
  // The count check also makes this idempotent: after patching, count no
  // longer matches kOptGroupCount, so it stops matching until the screen is
  // rebuilt (which resets the count to kOptGroupCount).
  for (u32 i = REX_LOAD_U32(menu + 392);
       i >= 0x82000000u && i < 0xFB000000u; i = REX_LOAD_U32(i + 48)) {
    if (REX_LOAD_U32(i) != kOptGroupId ||
        REX_LOAD_U8(i + 0x0C) != kOptGroupCount) {
      continue;
    }
    const u32 arr = REX_LOAD_U32(i + 8);
    if (arr < 0x82000000u || arr >= 0xFB000000u) {
      continue;
    }
    const u32 s0 = REX_LOAD_U32(arr);
    const u32 s1 = REX_LOAD_U32(arr + 4);
    if (s0 < 0x82000000u || s0 >= 0xFB000000u || s1 < 0x82000000u ||
        s1 >= 0xFB000000u) {
      continue;
    }
    // Confirm this really is the Options bottom group before writing.
    if (static_cast<int32_t>(REX_LOAD_U32(s0 + 8)) != kOptRow0Y ||
        static_cast<int32_t>(REX_LOAD_U32(s1 + 8)) != kOptRow1Y) {
      continue;
    }

    u32 srow[kOptionRowCount];
    bool ok = true;
    for (u32 r = 0; r < kOptionRowCount; ++r) {
      const u32 s = REX_LOAD_U32(arr + 8 + 4 * r);
      if (s < 0x82000000u || s >= 0xFB000000u) {
        ok = false;
        break;
      }
      srow[r] = s;
    }
    if (!ok) {
      continue;
    }

    // The cursor column x is per-language (confirmed by diffing the raw
    // display lists 2026-08-06: Italian/French author 550, English 500), so
    // it has to be read from a stock row rather than hardcoded - a mismatch
    // here is what made Down-navigation skip straight over the new rows in
    // English/German/Spanish despite the count byte being patched correctly.
    const u32 opt_x = REX_LOAD_U32(s0 + 4);
    for (u32 r = 0; r < kOptionRowCount; ++r) {
      const int32_t y = kOptRow1Y + kRowYStep * static_cast<int32_t>(r + 1);
      REX_STORE_U32(srow[r] + 4, opt_x);
      REX_STORE_U32(srow[r] + 8, static_cast<u32>(y));
    }
    REX_STORE_U8(i + 0x0C, static_cast<u8>(kOptGroupCount + kOptionRowCount));
    REX_STORE_U8(i + 0x0D, static_cast<u8>(kOptGroupCount + kOptionRowCount));
    REXLOG_INFO("[options] {} native rows made selectable (node=0x{:08X})",
                kOptionRowCount, i);
    break;
  }

  // Handle input, but only while the cursor is actually parked on one of our
  // rows: group 2 selected and its sub-index at or past kOptGroupCount. The
  // game has no handler of its own for those indices, so nothing else
  // consumes the press.
  if (REX_LOAD_U32(menu + 396) != kOptGroupId) {
    return;
  }
  for (u32 i = REX_LOAD_U32(menu + 392);
       i >= 0x82000000u && i < 0xFB000000u; i = REX_LOAD_U32(i + 48)) {
    if (REX_LOAD_U32(i) != kOptGroupId) {
      continue;
    }
    const u8 row = REX_LOAD_U8(i + 0x2C);

    // The just-pressed mask stays set for a whole guest frame, so latch on our
    // own observation of the transition - this stays correct even if the cursor
    // update runs more than once per frame.
    static u32 s_prev_pressed = 0;
    const u32 pressed = REX_LOAD_U32(kPad0 + kPadPressed);
    const u32 fresh = pressed & ~s_prev_pressed;
    s_prev_pressed = pressed;

    // Subtitles (row 0 of this group) is the reference two-option row for the
    // highlight hunt - it is a stock row, so its value and highlight move
    // through the game's own code path.
    if (row == kSubtitleRowIndex && REXCVAR_GET(menu_scan)) {
      if (fresh & kLeftMask) {
        ScanOnRowInput(base, true);
      } else if (fresh & kRightMask) {
        ScanOnRowInput(base, false);
      }
    }

    if (row < kOptGroupCount || row - kOptGroupCount >= kOptionRowCount) {
      return;
    }
    const u32 opt_row = row - kOptGroupCount;
    const OptionRow& def = kOptionRows[opt_row];
    const int cur = def.get_index();
    int next = cur;

    // Match how a real two-option row behaves: left steps toward the first
    // value, right toward the last (clamped at the ends, not wrapping) - for
    // a boolean row that is exactly "left picks Si, right picks NO". A cycles
    // forward through every value, wrapping.
    if (fresh & kLeftMask) {
      if (cur > 0) {
        next = cur - 1;
      }
    } else if (fresh & kRightMask) {
      if (cur < static_cast<int>(def.value_count) - 1) {
        next = cur + 1;
      }
    } else if (fresh & kBtnA) {
      next = (cur + 1) % static_cast<int>(def.value_count);
    }

    if (next != cur) {
      def.set_index(base, next);
      MoveOptionBar(base, opt_row, next, /*move=*/true);
    }
    return;
  }
}
