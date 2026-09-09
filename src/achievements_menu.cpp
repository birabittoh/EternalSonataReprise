// eternalsonata - the status menu's Achievements screen.
//
// option_strip.cpp puts an Achievements entry in the option strip carrying tag
// 11, the one tag in 1..12 whose byte_82082148 entry lands on sub_82236CD0's
// default arm and so opens nothing. This file gives it a screen by borrowing
// the Music gallery's, which is the closest thing the game already has to a
// list of named, individually locked collectibles.
//
// Nothing is cloned in the sense of new guest code. Screen id 0xA runs exactly
// as it always did; a mode flag changes what four things do while it is set:
//
//   * sub_82236CD0   tag 11 enters screen 0xA (the Music arm at loc_82236F08,
//                    replicated: push the screen history, store the id, state 0)
//   * sub_821F2890   per row build. Its track id argument is forced into the
//                    1..66 range sub_821FEBC8 range checks, because the row
//                    count now comes from the achievement catalogue and can run
//                    past what byte_8238E128 holds. The row's tab and 1-based
//                    number are what identify the achievement.
//   * sub_8223B780   the row title lookup, answered with the achievement's name
//                    (hooked in eternalsonata_options.cpp, which owns it)
//   * sub_822265C8 / sub_822273A0  fix up dword_82440128, the scroll limit,
//                    which both take from byte_822FF594 after the rows are laid
//                    out
//
// Playback is refused rather than remapped: the input handler reads
// byte_8238E128 directly, so the row the cursor is on is still a real track to
// it. sub_821F77F0 (start the stream) and sub_822278A0 (the now playing
// highlight) are both skipped while the flag is set, which is everything the
// A button does on this screen.
//
// See docs/music.md for the gallery itself.

#include "generated/eternalsonata_init.h"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/memory/utils.h>
#include <rex/system/achievement_manager.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/image_decode.h>

#include "achievements_menu.h"
#include "eternalsonata_asset_api.h"
#include "images.generated.h"
#include "option_strip.h"

// Defined in eternalsonata_asset_system.cpp as part of the mod-facing C ABI;
// used here directly rather than through GetProcAddress.
extern "C" EternalSonataAssetResult EternalSonataReplaceTexture(
    const char* ref, const EternalSonataImage* image, uint32_t flags);

// sub_821F6580(ui, id): a screen object by id. sub_82179160(manager, handle,
// colour, 0, 0): its modulate colour, -1 for white. Both called directly to
// take the Custom tab's frame out of the strip.
REX_EXTERN(__imp__sub_821F6580);
REX_EXTERN(__imp__sub_82179160);

namespace achievements_menu {
namespace {

// The strip's icon art. The camp UI textures are NTEX chunks in appkeep.bmd and
// an icon id is that chunk's section index plus one, so the entry option_strip
// moved (icon 186, Status') is section 185: a 64x64 DXT5. The asset API
// addresses textures by their ordinal among all NTX2/NTEX chunks in the
// container, in file order, which for section 185 is 245. NTEX chunks carry no
// name, so an ordinal is the only way to name one.
constexpr const char* kTrophyIconRef = "appkeep.bmd#tex:245";

// The row crystal a locked achievement gets. sub_821FEBC8 picks 288, 289 or 290
// by tab and falls back to icon 230 for a tab of 3 and up, which this screen is
// the only caller ever to pass.
constexpr std::uint32_t kLockedCrystalTab = 3u;

// Screen state, all in the block every status menu screen zeroes on entry.
constexpr std::uint32_t kScratchAddr = 0x8243F358u;      // the tag being opened
constexpr std::uint32_t kScreenIdAddr = 0x8243F364u;     // dword_8243F364
constexpr std::uint32_t kStateAddr = 0x8243F3B8u;        // byte_8243F3B8
constexpr std::uint32_t kHistoryAddr = 0x8243F378u;      // dword_8243F378[16]
constexpr std::uint32_t kHistoryDepthAddr = 0x8243E8A3u; // byte_8243E8A3
constexpr std::uint32_t kHistoryMax = 0x10u;

// dword_82440128: rows in the current tab, the scroll limit sub_82226858 tests.
constexpr std::uint32_t kRowCountAddr = 0x82440128u;

// The Music gallery's own title blob. Its ids are track ids, so a row title
// lookup is recognised by the blob rather than by the id.
constexpr std::uint32_t kTitleBlobAddr = 0x82053E10u;

// The status menu's shared text blob, where the screen's own furniture lives.
constexpr std::uint32_t kMenuBlobAddr = 0x82031A00u;

// Its ids for what the gallery calls the tabs and the playback prompts.
constexpr std::uint32_t kSidPerform = 158u;
constexpr std::uint32_t kSidStop = 159u;
constexpr std::uint32_t kSidTabFirst = 160u;  // Event Sequence, Field, Battle

// The "No." label of a row's left column. sub_821FEBC8 draws it and the row
// number as two separate text objects out of one buffer, so the achievement's
// gamerscore takes the label's place and the number is blanked: the number is
// written by sub_821DC238 into a buffer the caller truncates to two characters,
// which "100G" would not survive.
constexpr std::uint32_t kSidNumberLabel = 210u;

// The description's point size. The gallery's own text records use 40, and a
// record's y is the top of the line rather than its middle, so smaller text
// rides high in the box by half the difference.
constexpr std::uint32_t kStockPointSize = 40u;
constexpr std::uint32_t kDescriptionPointSize = 30u;

// Half the size difference recentres the line; the rest is by eye, the box not
// being centred on the text's own box to begin with.
constexpr std::uint32_t kDescriptionYExtra = 3u;

constexpr std::uint32_t kAchievementsTag = 11u;
constexpr std::uint32_t kMusicScreenId = 0xAu;

// sub_821FEBC8 refuses to build a row whose track id is outside 1..66. Rows
// past what the Music tabs hold would read the table's zero padding, so every
// row is built as track 1 and the title is substituted afterwards.
constexpr std::uint32_t kPlaceholderTrackId = 1u;

constexpr int kTabCount = 3;
constexpr int kMaxRowsPerTab = 32;

// The tab the gallery is on, its saved scroll and cursor per tab, and the
// button word sub_82226858 reads out of the menu state.
constexpr std::uint32_t kTabAddr = 0x8243F370u;
constexpr std::uint32_t kTabScrollAddr = 0x82440120u;
constexpr std::uint32_t kTabCursorAddr = 0x82440124u;
constexpr std::uint32_t kMenuStateAddr = 0x824400E8u;
constexpr std::uint32_t kButtonsOffset = 448u;
constexpr std::uint32_t kButtonUp = 0x10001u;
constexpr std::uint32_t kButtonDown = 0x20002u;
constexpr std::uint32_t kButtonRight = 0x200u;
constexpr std::uint32_t kButtonLeft = 0x100u;

// dword_8243F368: how far the current tab is scrolled.
constexpr std::uint32_t kScrollAddr = 0x8243F368u;

// The third tab's widget id in the screen block, and the object manager every
// sprite handle belongs to (dword_824CF500).
constexpr std::uint32_t kCustomTabWidgetOffset = 1552u;
constexpr std::uint32_t kObjectManagerAddr = 0x824CF500u;

// dword_82555690, the text manager every string object belongs to.
constexpr std::uint32_t kTextManagerAddr = 0x82555690u;

// Which language block of a BTX blob the game is reading: 0 JPN, 1 USA, 2 GBR,
// 3 FRA, 4 ITA, 5 DEU, 6 ESP.
constexpr std::uint32_t kLanguageIndexAddr = 0x8243D370u;

// The tabs, by what they hold rather than by position. The title ships 22
// achievements: 1..14 follow the story, 15..22 are everything else. An id
// outside that range came from a mod, so it lands in the third tab, which is
// hidden entirely when no mod added any. Latin-1, for the same reason
// option_strip's strip label is: the text records are single byte. JPN borrows
// the English words.
constexpr std::uint32_t kLastProgressionId = 14u;
constexpr std::uint32_t kLastStockId = 22u;

// The Y prompt, which on the Music gallery reads "Stop". Its slot moves left by
// this much, which leaves the label the room the A prompt's "Perform" had.
constexpr std::uint32_t kRevealPromptShift = 60u;

// The Y prompt, which on the Music gallery reads "Stop".
constexpr const char* kRevealLabel[7] = {
    "Reveal", "Reveal", "Reveal", "R\xE9v\xE9ler", "Rivela", "Zeigen",
    "Revelar"};

constexpr const char* kTabLabel[kTabCount][7] = {
    {"Progression", "Progression", "Progression", "Progression", "Progressione",
     "Fortschritt", "Progresi\xF3n"},
    {"Other", "Other", "Other", "Autres", "Altri", "Sonstige", "Otros"},
    {"Custom", "Custom", "Custom", "Personnalis\xE9", "Personalizzati",
     "Eigene", "Personalizados"},
};

// Restrict edits to the seven stock Music lists.
constexpr std::uint32_t kScreenLists[] = {
    0x8205BBC0u, 0x820315B8u, 0x82065998u, 0x8206A878u,
    0x82060AD0u, 0x82074628u, 0x8206F750u};
constexpr std::uint32_t kRecordEnd = 0xFFFFFFFFu;
constexpr std::uint32_t kListEnd = 0x0000FFFFu;
constexpr std::uint32_t kMaxListWords = 0x400u;
constexpr std::uint32_t kTypeGlyph = 100u;
constexpr std::uint32_t kTypeText = 200u; // {200, sid, x, y, ...}

std::uint32_t ReadU32(const std::uint8_t* base, std::uint32_t address) {
  return rex::memory::load_and_swap<std::uint32_t>(base + address);
}

void WriteU32(std::uint8_t* base, std::uint32_t address, std::uint32_t value) {
  rex::memory::store_and_swap<std::uint32_t>(base + address, value);
}

struct Row {
  std::uint32_t id = 0;
  std::string label;
  std::string description;
  std::string score;  // the gamerscore, as it is drawn: "100G"
  bool unlocked = false;
  bool secret = false;  // the title hides its description until it is earned
  bool revealed = false;
};

// The guest row object each visible row was last built into. sub_821FEBC8
// keeps a row's title text object at +20, which is what a reveal rewrites, so
// the row does not have to be built again. Only the four rows on screen are
// ever addressed this way, and each of those was recorded by the build that put
// it there.
constexpr std::uint32_t kRowTitleObjectOffset = 20u;
// The sprite group everything in a row hangs off, and the row's crystal.
constexpr std::uint32_t kRowGroupOffset = 16u;
constexpr std::uint32_t kRowIconOffset = 32u;
std::uint32_t g_row_object[kTabCount][kMaxRowsPerTab] = {};

std::mutex g_mutex;         // guards g_tabs, g_built and g_revealed
bool g_built = false;
std::vector<Row> g_tabs[kTabCount];

// Reveals are per session and are not in the catalogue, so they are kept by id
// and reapplied whenever the rows are rebuilt.
std::set<std::uint32_t> g_revealed;

bool g_active = false;

// Set only for the duration of the one sub_821F2890 call that builds a row, so
// the single title lookup that call makes is ours.
thread_local bool t_building_row = false;
thread_local int t_row_tab = 0;
thread_local int t_row_index = 0;
thread_local int t_building_tab = -1;

std::uint32_t g_title_string = 0;  // guest buffers, allocated on first use
std::uint32_t g_tab_string = 0;
std::uint32_t g_prompt_string = 0;
std::uint32_t g_reveal_string = 0;
std::uint32_t g_score_string = 0;
std::uint32_t g_screen_title_string = 0;
std::uint32_t g_list_copy = 0;
std::uint32_t g_list_copy_words = 0;
std::uint32_t g_description_string = 0;
std::uint32_t g_description_object = 0;
int g_description_tab = -1;
int g_description_row = -1;

// The guest's text records are single byte, so a multi-byte character would
// draw as two garbled glyphs. Achievement metadata is UTF-8 (it comes from the
// XDBF, or from a mod's translation table), so fold it to Latin-1 and drop what
// does not fit.
std::string ToLatin1(const std::string& utf8) {
  std::string out;
  out.reserve(utf8.size());
  for (std::size_t i = 0; i < utf8.size();) {
    const unsigned char c = static_cast<unsigned char>(utf8[i]);
    if (c < 0x80u) {
      out.push_back(static_cast<char>(c));
      ++i;
    } else if ((c & 0xE0u) == 0xC0u && i + 1 < utf8.size()) {
      const unsigned int cp =
          ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(utf8[i + 1]) & 0x3Fu);
      out.push_back(static_cast<char>(cp < 0x100u ? cp : '?'));
      i += 2;
    } else {
      out.push_back('?');
      i += (c & 0xF0u) == 0xE0u ? 3 : ((c & 0xF8u) == 0xF0u ? 4 : 1);
    }
  }
  return out;
}

// Builds the row tables once, from the host catalogue, in id order. Which tab a
// row lands in follows its id, so a mod's achievements stay together in Custom
// however many the title itself has.
void EnsureRows() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_built) {
    return;
  }
  g_built = true;
  for (auto& tab : g_tabs) {
    tab.clear();
  }

  auto* kernel = rex::system::kernel_state();
  if (!kernel) {
    return;
  }
  std::vector<rex::system::AchievementInfo> catalogue =
      kernel->achievements().ListAchievements();
  std::sort(catalogue.begin(), catalogue.end(),
            [](const rex::system::AchievementInfo& a,
               const rex::system::AchievementInfo& b) { return a.id < b.id; });

  for (const auto& info : catalogue) {
    const int tab = info.id > kLastStockId          ? 2
                    : info.id > kLastProgressionId  ? 1
                                                    : 0;
    if (g_tabs[tab].size() >= kMaxRowsPerTab) {
      continue;
    }
    const bool unlocked = kernel->achievements().IsUnlocked(info.id);
    const bool secret =
        !(info.flags & rex::system::kAchievementFlagShowUnachieved);
    g_tabs[tab].push_back(
        Row{info.id, ToLatin1(info.label),
            ToLatin1(!unlocked && !info.unachieved_description.empty()
                         ? info.unachieved_description : info.description),
            std::to_string(info.gamerscore) + "G",
            unlocked, secret, g_revealed.count(info.id) != 0});
  }
  REXLOG_INFO("[achievements] menu rows: {} / {} / {}", g_tabs[0].size(),
              g_tabs[1].size(), g_tabs[2].size());
}

// A row reads as itself once it is earned, or once the player asked for it with
// the Reveal prompt. Locked rows otherwise read the way the gallery's own
// locked tracks do, and a secret one keeps its description hidden as well.
// A row past the end of the tab has no title at all: it is one of the four the
// layout always builds, and HideRow paints the rest of it out.
std::string RowTitle(const Row* row) {
  if (!row) {
    return std::string();
  }
  // Only a secret keeps its name back; an ordinary locked row reads as itself,
  // the same way its description is already shown.
  if (row->unlocked || row->revealed || !row->secret) {
    return row->label;
  }
  return std::string("???");
}

std::string RowDescription(const Row* row) {
  if (!row) {
    return std::string();
  }
  if (row->unlocked || row->revealed || !row->secret) {
    return row->description;
  }
  return std::string();
}

const Row* FindRow(int tab, int index) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (tab < 0 || tab >= kTabCount || index < 0 ||
      index >= static_cast<int>(g_tabs[tab].size())) {
    return nullptr;
  }
  return &g_tabs[tab][static_cast<std::size_t>(index)];
}

// The Custom tab only exists when a mod added an achievement. Otherwise it is
// left unlabelled and the cursor is walked past it (see the sub_82226858 hook).
bool CustomTabVisible() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return !g_tabs[2].empty();
}

std::uint32_t RowCount(int tab) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (tab < 0 || tab >= kTabCount) {
    return 0;
  }
  return static_cast<std::uint32_t>(g_tabs[tab].size());
}

// The scroll limit both the init and the tab draw take from byte_822FF594, so
// it has to be restated after either of them runs.
void FixRowCount(std::uint8_t* base, int tab) {
  WriteU32(base, kRowCountAddr, RowCount(tab));
}

std::string ReadGuestString(const std::uint8_t* base, std::uint32_t address) {
  std::string out;
  for (std::uint32_t i = 0; i < 128u; ++i) {
    const char c = static_cast<char>(base[address + i]);
    if (!c) {
      break;
    }
    out.push_back(c);
  }
  return out;
}

// The name the strip's entry already carries, in the language it is drawn in.
std::string AchievementsWord(const std::uint8_t* base) {
  return option_strip::AchievementsWord(base);
}

void WriteGuestString(std::uint8_t* base, std::uint32_t address,
                      const std::string& text) {
  std::size_t i = 0;
  for (; i < text.size() && i < 63u; ++i) {
    REX_STORE_U8(address + static_cast<std::uint32_t>(i),
                 static_cast<std::uint8_t>(text[i]));
  }
  REX_STORE_U8(address + static_cast<std::uint32_t>(i), 0u);
}

std::string TabLabel(const std::uint8_t* base, std::uint32_t sid) {
  const std::uint32_t tab = sid - kSidTabFirst;
  if (tab >= static_cast<std::uint32_t>(kTabCount) ||
      (tab == 2u && !CustomTabVisible())) {
    return std::string();
  }
  const std::uint32_t language = ReadU32(base, kLanguageIndexAddr);
  return kTabLabel[tab][language < 7u ? language : 1u];
}

// Paints the third tab's frame out. sub_82227CB0 builds the strip from three
// widgets held at screen + 1544 / 1548 / 1552; each one is a group of sprite
// handles (the body at +0, the two end caps at +8 and +12, the highlight at
// +44), so an empty label still leaves its frame on screen. Colour 0 is alpha
// 0, which is the only handle the widget offers: it has no visibility flag.
void HideCustomTab(PPCContext& ctx, std::uint8_t* base) {
  const std::uint32_t ui = REX_LOAD_U32(0x824400E4u);
  const std::uint32_t screen = ui ? REX_LOAD_U32(ui + 2836u) : 0u;
  if (!screen) {
    return;
  }

  const std::uint32_t saved_r3 = ctx.r3.u32;
  const std::uint32_t saved_r4 = ctx.r4.u32;
  const std::uint32_t saved_r5 = ctx.r5.u32;
  const std::uint32_t saved_r6 = ctx.r6.u32;
  const std::uint32_t saved_r7 = ctx.r7.u32;

  ctx.r3.u32 = ui;
  ctx.r4.u32 = REX_LOAD_U32(screen + kCustomTabWidgetOffset);
  __imp__sub_821F6580(ctx, base);
  const std::uint32_t widget = ctx.r3.u32;
  if (widget) {
    for (const std::uint32_t field : {0u, 8u, 12u, 44u}) {
      const std::uint32_t handle = REX_LOAD_U32(widget + field);
      if (handle == 0xFFFFFFFFu) {
        continue;
      }
      ctx.r3.u32 = kObjectManagerAddr;
      ctx.r4.u32 = handle;
      ctx.r5.u32 = 0u;  // ARGB, so alpha 0
      ctx.r6.u32 = 0u;
      ctx.r7.u32 = 0u;
      __imp__sub_82179160(ctx, base);
    }
  }

  ctx.r3.u32 = saved_r3;
  ctx.r4.u32 = saved_r4;
  ctx.r5.u32 = saved_r5;
  ctx.r6.u32 = saved_r6;
  ctx.r7.u32 = saved_r7;
}

// Four rows are laid out whatever the tab holds, so a tab with fewer than four
// achievements has spare ones. They cannot be refused: sub_821FEBC8's own "no
// row" exit hands the caller a -1 that sub_822273A0 then looks up and writes
// through. So the row is built and painted out instead, group and crystal both,
// which with the empty title RowTitleOverride gives it leaves nothing on screen.
void HideRow(PPCContext& ctx, std::uint8_t* base, std::uint32_t row_object) {
  const std::uint32_t saved_r3 = ctx.r3.u32;
  const std::uint32_t saved_r4 = ctx.r4.u32;
  const std::uint32_t saved_r5 = ctx.r5.u32;
  const std::uint32_t saved_r6 = ctx.r6.u32;
  const std::uint32_t saved_r7 = ctx.r7.u32;

  for (const std::uint32_t field : {kRowGroupOffset, kRowIconOffset}) {
    const std::uint32_t handle = REX_LOAD_U32(row_object + field);
    if (!handle || handle == 0xFFFFFFFFu) {
      continue;
    }
    ctx.r3.u32 = kObjectManagerAddr;
    ctx.r4.u32 = handle;
    ctx.r5.u32 = 0u;  // ARGB, so alpha 0
    ctx.r6.u32 = 0u;
    ctx.r7.u32 = 0u;
    __imp__sub_82179160(ctx, base);
  }

  ctx.r3.u32 = saved_r3;
  ctx.r4.u32 = saved_r4;
  ctx.r5.u32 = saved_r5;
  ctx.r6.u32 = saved_r6;
  ctx.r7.u32 = saved_r7;
}

// The row under the cursor: the tab's scroll position plus the cursor's slot in
// the selectable group, which handlers read as the byte at group + 44.
bool HighlightedRow(std::uint8_t* base, int* tab, int* index) {
  const std::uint32_t menu = REX_LOAD_U32(kMenuStateAddr);
  if (!menu) {
    return false;
  }
  const std::uint32_t group_id = REX_LOAD_U32(menu + 396u);
  std::uint32_t group = REX_LOAD_U32(menu + 392u);
  for (unsigned count = 0; group && count < 48; ++count) {
    if (REX_LOAD_U32(group) == group_id) {
      break;
    }
    group = REX_LOAD_U32(group + 48u);
  }
  if (!group || REX_LOAD_U32(group) != group_id) {
    return false;
  }
  *tab = static_cast<int>(ReadU32(base, kTabAddr));
  *index = static_cast<int>(ReadU32(base, kScrollAddr)) +
           REX_LOAD_U8(group + 44u);
  return true;
}

// Shows the highlighted row's own name and description in place of "???", for
// this session only: nothing is unlocked and nothing is written to the save.
// The title is rewritten straight into the row's own text object, because
// rebuilding the list (sub_822273A0) allocates a second set of rows over the
// first and takes the scroll back to the top.
void RevealHighlighted(PPCContext& ctx, std::uint8_t* base) {
  int tab = 0;
  int index = 0;
  if (!HighlightedRow(base, &tab, &index) || tab < 0 || tab >= kTabCount ||
      index < 0 || index >= kMaxRowsPerTab) {
    return;
  }

  std::string title;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (index >= static_cast<int>(g_tabs[tab].size())) {
      return;
    }
    Row& row = g_tabs[tab][static_cast<std::size_t>(index)];
    if (row.revealed) {
      return;
    }
    row.revealed = true;
    g_revealed.insert(row.id);
    title = RowTitle(&row);
  }

  g_description_row = -1;  // so the description is rewritten this tick

  const std::uint32_t row_object = g_row_object[tab][index];
  if (!row_object) {
    return;
  }
  const std::uint32_t text_object =
      REX_LOAD_U32(row_object + kRowTitleObjectOffset);
  if (!text_object || text_object == 0xFFFFFFFFu) {
    return;
  }
  if (!g_reveal_string) {
    auto* mem = rex::system::kernel_memory();
    g_reveal_string = mem ? mem->SystemHeapAlloc(64, 0x20) : 0;
    if (!g_reveal_string) {
      return;
    }
  }
  WriteGuestString(base, g_reveal_string, title);

  PPCContext call = ctx;
  call.r3.u32 = kTextManagerAddr;
  call.r4.u32 = text_object;
  call.r5.u32 = g_reveal_string;
  call.r6.s64 = -1;
  sub_821D3890(call, base);
}

void UpdateDescription(PPCContext& ctx, std::uint8_t* base) {
  const std::uint32_t ui = REX_LOAD_U32(0x824400E4u);
  if (!ui) {
    return;
  }
  const std::uint32_t screen = REX_LOAD_U32(ui + 2836u);
  if (!screen) {
    return;
  }
  int tab = 0;
  int index = 0;
  if (!HighlightedRow(base, &tab, &index)) {
    return;
  }
  const Row* row = FindRow(tab, index);
  PPCContext call = ctx;
  call.r3.u32 = ui;
  call.r4.u32 = REX_LOAD_U32(screen + 408u);
  sub_821F6580(call, base);
  const std::uint32_t object = call.r3.u32;
  if (!object || (object == g_description_object && tab == g_description_tab &&
                  index == g_description_row)) {
    return;
  }
  if (!g_description_string) {
    auto* mem = rex::system::kernel_memory();
    g_description_string = mem ? mem->SystemHeapAlloc(400, 0x20) : 0;
    if (!g_description_string) {
      return;
    }
  }
  // The guest text setter accepts at most 399 characters.
  const std::string text = RowDescription(row).substr(0, 399);
  for (std::size_t i = 0; i < text.size(); ++i) {
    REX_STORE_U8(g_description_string + static_cast<std::uint32_t>(i),
                 static_cast<std::uint8_t>(text[i]));
  }
  REX_STORE_U8(g_description_string + static_cast<std::uint32_t>(text.size()), 0);
  call.r3.u32 = kTextManagerAddr;
  call.r4.u32 = object;
  call.r5.u32 = g_description_string;
  call.r6.s64 = -1;
  sub_821D3890(call, base);
  g_description_object = object;
  g_description_tab = tab;
  g_description_row = index;
}

}  // namespace

bool Active() { return g_active; }

void InvalidateRows() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_built = false;
}

namespace {

void RegisterIcon(const char* ref, const unsigned char* png, unsigned int size,
                  const char* what) {
  int width = 0;
  int height = 0;
  auto pixels = rex::ui::DecodeImageRGBA(png, size, width, height);
  if (pixels.empty() || width <= 0 || height <= 0) {
    REXLOG_WARN("[achievements] {} did not decode", what);
    return;
  }

  EternalSonataImage image = {};
  image.pixels = pixels.data();
  image.width = static_cast<std::uint32_t>(width);
  image.height = static_cast<std::uint32_t>(height);
  image.mip_levels = 1;

  const auto result = EternalSonataReplaceTexture(ref, &image, 0);
  if (result != ETERNALSONATA_ASSET_OK) {
    REXLOG_WARN("[achievements] {} patch {} failed: {}", what, ref,
                static_cast<int>(result));
  }
}

}  // namespace

void RegisterIcons() {
  RegisterIcon(kTrophyIconRef, eternalsonata::kIconTrophiesPNG,
               eternalsonata::kIconTrophiesPNGSize, "trophy icon");
}

std::uint32_t RowTitleOverride(std::uint8_t* base, std::uint32_t blob,
                               std::uint32_t sid) {
  if (!g_active) {
    return 0;
  }
  // The tab strip. Its three labels are looked up from code rather than from
  // the display list, so there is no record to edit: they are answered here
  // instead, and only while this screen is up.
  if (blob == kMenuBlobAddr && sid >= kSidTabFirst &&
      sid < kSidTabFirst + kTabCount) {
    if (!g_tab_string) {
      auto* mem = rex::system::kernel_memory();
      g_tab_string = mem ? mem->SystemHeapAlloc(64, 0x20) : 0;
      if (!g_tab_string) {
        return 0;
      }
    }
    WriteGuestString(base, g_tab_string, TabLabel(base, sid));
    return g_tab_string;
  }
  // The kept Y prompt, which reads Reveal here rather than Stop.
  if (blob == kMenuBlobAddr && sid == kSidStop) {
    if (!g_prompt_string) {
      auto* mem = rex::system::kernel_memory();
      g_prompt_string = mem ? mem->SystemHeapAlloc(64, 0x20) : 0;
      if (!g_prompt_string) {
        return 0;
      }
    }
    const std::uint32_t language = ReadU32(base, kLanguageIndexAddr);
    WriteGuestString(base, g_prompt_string,
                     kRevealLabel[language < 7u ? language : 1u]);
    return g_prompt_string;
  }
  if (!t_building_row) {
    return 0;
  }
  const Row* row = FindRow(t_row_tab, t_row_index);
  // The row's left column, which the gallery uses for "No. NN".
  if (blob == kMenuBlobAddr && sid == kSidNumberLabel) {
    if (!g_score_string) {
      auto* mem = rex::system::kernel_memory();
      g_score_string = mem ? mem->SystemHeapAlloc(64, 0x20) : 0;
      if (!g_score_string) {
        return 0;
      }
    }
    WriteGuestString(base, g_score_string, row ? row->score : std::string());
    return g_score_string;
  }
  if (blob != kTitleBlobAddr) {
    return 0;
  }
  if (!g_title_string) {
    auto* mem = rex::system::kernel_memory();
    g_title_string = mem ? mem->SystemHeapAlloc(64, 0x20) : 0;
    if (!g_title_string) {
      return 0;
    }
  }
  WriteGuestString(base, g_title_string, RowTitle(row));
  return g_title_string;
}

bool WantsTitleSwap(std::uint32_t blob) {
  return g_active && blob == kMenuBlobAddr;
}

std::uint32_t TitleOverrideFor(std::uint8_t* base, std::uint32_t result,
                               std::uint32_t music_text) {
  if (!g_active || !result || !music_text) {
    return 0;
  }
  const std::string text = ReadGuestString(base, result);
  const std::string music = ReadGuestString(base, music_text);
  if (music.empty()) {
    return 0;
  }
  // The heading carries the blob's own markup, "<g><m2>" and the like, ahead of
  // the word itself. Keep whatever it opened with and replace only the word.
  std::size_t at = 0;
  while (at < text.size() && text[at] == '<') {
    const std::size_t close = text.find('>', at);
    if (close == std::string::npos) {
      break;
    }
    at = close + 1;
  }
  if (text.compare(at, std::string::npos, music) != 0) {
    return 0;
  }
  if (!g_screen_title_string) {
    auto* mem = rex::system::kernel_memory();
    g_screen_title_string = mem ? mem->SystemHeapAlloc(64, 0x20) : 0;
    if (!g_screen_title_string) {
      return 0;
    }
  }
  WriteGuestString(base, g_screen_title_string,
                   text.substr(0, at) + AchievementsWord(base));
  return g_screen_title_string;
}

std::uint32_t MaybeSwapScreenList(std::uint8_t* base, std::uint32_t list) {
  if (!g_active ||
      std::find(std::begin(kScreenLists), std::end(kScreenLists), list) ==
          std::end(kScreenLists)) {
    return 0;
  }

  std::vector<std::uint32_t> words;
  std::uint32_t at = 0;
  while (at < kMaxListWords) {
    const std::uint32_t word = ReadU32(base, list + 4u * at);
    if (word == kListEnd) {
      break;
    }
    words.push_back(word);
    ++at;
  }
  if (at >= kMaxListWords) {
    return 0;
  }

  // Each prompt has command 1, a seven word icon and a nine word label.
  // Other commands have different lengths and can contain sentinel values.
  constexpr std::size_t kPromptWords = 17;
  unsigned perform_count = 0;
  unsigned stop_count = 0;
  std::uint32_t reveal_glyph_x = 0;
  for (std::size_t i = 0; i + kPromptWords <= words.size(); ++i) {
    if (words[i] != 1u || words[i + 1] != kTypeGlyph ||
        words[i + 7] != kRecordEnd || words[i + 8] != kTypeText ||
        words[i + 16] != kRecordEnd) {
      continue;
    }
    if (words[i + 2] == 236u && words[i + 9] == kSidPerform) {
      ++perform_count;
    } else if (words[i + 2] == 239u && words[i + 9] == kSidStop) {
      ++stop_count;
      // Kept, as the Reveal prompt, but slid left: the stock slot leaves only
      // as much room before Back as "Stop" needs, and every translation of
      // Reveal is longer than that.
      words[i + 3] -= kRevealPromptShift;
      words[i + 10] -= kRevealPromptShift;
      reveal_glyph_x = words[i + 3];
      continue;
    } else {
      continue;
    }
    // The opening animation requires stable object indices.
    words[i + 3] = static_cast<std::uint32_t>(-10000);
    words[i + 10] = static_cast<std::uint32_t>(-10000);
  }
  if (perform_count != 1 || stop_count != 1 || !reveal_glyph_x) {
    return 0;
  }

  // Only Perform goes: Y keeps its prompt, and its slot, with the label reading
  // Reveal instead of Stop. Change Tab then slides up against it rather than
  // against Back, which is where the freed space is.
  for (std::size_t i = 0; i + 23 <= words.size(); ++i) {
    if (words[i] != kTypeGlyph || words[i + 1] != 240u ||
        words[i + 6] != kRecordEnd || words[i + 7] != kTypeGlyph ||
        words[i + 8] != 242u || words[i + 13] != kRecordEnd ||
        words[i + 14] != kTypeText || words[i + 15] != 64u ||
        words[i + 22] != kRecordEnd) {
      continue;
    }
    // A tighter gap than the stock 24 the strip left before Back, which reads
    // better with only two prompts to its right.
    constexpr std::uint32_t kGap = 10u;
    const std::uint32_t shift =
        reveal_glyph_x - kGap - words[i + 18] - words[i + 16];
    words[i + 2] += shift;
    words[i + 9] += shift;
    words[i + 16] += shift;
    break;
  }

  // Use the panel width with equal text margins, and set the description's own
  // point size: an achievement's text is longer than a track title, and the
  // stock 40 runs a few of them past the end of the line.
  for (std::size_t i = 0; i + 16 <= words.size(); ++i) {
    if (words[i] == 1u && words[i + 1] == 65u &&
        words[i + 2] == 880u && words[i + 3] == 1150u &&
        words[i + 4] == 90u && words[i + 5] == 2100u &&
        words[i + 6] == 1u && words[i + 7] == kTypeText &&
        words[i + 8] == 0u && words[i + 15] == kRecordEnd) {
      words[i + 11] = words[i + 3] - 2u * words[i + 9];
      words[i + 14] = kDescriptionPointSize;
      words[i + 10] +=
          (kStockPointSize - kDescriptionPointSize) / 2u + kDescriptionYExtra;
      break;
    }
  }

  if (!g_list_copy || g_list_copy_words < at + 1u) {
    auto* mem = rex::system::kernel_memory();
    const std::uint32_t bytes = 4u * (kMaxListWords + 1u);
    g_list_copy = mem ? mem->SystemHeapAlloc(bytes, 0x20) : 0;
    if (!g_list_copy) {
      return 0;
    }
    g_list_copy_words = kMaxListWords + 1u;
  }

  std::uint32_t out = 0;
  for (std::size_t i = 0; i < words.size(); ++i) {
    WriteU32(base, g_list_copy + 4u * out++, words[i]);
  }
  WriteU32(base, g_list_copy + 4u * out, kListEnd);
  return g_list_copy;
}

}  // namespace achievements_menu

// sub_82236CD0(): dispatches the option strip's selected tag, read back from
// dword_8243F358, into a screen id. Tag 11 is ours; every other tag is the
// stock switch, and dispatching one of them is what takes the mode back off.
REX_EXTERN(__imp__sub_82236CD0);

REX_HOOK_RAW(sub_82236CD0) {
  using namespace achievements_menu;
  const std::uint32_t tag = ReadU32(base, kScratchAddr);
  if (tag != kAchievementsTag) {
    g_active = false;
    __imp__sub_82236CD0(ctx, base);
    return;
  }

  EnsureRows();
  g_active = true;

  // loc_82236F08 with r9 = 0xA, the Music arm: remember where we came from,
  // name the screen, and hand the strip's tick back to state 0.
  const std::uint8_t depth = base[kHistoryDepthAddr];
  if (depth < kHistoryMax) {
    WriteU32(base, kHistoryAddr + 4u * depth, 0u);
    base[kHistoryDepthAddr] = static_cast<std::uint8_t>(depth + 1);
  }
  WriteU32(base, kScreenIdAddr, kMusicScreenId);
  base[kStateAddr] = 0u;
}

// sub_822265C8(): the Music gallery's init. It lays out tab 0 and then takes
// the scroll limit from byte_822FF594, so ours has to be restated after it.
REX_EXTERN(__imp__sub_822265C8);

REX_HOOK_RAW(sub_822265C8) {
  achievements_menu::g_description_object = 0;
  __imp__sub_822265C8(ctx, base);
  if (achievements_menu::Active()) {
    achievements_menu::FixRowCount(base, 0);
  }
}

REX_EXTERN(__imp__sub_821DD108);

REX_HOOK_RAW(sub_821DD108) {
  __imp__sub_821DD108(ctx, base);
  if (!achievements_menu::Active()) {
    return;
  }
  // Blanking the third tab's label leaves its frame drawn, so its sprites are
  // made transparent as well. Per tick rather than once: the tab strip repaints
  // itself and recolours the frame on every tab change.
  if (!achievements_menu::CustomTabVisible()) {
    achievements_menu::HideCustomTab(ctx, base);
  }
  // State 3 has the current tab's cursor and text objects ready.
  if (REX_LOAD_U8(0x8243F3C2u) == 3u) {
    achievements_menu::UpdateDescription(ctx, base);
  }
}

// sub_822273A0(tab, animate): draws four rows of one tab and, like the init,
// ends by taking the scroll limit from byte_822FF594.
REX_EXTERN(__imp__sub_822273A0);

REX_HOOK_RAW(sub_822273A0) {
  const int tab = static_cast<int>(ctx.r3.u32);
  achievements_menu::t_building_tab = achievements_menu::Active() ? tab : -1;
  __imp__sub_822273A0(ctx, base);
  achievements_menu::t_building_tab = -1;
  if (achievements_menu::Active()) {
    achievements_menu::FixRowCount(base, tab);
  }
}

REX_EXTERN(__imp__sub_821F8B38);

REX_HOOK_RAW(sub_821F8B38) {
  using namespace achievements_menu;
  if (g_active && t_building_tab >= 0) {
    const std::uint32_t rows = RowCount(t_building_tab);
    const std::uint32_t maximum = rows > 4u ? rows - 4u : 0u;
    const std::uint32_t scrollbar = ctx.r3.u32;
    REX_STORE_U32(scrollbar + 48u, maximum);
    REX_STORE_U32(scrollbar + 52u,
                  std::min(REX_LOAD_U32(scrollbar + 52u), maximum));
    REX_STORE_U32(scrollbar + 56u,
                  std::min(REX_LOAD_U32(scrollbar + 56u), maximum));
  }
  __imp__sub_821F8B38(ctx, base);
}

// sub_821F2890(ui, x, y, track_id, number, tab, ...): builds one row. The row
// is identified by its tab and 1-based number; the track id only has to survive
// sub_821FEBC8's 1..66 range check, because the title is substituted.
REX_EXTERN(__imp__sub_821F2890);

REX_HOOK_RAW(sub_821F2890) {
  using namespace achievements_menu;
  if (!g_active) {
    __imp__sub_821F2890(ctx, base);
    return;
  }
  t_row_tab = static_cast<int>(ctx.r8.u32);
  t_row_index = static_cast<int>(ctx.r7.u32) - 1;
  ctx.r6.u32 = kPlaceholderTrackId;
  t_building_row = true;
  __imp__sub_821F2890(ctx, base);
  t_building_row = false;
}

// sub_821DC238(value, buffer, width, pad): formats an integer into a caller's
// buffer. sub_821FEBC8 uses it for the row number that follows "No.", which the
// gamerscore has taken the place of, so the number is blanked while a row of
// this screen is being built. Every other caller is untouched.
REX_EXTERN(__imp__sub_821DC238);

REX_HOOK_RAW(sub_821DC238) {
  if (achievements_menu::t_building_row) {
    REX_STORE_U8(ctx.r4.u32, 0u);
    ctx.r3.u32 = 0;
    return;
  }
  __imp__sub_821DC238(ctx, base);
}

// sub_821F77F0(unused, filename): starts a stream as the current BGM. The input
// handler still sees a real track under the cursor, so playback is refused here
// rather than remapped.
REX_EXTERN(__imp__sub_821F77F0);

REX_HOOK_RAW(sub_821F77F0) {
  if (achievements_menu::Active()) {
    ctx.r3.u32 = 0;
    return;
  }
  __imp__sub_821F77F0(ctx, base);
}

// sub_822278A0(tab, row): the now playing highlight. Nothing plays on this
// screen, so it would mark a row for a track that was never started.
REX_EXTERN(__imp__sub_822278A0);

REX_HOOK_RAW(sub_822278A0) {
  if (achievements_menu::Active()) {
    ctx.r3.u32 = 0;
    return;
  }
  __imp__sub_822278A0(ctx, base);
}

// sub_82226858(): the gallery's input handler. Its tab step is a hardcoded
// (tab + 1) % 3 and (tab + 2) % 3, so with no Custom tab to show, the current
// tab is spoofed to 2 for the duration of the call: the same step then lands on
// 0 going right and on 1 going left. The scroll position the original saves
// goes to slot 2 with it, so it is moved back afterwards.
REX_EXTERN(__imp__sub_82226858);

REX_HOOK_RAW(sub_82226858) {
  using namespace achievements_menu;
  if (!Active() || CustomTabVisible()) {
    __imp__sub_82226858(ctx, base);
    return;
  }
  const std::uint32_t menu = REX_LOAD_U32(kMenuStateAddr);
  const std::uint32_t buttons = menu ? REX_LOAD_U32(menu + kButtonsOffset) : 0u;
  const std::uint32_t current = REX_LOAD_U32(kTabAddr);
  bool spoofed = false;
  if (current < static_cast<std::uint32_t>(kTabCount) && !(buttons & kButtonUp) && !(buttons & kButtonDown)) {
    const std::uint32_t next = (buttons & kButtonRight) ? (current + 1u) % 3u
                               : (buttons & kButtonLeft) ? (current + 2u) % 3u
                                                         : current;
    spoofed = next == 2u;
  }
  if (spoofed) {
    REX_STORE_U32(kTabAddr, 2u);
  }
  __imp__sub_82226858(ctx, base);
  if (!spoofed) {
    return;
  }
  if (REX_LOAD_U32(kTabAddr) == 2u) {
    REX_STORE_U32(kTabAddr, current);  // the tab was never stepped after all
    return;
  }
  REX_STORE_U8(kTabScrollAddr + current, REX_LOAD_U8(kTabScrollAddr + 2u));
  REX_STORE_U8(kTabCursorAddr + current, REX_LOAD_U8(kTabCursorAddr + 2u));
}

// sub_82227F08(): what the Y button does on this screen. Nothing plays here, so
// it reveals the highlighted row instead: its name and description are shown
// for the rest of the session, without unlocking it or writing anything.
REX_EXTERN(__imp__sub_82227F08);

REX_HOOK_RAW(sub_82227F08) {
  if (!achievements_menu::Active()) {
    __imp__sub_82227F08(ctx, base);
    return;
  }
  achievements_menu::RevealHighlighted(ctx, base);
  ctx.r3.u32 = 0;
}

// sub_821FEBC8(row, track, number, tab, x, y, ...): builds one row into a row
// object. Recording that object is what lets a reveal rewrite the row's title
// without building it again.
REX_EXTERN(__imp__sub_821FEBC8);

REX_HOOK_RAW(sub_821FEBC8) {
  using namespace achievements_menu;
  const std::uint32_t row_object = ctx.r3.u32;
  // r6 is the tab, and the tab only picks the row crystal. A row nobody has
  // earned yet asks for a tab of 3, the fallback icon, so it reads differently
  // from an earned one. Revealing a row does not earn it, so it keeps that icon.
  const bool ours = g_active && t_building_row;
  const Row* row = ours ? FindRow(t_row_tab, t_row_index) : nullptr;
  if (row && !row->unlocked) {
    ctx.r6.u32 = kLockedCrystalTab;
  }
  __imp__sub_821FEBC8(ctx, base);
  if (!ours) {
    return;
  }
  if (!row) {
    HideRow(ctx, base, row_object);
    return;
  }
  if (t_row_tab >= 0 && t_row_tab < kTabCount && t_row_index >= 0 &&
      t_row_index < kMaxRowsPerTab) {
    g_row_object[t_row_tab][t_row_index] = row_object;
  }
}
