#include "generated/eternalsonata_init.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include "eternalsonata_options_api.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <rex/cvar.h>

#include "eternalsonata_hooks_internal.h"
#include "settings.h"

// Cvars read by the row getters below. Defined (and persisted) in settings.cpp.
REXCVAR_DECLARE(std::string, frame_rate);
REXCVAR_DECLARE(bool, adaptive_framerate);
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
// Rows are registry-driven (Rows() below): each has a label, an ordered
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

// The button-configuration screen - the *second* page of Options, reached with
// RB - is built by the very same interpreter. sub_822028C8 (menu state 6) calls
// sub_821EC050(root, 20, 0), and sub_821EC050 is a 25-way jump table
// (word_82082468, base loc_821EC160) whose every arm picks a display list by
// language and hands it to sub_821F2F38. Screen 8 is Options, screen 20 is the
// button config; the arm for 20 is at 0x821ECB88 and the five addresses below
// are read straight out of it, in the same language order as
// kOptionsListByLang.
//
// This is what makes page 2 tractable at all: it is not a special
// programmatically-built screen, it is a display list exactly like page 1, so
// the splice that already works there works here unchanged. (sub_822028C8 does
// go on to *position* three objects itself, but only the three stock rows'
// value markers - ids 0/1/2 - which is why our records must never renumber
// them; see the ordering rule in EnsurePageRows.)
constexpr u32 kButtonsListByLang[5] = {
    0x8202FBF8u,  // en
    0x82072C68u,  // de
    0x82068EB8u,  // fr
    0x8206DD90u,  // es
    0x8205F108u,  // it
};
constexpr u32 kButtonsListBytes = 0x460u;

// Splice point: just past the whole Sottotitoli row group - its label
// (0x8205EF64), the "Si" and "NO" value records, and the type-600 underline -
// i.e. guest 0x8205EFEC. Sottotitoli is the closest analogue to what we are
// adding (a boolean row), so inserting here puts our records in exactly the
// drawing state the real rows are in. Appending at the end of the list instead
// made the label render offset from its own cursor position.
constexpr u32 kInsertOffset = 0x76Cu;

// Page 2's splice point: 0x408, immediately after the last record that draws
// anything and immediately before the selection block ({2101, 1500, group id,
// 1502 x3, 1501, 1503}). The record ending right there is the type-200 hint
// line at record y 490, so our records inherit a plain text drawing state -
// the same condition the page-1 splice point was chosen for.
//
// Page 2's own three rows sit at record y 25/75/125, and the hint line at 490,
// so rows appended from 175 down have room for five before they would collide.
constexpr u32 kButtonsInsertOffset = 0x408u;
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

// There is NO horizontal twin of kBarShrinkFixup, and that is a measured
// result rather than an oversight. The obvious model - sprite anchored at its
// left edge, so narrowing it pulls its centre left by (1 - scale) * width / 2,
// exactly as narrowing it vertically raises the centre - was tried and is
// wrong: applied to every row it threw the Text row far right and moved
// Resolution visibly off too, when Resolution had been correct without it. So
// the bar and its value share a left edge, whatever the bar's width scale, and
// the width scale does not move it.
//
// What remains is a per-row nudge, below, for the one row where the bar still
// does not sit where it should.

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

// Row layout: 50px pitch, matching the stock rows' own pitch on both pages.
constexpr int32_t kRowYStep = 50;
constexpr int32_t kRowXLabel = 120;   // same in every language (verified: label x doesn't shift)
// The value column's *base* x is per-language (Italian/French 490, English
// 440, ... - confirmed 2026-08-06 by diffing the stock Sottotitoli record
// across languages), so it is read at runtime from that very record rather
// than hardcoded; hardcoding it (as this constant briefly was) is what put
// every non-Italian/French row's values visibly right of the real column.
// The 200px stride between columns does not shift, only the base does.
constexpr u32 kSottotitoliValue1Offset = 0x70Cu;  // "Si" text record, right after Sottotitoli's label
// A row's synthetic BTX ids are laid out label-then-values inside one stride
// of kRowSidStride, so the stride is what caps values per row.
constexpr u32 kMaxRowValues = ETERNALSONATA_MAX_ROW_VALUES;

// Hard ceiling on rows *per page*, set by the game rather than by us: a
// selectable group's item array is pre-allocated with 10 slots (see the cursor
// hook near the end of this file), and each page's stock rows already occupy
// some of them. Registration past a page's share is refused.
constexpr u32 kSelectableSlots = 10;
constexpr u32 kMaxOptionRows = ETERNALSONATA_MAX_OPTION_ROWS;

// ---------------------------------------------------------------------------
// The two pages
// ---------------------------------------------------------------------------
//
// Options is two pages: the Options screen proper (menu state 3, LB/RB paired
// with) and the button-configuration screen (state 7). Both are display lists
// walked by sub_821F2F38, so one implementation drives both; everything that
// differs between them is a field below.
//
// Which page is on screen is read every frame from byte_8243F3C0, the menu
// state machine's own state byte (sub_821DCC08 dispatches on it). That is the
// only honest signal available: screens are pushed and popped without being
// rebuilt - LB from page 2 just pops the depth, nothing re-runs the
// interpreter - so a flag edge-triggered off the build hook goes stale the
// moment the player pages back and forth.
constexpr u32 kMenuState = 0x8243F3C0u;
constexpr u8 kStateOptionRows = 3;   // sub_82201620, page 1's row handler
constexpr u8 kStateOptionSlider = 11;  // sub_82201FB0, page 1's slider handler
constexpr u8 kStateButtons = 7;      // sub_82202CB0, page 2's handler

enum PageId { kPageOptions = 0, kPageButtons = 1, kPageCount = 2 };

struct PageLayout {
  // Display list, one address per language (same order as LanguageIndex).
  const u32* lists;
  u32 list_bytes;       // up to, excluding, the terminator
  u32 insert_offset;    // where our text/separator records are spliced in
  // Stock bytes copied between our text records and our bar records. Bars
  // create entries in the screen's id array and the game indexes that array
  // positionally, so every stock object-creating record must be emitted before
  // ours. On page 1 that means stepping over the icon record that sits at the
  // splice point; on page 2 the splice point is already past the last one.
  u32 bar_skip_bytes;
  u32 group_id;         // selectable group holding the page's rows
  u32 stock_rows;       // rows the group ships with
  int32_t stock_item_y0;  // first stock item, used to identify the group
  int32_t stock_item_yn;  // last stock item; our rows follow it at 50px pitch
  int32_t first_row_y;    // record y of our first row
};

constexpr PageLayout kPages[kPageCount] = {
    // Page 1: rows appended below Voce (record y 285, 335).
    {kOptionsListByLang, kOptionsListBytes, kInsertOffset, kIconRecordBytes, 2,
     2, 430, 480, 385},
    // Page 2: rows appended below the three button rows (record y 25/75/125,
    // selectable items at 165/215/265).
    {kButtonsListByLang, kButtonsListBytes, kButtonsInsertOffset, 0, 1, 3, 165,
     265, 175},
};

u32 PageMaxRows(int page) {
  return kSelectableSlots - kPages[page].stock_rows;
}

// Where a mod row lands unless it asks otherwise. Page 1 is reserved for the
// game's own settings and is expected to fill up with them, so mods start on
// page 2 and spill over only by asking.
constexpr int kModDefaultPage = kPageButtons;

// Synthetic BTX ids, far above any real entry (the xex block defines 211) so
// they can never collide with a genuine lookup. Row r's label is
// kRowSidBase + kRowSidStride*r; its value i is one past that, +i.
constexpr u32 kRowSidBase = 900u;
constexpr u32 kRowSidStride = 10u;
static_assert(kMaxRowValues < kRowSidStride,
              "a row's values must fit inside one synthetic-id stride");

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
constexpr int kLanguageCount = ETERNALSONATA_LANG_COUNT;

struct LocalizedLabel {
  const char* text[kLanguageCount];
};

// Which language is active is derived from *which of the 5 known list
// addresses matched* (see kOptionsListByLang), not from dword_8243D370's own
// switch - that byte's numbering doesn't correspond to XLanguage kernel ids in
// any way that was confirmed reliable (verified Italian, guessed the other
// three, and two of those guesses were wrong - see 2026-08-06 in
// docs/debug-hooks.md). Address matching sidesteps the guess entirely: the
// list address is what sub_821F2F38 actually branches on, so matching against
// it directly is ground truth, not an inference from an unrelated byte.
// Both pages' tables are in the same language order, so one lookup answers
// "which page is this list, and in which language" at once. Returns false for
// every other screen in the game, which is the overwhelming majority.
bool ClassifyList(u32 list_addr, int* page, int* lang_idx) {
  for (int p = 0; p < kPageCount; ++p) {
    for (int i = 0; i < kLanguageCount; ++i) {
      if (kPages[p].lists[i] == list_addr) {
        *page = p;
        *lang_idx = i;
        return true;
      }
    }
  }
  return false;
}

constexpr LocalizedLabel kLabelResolution = {
    {"Resolution", "Aufl\xF6sung", "R\xE9solution", "Resoluci\xF3n", "Risoluzione"}};
constexpr LocalizedLabel kLabelFrameRate = {
    {"Frame Rate", "Bildrate", "Fr\xE9quence", "Fotogramas", "Framerate"}};
// Kept short on purpose: a label shares its row with the value column at
// x=440, so it has roughly 320px - about 13 characters - before the two would
// touch. The stock labels ("Sottotitoli", "Aufl\xF6sung") sit inside the same
// budget.
constexpr LocalizedLabel kLabelText = {
    {"Text", "Text", "Texte", "Texto", "Testo"}};

// The Text row is the one row whose highlight does not land on its value on
// its own. Two corrections, both settled by eye (see OptionRow::bar_nudge_x):
//
//   * +10 constant, confirmed: the first code sits dead centre in its bar.
//     (An earlier +20 was visibly right of it.)
//   * -3 per step, bracketed rather than derived. With no step correction the
//     bar drifted right as the value index grew; with -6 it drifted left by
//     about as much, the last code nearly escaping its bar. The truth is
//     between them, so this is the midpoint.
//
// The drift itself means the bar's effective stride and the text's disagree by
// a couple of percent, even though both are the same ColumnStride value - so
// the record-to-runtime mapping is not quite the pure +40 offset the stock
// rows suggest. Worth understanding properly if a third many-valued row ever
// appears; until then only a row with five values makes it visible at all,
// which is why every other row keeps both terms at 0. A formula applied to all
// rows was tried and measured to be wrong.
constexpr int32_t kTextBarNudge = 10;
constexpr int32_t kTextBarNudgeStep = -3;

// One entry per value a row can hold. `literal` non-empty means we author that
// exact text ourselves (used where there is no stock analogue, e.g. "60
// FPS"); an empty literal means reuse the stock BTX string `btx_id`
// (localised for free) - that is how the boolean rows would get "Si"/"NO".
// Values here (FPS figures, resolution names) are conventionally left
// untranslated even in localised menus, so unlike labels they need no
// per-language table.
struct OptionValue {
  std::string literal;
  u32 btx_id = 0;
};

struct OptionRow {
  // Per-language label. Slot 0 (English) is the fallback: any slot left empty
  // is drawn using it, so a mod that only registers one label still renders in
  // every language.
  std::string label[kLanguageCount];
  std::vector<OptionValue> values;
  std::function<int()> get_index;                    // active value, 0-based
  std::function<void(u8*, int)> set_index;           // apply a newly selected value
  // Highlight bar width, thousandths of the stock bar. Wider literal values
  // (e.g. "1080p") need a wider bar than the stock "Si"/"NO" width to actually
  // cover the text. Derived from the longest value, see BarWidthForValues.
  int32_t bar_width = 0;
  // Horizontal correction for the highlight bar, in runtime pixels: the bar
  // for value i is nudged by bar_nudge_x + i * bar_nudge_step_x. Both are zero
  // for every row whose bar already lines up.
  //
  // Two terms because the Text row needed two: a constant part (its bar sat
  // slightly left of the first code) and a part that grew with the value index
  // (each further code drifted a little further right). A drift that grows per
  // step means the bar's stride and the text's stride disagree slightly, even
  // though both are computed from the same ColumnStride - so it is the
  // record-to-runtime mapping that is not the pure +40 offset the stock rows
  // suggest, and this is the empirical stand-in until that is understood.
  // Settled by eye against the real rows, like kBarShrinkFixup.
  int32_t bar_nudge_x = 0;
  int32_t bar_nudge_step_x = 0;
  // Which page the row is drawn on. kPageOptions is the game-settings page,
  // kPageButtons the graphics one - that split is the whole point of having
  // two pages. The built-in rows pick their page explicitly; a mod row
  // defaults to kModDefaultPage.
  int page = kPageOptions;
};

// The registry proper. Rows are appended in registration order and drawn top
// to bottom in that order. Never shrinks: a row index handed out by
// RegisterOptionRow stays valid for the process lifetime.
std::vector<OptionRow>& Rows();

u32 RowCount() { return static_cast<u32>(Rows().size()); }

// Bar width in thousandths, from the longest value string a row can show.
//
// This replaces two hand-tuned constants (750 for the "1080p"/"1440p"
// resolution row, 900 for "30 FPS"/"60 FPS"), both settled by eye against the
// real rows. They agree on exactly 150 per character - 750/5 and 900/6 - so
// this formula reproduces both of the known-good widths bit for bit while
// giving mod rows a sane default instead of a number they would have to guess.
// A fit through two points is a hypothesis, not a law: if a much longer or
// shorter value ever renders with a visibly wrong bar, this is what to revisit.
constexpr int32_t kBarWidthPerChar = 150;
constexpr int32_t kBarWidthMin = 450;

int32_t BarWidthForValues(const std::vector<OptionValue>& values) {
  size_t longest = 0;
  for (const OptionValue& v : values) {
    longest = std::max(longest, v.literal.size());
  }
  return std::max(kBarWidthMin,
                  static_cast<int32_t>(longest) * kBarWidthPerChar);
}

constexpr const char* kResolutionIds[3] = {"720p", "1080p", "1440p"};

// The Frame Rate row covers two cvars, not one. frame_rate picks the rate and
// adaptive_framerate decides what happens when the PC cannot sustain it, but
// the second is only meaningful at 60 - at 30 and unlocked there is nothing to
// step down to - so as a pair they have exactly four states worth offering.
// Folding them into one row is what makes that legible: "Adaptive" is 60 with
// the ladder on, "60 FPS" is 60 pinned.
//
// Keep the three arrays index-aligned. The getter maps the cvar pair back to
// an index through the *Rate/*Adaptive arrays, and the row draws *Values.
// Ordered by how far each state lets the rate climb: 30, 60 pinned, 60 with
// the ladder, then uncapped. That also keeps the two 60-based states adjacent,
// which is what they are - the same rate, differing only in what happens when
// the PC cannot hold it.
constexpr const char* kFrameRateIds[4] = {"30", "60", "60", "unlocked"};
constexpr bool kFrameRateAdaptive[4] = {false, false, true, false};
constexpr const char* kFrameRateValues[4] = {"30 FPS", "60 FPS", "Adaptive",
                                             "Unlocked"};
static_assert(std::size(kFrameRateIds) == std::size(kFrameRateValues));
static_assert(std::size(kFrameRateIds) == std::size(kFrameRateAdaptive));

int FrameRateGetIndex();
void FrameRateSetIndex(u8* base, int idx);
int ResolutionGetIndex();
void ResolutionSetIndex(u8* base, int idx);
int TextGetIndex();
void TextSetIndex(u8* base, int idx);

// Matches the ImGui overlay's Resolution row (settings.cpp): don't offer a
// preset wider than the user's actual display. Resolved once, when the
// registry is first built, same as the overlay's per-frame computation would
// settle on for a display that doesn't change resolution mid-session.
int ResolutionRowValueCount() {
  return std::min<int>(static_cast<int>(std::size(kResolutionIds)),
                       eternalsonata::AllowedResolutionCount());
}

// Fills `row` in from a label table and a list of literal values, the shape
// both built-in rows have. Mod rows go through the same path in
// RegisterOptionRow below, which is the point of the refactor: there is one
// row implementation, and the built-ins are just its first two clients.
void MakeLiteralRow(OptionRow& row, const LocalizedLabel& label,
                    const char* const* values, int value_count) {
  for (int i = 0; i < kLanguageCount; ++i) {
    row.label[i] = label.text[i] ? label.text[i] : "";
  }
  row.values.clear();
  for (int i = 0; i < value_count; ++i) {
    row.values.push_back(OptionValue{values[i], 0});
  }
  row.bar_width = BarWidthForValues(row.values);
}

// Built-in rows are registered lazily rather than in a static initialiser:
// ResolutionRowValueCount queries the display and the getters read cvars, and
// neither is safe to touch before the app has finished starting. Lazy
// registration also fixes the ordering against mods for free - Rows() is first
// reached from the Options screen build, long after every mod DLL has had its
// OnModuleLaunched() call, so the built-ins are always rows 0 and 1 and mod
// rows follow in load order.
std::vector<OptionRow>& Rows() {
  static std::vector<OptionRow> rows = [] {
    std::vector<OptionRow> initial;
    // Reserved to the hard cap up front, and never allowed past it, so the
    // vector can never reallocate. That is what makes a mod registering a row
    // while the guest thread is walking the registry safe: appends only ever
    // touch the tail, and references the guest side already holds stay valid.
    initial.reserve(kMaxOptionRows * kPageCount);
    initial.resize(3);

    // Page 2, the graphics page, in the order they are drawn.
    MakeLiteralRow(initial[0], kLabelResolution, kResolutionIds,
                   ResolutionRowValueCount());
    initial[0].get_index = &ResolutionGetIndex;
    initial[0].set_index = &ResolutionSetIndex;
    initial[0].page = kPageButtons;
    MakeLiteralRow(initial[1], kLabelFrameRate, kFrameRateValues,
                   static_cast<int>(std::size(kFrameRateValues)));
    initial[1].get_index = &FrameRateGetIndex;
    initial[1].set_index = &FrameRateSetIndex;
    initial[1].page = kPageButtons;

    // Page 1, the game page, below the stock Subtitles and Voice rows.
    MakeLiteralRow(initial[2], kLabelText, nullptr, 0);
    for (int i = 0; i < eternalsonata::UserLanguageCount(); ++i) {
      initial[2].values.push_back(
          OptionValue{eternalsonata::UserLanguageCode(i), 0});
    }
    initial[2].bar_width = BarWidthForValues(initial[2].values);
    initial[2].bar_nudge_x = kTextBarNudge;
    initial[2].bar_nudge_step_x = kTextBarNudgeStep;
    initial[2].get_index = &TextGetIndex;
    initial[2].set_index = &TextSetIndex;
    initial[2].page = kPageOptions;
    return initial;
  }();
  return rows;
}

// Per-row guest-side state, keyed by *global* row index (a row keeps its index
// for the process lifetime, whichever page it is on), sized when a list is
// built.
std::vector<u32> g_label_addr;  // per-row label string
std::vector<std::vector<u32>> g_value_addr;  // per-row literal value strings
u32 g_strings_rows = 0;  // rows the two arrays above have been filled for
u32 g_bar_vec = 0;  // shared guest scratch for the {x, y, z} move argument

// Everything that is per-page. The two pages are independent screens with
// independent object arrays, so nothing here can be shared between them.
struct PageState {
  u32 list = 0;         // guest address of the extended display list
  u32 list_bytes = 0;   // bytes it was allocated for; it is grown, never shrunk
  u32 built_rows = 0;   // rows on this page the current buffer was built for
  bool failed = false;
  // Registry ids of our highlight bars, one per row *on this page*, in page
  // order; 0xFFFFFFFF until the first frame after the screen is built.
  std::vector<u32> bar_id;
  bool bars_resolved = false;
  // The language's value column, read out of the display list at build time
  // (it shifts per language) and reused when placing bars.
  int32_t value_base_x = 0;
  // Right edge available to a row's values, read from the row rule at build
  // time (x + width, 1070 in every language) rather than assumed.
  int32_t row_right_edge = 0;
  // Global row indices drawn on this page, top to bottom.
  std::vector<u32> rows;
};
PageState g_page[kPageCount];

// Which Options page is on screen right now, or -1 for anything else. Read
// from the menu state machine's own state byte every frame rather than latched
// when a screen is built: paging with LB/RB pushes and pops screens without
// rebuilding them, so a latched flag would survive the page it described.
int ActivePage(u8* base) {
  switch (REX_LOAD_U8(kMenuState)) {
    case kStateOptionRows:
    case kStateOptionSlider:
      return kPageOptions;
    case kStateButtons:
      return kPageButtons;
    default:
      return -1;
  }
}

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

// Record space vs runtime space. A display-list record's x/y and the
// coordinates the game passes to sub_82178A88 / sub_82179F78 are two different
// spaces, and the offset between them is constant on both pages:
//
//   runtime x = record x + 40    (page 1 record 490 -> 530; page 2 record 670
//                                 -> the 710 origin sub_822028C8 uses)
//   runtime y = record y + 130   (page 1 record 285 -> 415; page 2 records
//                                 25/75/125 -> 155/205/255)
//
// The x relation is what replaced a hardcoded per-language 480/530 split: the
// value column is read out of the list, so it is right in every language by
// construction rather than by enumeration.
constexpr int32_t kRecordToRuntimeX = 40;
constexpr int32_t kRecordToRuntimeY = 130;

// Record y of row `row` (page-local index) on `page`.
int32_t RowRecordY(int page, u32 row) {
  return kPages[page].first_row_y + kRowYStep * static_cast<int32_t>(row);
}

// ---------------------------------------------------------------------------
// Where a row's values sit
// ---------------------------------------------------------------------------
//
// Evenly spaced columns are what the stock two-option rows use, and they are
// right until a row's values differ a lot in length: on the Frame Rate row an
// even stride left "30 FPS" and "60 FPS" swimming in space while "Unlocked"
// and "Adaptive" ran into each other. So spacing follows content - each value
// gets the room it needs, and whatever is left over is shared out as equal
// gaps.
//
// Even spacing is still used whenever it fits, so the rows that were settled
// by eye keep the exact positions they were settled at: a row falls back to
// content-aware placement only when its widest value would not fit an even
// column. Resolution (widest 110px into a 200px column) and Text (44px into
// 126px) are unaffected; Frame Rate (165px into 157px) is the row that needs
// it.
//
// The font is proportional and its metrics are not readable from here, so a
// value's width is estimated: most glyphs are about 22px in record space, and
// a handful of obviously thin ones about half that. That is coarse, but it
// only has to be good enough to rank "Unlocked" above "60 FPS", and it is only
// consulted for rows even spacing cannot serve.
constexpr int32_t kValueCharPx = 22;
constexpr int32_t kValueNarrowCharPx = 11;
constexpr int32_t kValueGapMin = 10;
// A value that reuses a stock BTX string has no literal to measure. They are
// short words like "Si"/"NO", so assume a small width rather than skipping
// them and under-counting the row.
constexpr int32_t kValueBtxWidthPx = 2 * kValueCharPx;

bool IsNarrowGlyph(char c) {
  return c == ' ' || c == 'i' || c == 'l' || c == 'j' || c == 'I' ||
         c == '.' || c == ',' || c == ':' || c == ';' || c == '!' || c == '\'';
}

int32_t ValueWidth(const OptionValue& value) {
  if (value.literal.empty()) {
    return kValueBtxWidthPx;
  }
  int32_t w = 0;
  for (const char c : value.literal) {
    w += IsNarrowGlyph(c) ? kValueNarrowCharPx : kValueCharPx;
  }
  return w;
}

// Even column stride, capped so a many-valued row still ends inside the row
// rule: two or three values keep the full 200px, four get about 157px, five
// about 126px - which is why the Text row draws two-letter language codes
// rather than names.
int32_t ColumnStride(int page, size_t value_count) {
  const PageState& st = g_page[page];
  if (value_count < 2 || st.row_right_edge <= st.value_base_x) {
    return kBarColumnStride;
  }
  const int32_t span = st.row_right_edge - st.value_base_x;
  return std::min<int32_t>(kBarColumnStride,
                           span / static_cast<int32_t>(value_count));
}

// x of value `index`, as an offset from the row's value column. The one
// authority on the question: the display-list records and the highlight bar
// both go through it, so a bar can never disagree with the text it highlights.
int32_t ValueColumnOffset(int page, const std::vector<OptionValue>& values,
                          u32 index) {
  if (index == 0 || values.size() < 2) {
    return 0;
  }
  const PageState& st = g_page[page];
  const int32_t stride = ColumnStride(page, values.size());

  int32_t widest = 0;
  int32_t total = 0;
  for (const OptionValue& v : values) {
    const int32_t w = ValueWidth(v);
    widest = std::max(widest, w);
    total += w;
  }
  if (widest + kValueGapMin <= stride) {
    return stride * static_cast<int32_t>(index);
  }

  const int32_t span = st.row_right_edge - st.value_base_x;
  const int32_t gap =
      std::max(kValueGapMin,
               (span - total) / static_cast<int32_t>(values.size()));
  int32_t x = 0;
  for (u32 i = 0; i < index && i < values.size(); ++i) {
    x += ValueWidth(values[i]) + gap;
  }
  return x;
}

// Slides row `row` (page-local index) of `page`'s highlight bar onto
// `value_index`. Called on screen entry (so each bar starts on the active
// value) and on every change. `move` picks the mechanism: the animated slide
// the row handler uses on a value change, or the instant set the screen init
// uses when the page opens.
void MoveOptionBar(u8* base, int page, u32 row, int value_index, bool move) {
  PageState& st = g_page[page];
  if (row >= st.bar_id.size() || st.bar_id[row] == 0xFFFFFFFFu || !g_bar_vec) {
    return;
  }
  const OptionRow& def = Rows()[st.rows[row]];
  const int32_t x =
      st.value_base_x + kRecordToRuntimeX +
      ValueColumnOffset(page, def.values, static_cast<u32>(value_index)) +
      def.bar_nudge_x + value_index * def.bar_nudge_step_x;
  // One y for both mechanisms. An earlier version biased the *instant*
  // placement by 740px, on the strength of sub_82200FE8 placing page 1's stock
  // bars at 895/945 where its handler animates them to 155/205. That bias is
  // wrong here: it put the Text row's bar off the bottom of the screen, where
  // it stayed until the first value change slid it up into the right place.
  // The two spaces sub_82200FE8 appears to use are a property of when it runs
  // relative to the screen's own setup, not of sub_82178A88 - and our
  // placement runs from the per-frame cursor hook, well after all of that.
  // Page 2's rows have always been placed unbiased, and land correctly.
  const int32_t y =
      RowRecordY(page, row) + kRecordToRuntimeY + kBarShrinkFixup;

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
                  st.bar_id[row], g_bar_vec, 0, 0, 0, 0, kBarMoveMode);
  } else {
    g_set_object_pos(kTextRegistry, st.bar_id[row], g_bar_vec, 0, 0, 0xFFFFFFFFu);
  }
}

int FrameRateGetIndex() {
  const std::string cur = REXCVAR_GET(frame_rate);
  const bool adaptive = REXCVAR_GET(adaptive_framerate);
  for (int i = 0; i < static_cast<int>(std::size(kFrameRateIds)); ++i) {
    if (cur == kFrameRateIds[i] && adaptive == kFrameRateAdaptive[i]) {
      return i;
    }
  }
  // The rate matched no pair - "stock", or a rate whose adaptive flag is set
  // where it means nothing. Fall back on the rate alone rather than reporting
  // the first row: the flag is the part that does not matter here.
  for (int i = 0; i < static_cast<int>(std::size(kFrameRateIds)); ++i) {
    if (cur == kFrameRateIds[i]) {
      return i;
    }
  }
  return 0;
}

void FrameRateSetIndex(u8* base, int idx) {
  // Both cvars, every time. Picking a pinned rate has to clear the ladder or
  // the row would not round-trip: adaptive_framerate defaults to true, so
  // "60 FPS" would read back as "Adaptive" on the next entry.
  eternalsonata::SetFrameRateSetting(kFrameRateIds[idx]);
  eternalsonata::SetAdaptiveFramerateSetting(kFrameRateAdaptive[idx]);
  REXLOG_INFO("[options] frame_rate -> {} (adaptive {})", kFrameRateIds[idx],
              kFrameRateAdaptive[idx]);
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

// Text language. The guest reads its language once at boot, so this only
// changes what the menus say after a restart - the same as the overlay's own
// Language row, and the reason SetUserLanguageSetting marks a pending restart.
int TextGetIndex() { return eternalsonata::UserLanguageIndex(); }

void TextSetIndex(u8* base, int idx) {
  eternalsonata::SetUserLanguageSetting(idx);
  REXLOG_INFO("[options] user_language -> {}",
              eternalsonata::UserLanguageCode(idx));
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

int32_t RowBarDisplayY(int page, u32 row) {
  return RowRecordY(page, row) + kBarShrinkFixup;
}

// Global row indices drawn on `page`, in registration order.
std::vector<u32> RowsOnPage(int page) {
  std::vector<u32> out;
  const std::vector<OptionRow>& rows = Rows();
  for (u32 r = 0; r < rows.size(); ++r) {
    if (rows[r].page == page) {
      out.push_back(r);
    }
  }
  return out;
}

// Allocates the guest-side label and value strings for every row registered so
// far. Page-independent: a row keeps its strings whichever page it is on, and
// rows that already have theirs are left alone.
template <typename Mem>
bool EnsureRowStrings(u8* base, Mem* mem) {
  const std::vector<OptionRow>& rows = Rows();
  const u32 row_count = static_cast<u32>(rows.size());
  if (row_count <= g_strings_rows) {
    return true;
  }
  g_label_addr.resize(row_count, 0);
  g_value_addr.resize(row_count);
  for (u32 r = g_strings_rows; r < row_count; ++r) {
    g_label_addr[r] = mem->SystemHeapAlloc(64, 0x20);
    if (!g_label_addr[r]) {
      REXLOG_WARN("[options] native rows: guest allocation failed");
      return false;
    }
    g_value_addr[r].assign(rows[r].values.size(), 0);
    for (u32 v = 0; v < rows[r].values.size(); ++v) {
      const OptionValue& val = rows[r].values[v];
      if (val.literal.empty()) {
        continue;
      }
      g_value_addr[r][v] = mem->SystemHeapAlloc(64, 0x20);
      if (!g_value_addr[r][v]) {
        REXLOG_WARN("[options] native rows: guest allocation failed");
        return false;
      }
      WriteGuestString(base, g_value_addr[r][v], val.literal.c_str());
    }
  }
  g_strings_rows = row_count;
  return true;
}

// Builds `page`'s extended list: every row registered onto it gets a label + N
// value text records at its own 50px-pitch row, then every row's bar record,
// appended past the last stock object-creating record (see the ordering rule
// below).
//
// The contents are rewritten on every entry rather than built once. That costs
// nothing (it is a memcpy of under 0x900 bytes) and keeps each row's value text
// in step with its cvar, since the interpreter only reads the list at screen
// construction. Rewriting every entry is also what lets a row registered by a
// mod after the first entry still appear: the buffer grows below, and the next
// build picks the new row up.
//
// Both pages are built by this one function. What differs is entirely in
// kPages[page]: which list to copy, where to splice, and where the rows start.
// The *record templates* - the bar, the separator, the value column x - are
// read from the Options list on both pages, because page 2 has no rows of this
// shape to clone from; that is safe because the templates are static xex data,
// readable no matter which screen is being built.
void EnsurePageRows(u8* base, int page, int lang_idx) {
  PageState& st = g_page[page];
  if (st.failed) {
    return;
  }
  auto* mem = rex::system::kernel_memory();
  if (!mem) {
    st.failed = true;
    return;
  }
  if (!EnsureRowStrings(base, mem)) {
    st.failed = true;
    return;
  }

  const PageLayout& pl = kPages[page];
  const std::vector<OptionRow>& all = Rows();
  st.rows = RowsOnPage(page);
  const u32 row_count = static_cast<u32>(st.rows.size());

  u32 bytes = pl.list_bytes + static_cast<u32>(sizeof(u32));
  for (const u32 r : st.rows) {
    bytes += kTextRecordBytes +
             static_cast<u32>(all[r].values.size()) * kTextRecordBytes +
             kSepRecordBytes + kBarRecordBytes;
  }

  // Allocate on the first build, and again if a late registration made the
  // list outgrow what we have.
  if (!st.list || bytes > st.list_bytes) {
    st.list = mem->SystemHeapAlloc(bytes, 0x20);
    if (!st.list) {
      st.failed = true;
      REXLOG_WARN("[options] native rows: guest allocation failed");
      return;
    }
    st.list_bytes = bytes;
  }
  if (row_count != st.built_rows) {
    st.bar_id.assign(row_count, 0xFFFFFFFFu);
    st.built_rows = row_count;
  }
  st.bars_resolved = false;
  if (!g_bar_vec) {
    g_bar_vec = mem->SystemHeapAlloc(16, 0x20);
  }
  const u32 list = st.list;

  // Each page's list is per-language (see kOptionsListByLang) - same byte
  // layout at a different address - so every copy below reads from whichever
  // one is active right now rather than a single fixed address.
  const u32 src_list = pl.lists[lang_idx];
  const u32 tpl_list = kOptionsListByLang[lang_idx];
  st.value_base_x =
      static_cast<int32_t>(REX_LOAD_U32(tpl_list + kSottotitoliValue1Offset + 8));
  // How far below a row's text its separator sits, taken from the stock
  // Sottotitoli group (text y=285, rule y=328) rather than hardcoded, so it
  // tracks the game if the row metrics differ per language.
  const int32_t stock_row_y =
      static_cast<int32_t>(REX_LOAD_U32(tpl_list + kSottotitoliValue1Offset + 0x0C));
  const int32_t stock_sep_y =
      static_cast<int32_t>(REX_LOAD_U32(tpl_list + kSepRecordOffset + kSepYOffset));
  const int32_t sep_dy = stock_sep_y - stock_row_y;
  // How far right a row's values may run, which is what ColumnStride squeezes
  // many-valued rows into: the far end of the row rule ({600, x, y, width},
  // 120 + 950 = 1070 in every language).
  //
  // Allowing content one column past the rule was tried, on the grounds that
  // page 2 authors its own last value column at record x 970 with a 300px box.
  // It is wrong: at that edge the four-value Frame Rate row keeps the full
  // 200px spacing and its "Adaptive" runs off the screen. The rule is the real
  // boundary.
  st.row_right_edge =
      static_cast<int32_t>(REX_LOAD_U32(tpl_list + kSepRecordOffset + 0x04)) +
      static_cast<int32_t>(REX_LOAD_U32(tpl_list + kSepRecordOffset + 0x0C));

  // Labels are rewritten every entry (not just on first allocation) so a
  // language change while playing takes effect the next time Options opens,
  // matching the game's own text - and matching how the values below are
  // already rebuilt every entry to stay in step with their cvars.
  for (const u32 r : st.rows) {
    // Slot 0 is the fallback for any language a row did not translate, so a
    // mod that registers a single label still renders everywhere.
    const std::string& l = all[r].label[lang_idx].empty() ? all[r].label[0]
                                                          : all[r].label[lang_idx];
    WriteGuestString(base, g_label_addr[r], l.c_str());
  }

  // Insert, do not append. Records past the last row set drawing state, so a
  // row appended at the very end of the list inherits that trailing state and
  // renders in the wrong place (observed on page 1: label offset from the
  // cursor, which sat correctly at y=530). Both splice points sit right after
  // a text record, which is the state the real rows draw in.
  std::memcpy(REX_RAW_ADDR(list), REX_RAW_ADDR(src_list), pl.insert_offset);
  u32 at = list + pl.insert_offset;

  // Mirror the Sottotitoli row's layout for every row: label at X=120 and
  // every value drawn side by side from the language's real value column,
  // 200px apart. Each row also gets the separator that belongs *above* it -
  // drawn where the preceding row's rule would sit, one row pitch up - so the
  // stock row we now follow gets separated from us and our bottom row is left
  // without a rule under it, matching how the stock sections end.
  for (u32 i = 0; i < row_count; ++i) {
    const OptionRow& row = all[st.rows[i]];
    const u32 sid = kRowSidBase + kRowSidStride * st.rows[i];
    const int32_t y = RowRecordY(page, i);
    WriteTextRecord(base, at, sid, kRowXLabel, y);
    at += kTextRecordBytes;
    for (u32 v = 0; v < row.values.size(); ++v) {
      WriteTextRecord(base, at, sid + 1 + v,
                      st.value_base_x + ValueColumnOffset(page, row.values, v),
                      y);
      at += kTextRecordBytes;
    }
    WriteSeparatorRecord(base, at, tpl_list, y - kRowYStep + sep_dy);
    at += kSepRecordBytes;
  }

  // Bars last. Both handlers index their screen's id array **positionally** -
  // sub_82201620 reads elements 2/3/4 and 11 on page 1, sub_82202CB0 reads
  // elements 19/20/21 and 78/83/88 on page 2 - so a bar record emitted before
  // any stock object-creating record would renumber the stock rows and move
  // their highlights. Copy whatever stock records still sit at the splice
  // point through first (page 1's icon record; page 2 is already past the last
  // one), then append ours, so each row's bar takes the next free index and
  // nothing shifts.
  std::memcpy(REX_RAW_ADDR(at), REX_RAW_ADDR(src_list + pl.insert_offset),
              pl.bar_skip_bytes);
  at += pl.bar_skip_bytes;
  for (u32 i = 0; i < row_count; ++i) {
    WriteBarRecord(base, at, tpl_list, RowBarDisplayY(page, i),
                   all[st.rows[i]].bar_width);
    at += kBarRecordBytes;
  }
  const u32 rest = pl.insert_offset + pl.bar_skip_bytes;
  std::memcpy(REX_RAW_ADDR(at), REX_RAW_ADDR(src_list + rest),
              pl.list_bytes - rest);
  at += pl.list_bytes - rest;
  REX_STORE_U32(at, kListTerminator);

  REXLOG_INFO("[options] page {}: {} native rows built (list=0x{:08X})", page,
              row_count, list);
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
    if (row < RowCount()) {
      const OptionRow& def = Rows()[row];
      if (sub == 0) {
        if (row < g_label_addr.size() && g_label_addr[row]) {
          ctx.r3.u32 = g_label_addr[row];
          return;
        }
      } else if (sub - 1 < def.values.size()) {
        const u32 v = sub - 1;
        const OptionValue& val = def.values[v];
        if (!val.literal.empty()) {
          if (row < g_value_addr.size() && v < g_value_addr[row].size() &&
              g_value_addr[row][v]) {
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
  // The interpreter runs once per screen build - measured: once per entry, not
  // per frame, so the display list is a build step and nothing in it can
  // animate. Anything that moves while a screen is up (the cursor, the value
  // highlight) is a live object driven elsewhere.
  //
  // The list address identifies both the page and the language. Note this is
  // NOT used as a "which page is showing" signal: pages are pushed and popped
  // without rebuilding, so that question is answered per frame from the menu
  // state byte instead.
  int page = 0;
  int lang_idx = 0;
  if (ClassifyList(ctx.r4.u32, &page, &lang_idx)) {
    EnsurePageRows(base, page, lang_idx);
    if (g_page[page].list) {
      ctx.r4.u32 = g_page[page].list;
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

void DumpHighlightBars(u8* base, int page) {
  const u32 root = REX_LOAD_U32(kUiRoot);
  if (!GuestPtr(root)) {
    return;
  }
  const u32 depth = REX_LOAD_U8(root + 2833);
  const u32 screen = REX_LOAD_U32(root + 4 * (depth + kScreenSlotBase));
  REXLOG_INFO("[bar] root=0x{:08X} depth={} screen=0x{:08X}", root, depth,
              screen);
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
    const u32 id = which ? (g_page[page].bar_id.empty() ? 0xFFFFFFFFu
                                                        : g_page[page].bar_id[0])
                         : stock_id;
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

  const int page = ActivePage(base);
  if (page < 0) {
    return;
  }
  PageState& st = g_page[page];
  if (!st.list) {
    return;
  }
  const PageLayout& pl = kPages[page];
  const std::vector<OptionRow>& all = Rows();
  const u32 rows = static_cast<u32>(st.rows.size());

  if (!st.bars_resolved) {
    st.bars_resolved = true;
    // Bar records are appended in row order after every stock object-creating
    // record, so our ids are always the last `rows` entries of the id array at
    // screen+0x4C, in row order.
    const u32 root = REX_LOAD_U32(kUiRoot);
    if (root >= 0x82000000u && root < 0xFB000000u) {
      const u32 depth = REX_LOAD_U8(root + 2833);
      const u32 screen = REX_LOAD_U32(root + 4 * (depth + kScreenSlotBase));
      if (screen >= 0x82000000u && screen < 0xFB000000u) {
        // Page 2's screen holds far more objects than page 1's (its handler
        // indexes elements 78/83/88 for the row labels alone), so this has to
        // be sized for the bigger of the two, not for the Options screen.
        u32 ids[192];
        u32 n = 0;
        for (; n < std::size(ids); ++n) {
          const u32 id = REX_LOAD_U32(screen + 0x4C + 4 * n);
          if (id == 0xFFFFFFFFu) {
            break;
          }
          ids[n] = id;
        }
        if (n >= rows && rows <= st.bar_id.size()) {
          for (u32 r = 0; r < rows; ++r) {
            st.bar_id[r] = ids[n - rows + r];
          }
        }
        std::string ids_text;
        for (const u32 id : st.bar_id) {
          char buf[16];
          std::snprintf(buf, sizeof(buf), "%x ", id);
          ids_text += buf;
        }
        REXLOG_INFO("[options] page {}: {} objects, bar ids: {}", page, n,
                    ids_text);
      }
    }
    // Place every bar the way the screen init places the stock ones:
    // instantly. No settling delay - sub_82178A88 is not an animation.
    for (u32 r = 0; r < rows; ++r) {
      MoveOptionBar(base, page, r, all[st.rows[r]].get_index(), /*move=*/false);
    }
    if (REXCVAR_GET(menu_scan)) {
      DumpHighlightBars(base, page);
    }
  }
  const u32 menu = REX_LOAD_U32(0x824400E8u);
  if (menu < 0x82000000u || menu >= 0xFB000000u) {
    return;
  }

  // Find the page's row group and give it `rows` more items. Each node is
  // {id @ +0, subitem block @ +4, subitem pointer array @ +8, count byte @
  // +0x0C (mirrored at +0x0D), current index @ +0x2C, next @ +0x30}. The array
  // is pre-allocated with kSelectableSlots slots; unused ones are parked at the
  // sentinel (10000 + i, 10000 + i), so no allocation is needed - only a
  // position and a bigger count.
  //
  // Those slots, minus the page's stock rows, are the hard ceiling on how many
  // rows a page can hold - which is what PageMaxRows enforces at registration
  // time. Writing past the array would run into whatever follows it in guest
  // memory, so the count below is clamped rather than trusted.
  //
  // The count check also makes this idempotent: after patching, count no
  // longer matches stock_rows, so it stops matching until the screen is
  // rebuilt (which resets the count). Both pages' groups are walked from the
  // same list, so the item-y check is what tells them apart - the group ids
  // alone would not.
  for (u32 i = REX_LOAD_U32(menu + 392);
       i >= 0x82000000u && i < 0xFB000000u; i = REX_LOAD_U32(i + 48)) {
    if (REX_LOAD_U32(i) != pl.group_id ||
        REX_LOAD_U8(i + 0x0C) != pl.stock_rows) {
      continue;
    }
    const u32 arr = REX_LOAD_U32(i + 8);
    if (arr < 0x82000000u || arr >= 0xFB000000u) {
      continue;
    }
    const u32 s0 = REX_LOAD_U32(arr);
    const u32 sn = REX_LOAD_U32(arr + 4 * (pl.stock_rows - 1));
    if (s0 < 0x82000000u || s0 >= 0xFB000000u || sn < 0x82000000u ||
        sn >= 0xFB000000u) {
      continue;
    }
    // Confirm this really is the page's row group before writing.
    if (static_cast<int32_t>(REX_LOAD_U32(s0 + 8)) != pl.stock_item_y0 ||
        static_cast<int32_t>(REX_LOAD_U32(sn + 8)) != pl.stock_item_yn) {
      continue;
    }

    const u32 n = std::min<u32>(rows, PageMaxRows(page));
    u32 srow[kMaxOptionRows];
    bool ok = true;
    for (u32 r = 0; r < n; ++r) {
      const u32 s = REX_LOAD_U32(arr + 4 * (pl.stock_rows + r));
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
    for (u32 r = 0; r < n; ++r) {
      const int32_t y =
          pl.stock_item_yn + kRowYStep * static_cast<int32_t>(r + 1);
      REX_STORE_U32(srow[r] + 4, opt_x);
      REX_STORE_U32(srow[r] + 8, static_cast<u32>(y));
    }
    REX_STORE_U8(i + 0x0C, static_cast<u8>(pl.stock_rows + n));
    REX_STORE_U8(i + 0x0D, static_cast<u8>(pl.stock_rows + n));
    REXLOG_INFO("[options] page {}: {} native rows made selectable (node=0x{:08X})",
                page, n, i);
    break;
  }

  // Handle input, but only while the cursor is actually parked on one of our
  // rows: the page's group selected and its sub-index at or past the stock
  // rows. Neither handler has anything of its own for those indices -
  // sub_82201620 stops at 2 and sub_82202CB0 at 3 - so nothing else consumes
  // the press.
  if (REX_LOAD_U32(menu + 396) != pl.group_id) {
    return;
  }
  for (u32 i = REX_LOAD_U32(menu + 392);
       i >= 0x82000000u && i < 0xFB000000u; i = REX_LOAD_U32(i + 48)) {
    if (REX_LOAD_U32(i) != pl.group_id) {
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

    // Subtitles (row 0 of page 1's group) is the reference two-option row for
    // the highlight hunt - it is a stock row, so its value and highlight move
    // through the game's own code path.
    if (page == kPageOptions && row == kSubtitleRowIndex &&
        REXCVAR_GET(menu_scan)) {
      if (fresh & kLeftMask) {
        ScanOnRowInput(base, true);
      } else if (fresh & kRightMask) {
        ScanOnRowInput(base, false);
      }
    }

    if (row < pl.stock_rows || row - pl.stock_rows >= rows) {
      return;
    }
    const u32 opt_row = row - pl.stock_rows;
    const OptionRow& def = all[st.rows[opt_row]];
    const int value_count = static_cast<int>(def.values.size());
    const int cur = std::clamp(def.get_index(), 0, value_count - 1);
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
      if (cur < value_count - 1) {
        next = cur + 1;
      }
    } else if (fresh & kBtnA) {
      next = (cur + 1) % value_count;
    }

    if (next != cur) {
      def.set_index(base, next);
      MoveOptionBar(base, page, opt_row, next, /*move=*/true);
    }
    return;
  }
}

// ---------------------------------------------------------------------------
// Mod-facing registration API
// ---------------------------------------------------------------------------
//
// Exported for mod DLLs loaded into this same process, resolved with
// GetProcAddress(GetModuleHandle(nullptr), ...) - the same pattern
// EternalSonataIsBattleActive uses. Contract and usage: eternalsonata_options_api.h.
//
// The built-in Resolution and Frame Rate rows are not special-cased anywhere:
// they are registered through the same OptionRow shape mod rows land in (see
// Rows()), so anything that works for them works for a mod, and a bug in the
// row path shows up in the shipped rows rather than only in mod code.
//
// Threading: registration is expected from a mod's OnModuleLaunched(), which
// runs during startup, before the Options screen can be opened. The mutex
// below only covers two mods registering at once; safety against the guest
// thread reading the registry mid-append comes from Rows() reserving to
// kMaxOptionRows, so the vector never reallocates.

namespace {
std::mutex g_registry_mutex;
}  // namespace

extern "C" __declspec(dllexport) uint32_t EternalSonataOptionsAbiVersion(void) {
  return ETERNALSONATA_OPTIONS_ABI_VERSION;
}

extern "C" __declspec(dllexport) int EternalSonataRegisterOptionRow(
    const char* label, const char* const* values, int value_count,
    EternalSonataOptionGetFn get, EternalSonataOptionSetFn set, void* user) {
  if (!label || !*label || !values || !get || !set) {
    REXLOG_WARN("[options] mod row rejected: null argument");
    return -1;
  }
  if (value_count < 1 || value_count > static_cast<int>(kMaxRowValues)) {
    REXLOG_WARN("[options] mod row '{}' rejected: {} values, limit is {}", label,
                value_count, kMaxRowValues);
    return -1;
  }
  for (int i = 0; i < value_count; ++i) {
    if (!values[i]) {
      REXLOG_WARN("[options] mod row '{}' rejected: null value {}", label, i);
      return -1;
    }
  }

  std::lock_guard<std::mutex> lock(g_registry_mutex);
  std::vector<OptionRow>& rows = Rows();
  // Capacity is per page, and a mod row starts on kModDefaultPage; moving it
  // with EternalSonataSetOptionRowPage is checked against the page it moves to.
  if (RowsOnPage(kModDefaultPage).size() >= PageMaxRows(kModDefaultPage)) {
    REXLOG_WARN("[options] mod row '{}' rejected: page {} holds {} rows", label,
                kModDefaultPage + 1, PageMaxRows(kModDefaultPage));
    return -1;
  }

  rows.emplace_back();
  OptionRow& row = rows.back();
  // Slot 0 is the fallback every other language falls back to, so one label is
  // enough; EternalSonataSetOptionRowLabel fills in real translations.
  row.label[0] = label;
  row.values.reserve(static_cast<size_t>(value_count));
  for (int i = 0; i < value_count; ++i) {
    row.values.push_back(OptionValue{values[i], 0});
  }
  row.bar_width = BarWidthForValues(row.values);
  row.page = kModDefaultPage;
  row.get_index = [get, user] { return get(user); };
  row.set_index = [set, user](u8*, int idx) { set(idx, user); };

  const int index = static_cast<int>(rows.size()) - 1;
  REXLOG_INFO("[options] mod row '{}' registered as row {} ({} values)", label,
              index, value_count);
  return index;
}

extern "C" __declspec(dllexport) int EternalSonataSetOptionRowLabel(
    int row, int language, const char* label) {
  if (!label || language < 0 || language >= kLanguageCount) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(g_registry_mutex);
  std::vector<OptionRow>& rows = Rows();
  if (row < 0 || static_cast<size_t>(row) >= rows.size()) {
    return 0;
  }
  rows[static_cast<size_t>(row)].label[language] = label;
  return 1;
}

extern "C" __declspec(dllexport) int EternalSonataSetOptionRowPage(int row,
                                                                  int page) {
  if (page < 0 || page >= kPageCount) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(g_registry_mutex);
  std::vector<OptionRow>& rows = Rows();
  if (row < 0 || static_cast<size_t>(row) >= rows.size()) {
    return 0;
  }
  OptionRow& def = rows[static_cast<size_t>(row)];
  if (def.page == page) {
    return 1;
  }
  // The destination page has its own capacity, and the row is not on it yet,
  // so the check is against the page as it stands.
  if (RowsOnPage(page).size() >= PageMaxRows(page)) {
    REXLOG_WARN("[options] row {} not moved: page {} already holds {} rows", row,
                page, PageMaxRows(page));
    return 0;
  }
  def.page = page;
  // Both pages have to rebuild: the row leaves one list and joins the other,
  // and each list is only rewritten when its own screen is next built - which
  // is exactly what the row-count check in EnsurePageRows notices.
  return 1;
}

// ---------------------------------------------------------------------------
// Second Options page on the main menu
// ---------------------------------------------------------------------------
//
// There is only one Options screen; what differs between the two ways in is a
// single context argument. `sub_82200FE8(ctx, a2)` is the Options screen init
// (state 1 of the menu state machine at sub_821DCC08, dispatched on
// byte_8243F3C0 through the 12-entry table at 0x82082608). Its first two
// stores are:
//
//     dword_8243F358 = ctx;
//     dword_8243F360 = (ctx == 0);
//
// The in-game (pause / status screen) entry passes a non-zero ctx; the main
// menu passes 0. Everything about "one page vs two" falls out of those two
// globals:
//
//   * Page turn trigger. Both Options input handlers - sub_82201620 (state 3,
//     the row handler) at 0x82201D3C and sub_82201FB0 (state 11, the slider
//     handler) at 0x82202040 - end with the identical guard
//         if (!dword_8243F360 && (buttons & 0x200)) { byte_8243F3C0 = 6; ... }
//     0x200 is RB. State 6 is sub_822028C8, which builds screen 20 (the button
//     configuration page) - that *is* page 2. Its own handler, sub_82202CB0
//     (state 7), returns to page 1 on 0x100 (LB). With ctx == 0 the guard is
//     false in both handlers, so RB does nothing and page 2 is unreachable.
//
//   * Page turn prompt. sub_82200FE8 ends with a block gated on
//     `if (!dword_8243F358)` - i.e. the main-menu path only - that tears down
//     the three objects at screen slots 108/112/116 (sub_82179160 with a null
//     action) and fades six more (452/456/460/464/656/660) to alpha 0 via
//     sub_821D3768(reg, obj, 0) - that call's third argument is a colour whose
//     high byte becomes the alpha at object+480. Those nine objects are the
//     on-screen "RB - change page" hint, hidden precisely because the trigger
//     is off.
//
//   * Page 2 labels. sub_822028C8 uses dword_8243F358 again to choose between
//     the real per-command BTX names (ids resolved through the table at
//     dword_8243FC08) and the generic ids 138+. With a stale/empty command
//     table the lookup yields 0 and it falls back to the generic ids anyway,
//     so this branch degrades safely outside gameplay.
//
// So forcing ctx non-zero on the main-menu call opens the trigger, keeps the
// hint visible, and gives page 2 the same presentation it has in-game - no
// display list, asset, or state-machine change needed. Page 2's own screen is
// built fresh from the root+2840 slot table, independent of page 1's objects.
//
// The one piece of stock behaviour that is keyed off ctx == 0 and must be
// preserved is the first-call early out at the top of sub_82200FE8:
//
//     if (!ctx) { bool first = (byte_824409D6 == 0); byte_824409D6 = 1;
//                 if (first) return; }
//
// which delays the main-menu Options build by one frame the very first time it
// is opened. The hook replicates it verbatim before rewriting ctx, so that
// timing is unchanged.
static constexpr bool kUnlockMainMenuOptionsPage2 = true;

// Options screen init. r3 = context (0 from the main menu, non-zero in-game).
constexpr u32 kOptionsFirstBuildFlag = 0x824409D6u;

REX_EXTERN(__imp__sub_82200FE8);

REX_HOOK_RAW(sub_82200FE8) {
  if (kUnlockMainMenuOptionsPage2 && ctx.r3.u32 == 0) {
    // Stock one-frame delay on the first main-menu open, reproduced exactly.
    if (REX_LOAD_U8(kOptionsFirstBuildFlag) == 0) {
      REX_STORE_U8(kOptionsFirstBuildFlag, 1);
      return;
    }
    ctx.r3.u32 = 1;
  }
  __imp__sub_82200FE8(ctx, base);
}
