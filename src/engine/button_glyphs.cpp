// eternalsonata - Xbox, PlayStation or keyboard and mouse button art over the release's own.
//
// AppKeep.bmd holds the prompt glyphs in consecutive 48x48 NTEX textures: A, B,
// X, Y, LB, LT, RB, RT, Start, Back, plus the D-pad. The 360 file has the Xbox
// art and the PS3 file has cross, circle, square, triangle, L1, L2, R1, R2, Start
// and Select in the same slots (PS3 mode rebuilds the slot array in the 360's
// numbering).
// The art for the release the player is not running is rendered into res/generated, and
// so does the keyboard and mouse set, which covers A, B, X, Y, LB and RB (the mouse
// wheel), LT and RT (Alt and Ctrl), Start and the D-pad. The other slots keep the pad art there.
//
// The battle HUD's button sheet (BattleKeep slot 39) and the cutscene skip
// prompt (AppKeep slot 352, one texture per language) draw the same buttons
// inside larger textures, so those get the art composited over the shipped
// pixels in fixed boxes.
//
// The textures are rewritten in guest memory, which the texture mirror notices
// like any other in place rewrite, so the setting applies live.

#include "button_glyphs.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/ui/image_decode.h>

#include "active_input.h"
#include "eternalsonata_asset_texture.h"
#include "images.generated.h"
#include "ps3_appkeep.h"
#include "target.h"

REXCVAR_DECLARE(std::string, override_button_glyphs);
REXCVAR_DECLARE(std::string, prompt_device);

namespace button_glyphs {
namespace {

constexpr uint32_t kGlyphSize = 48;

// AppKeep slots: A, B, X, Y, LB, LT, RB, RT, Start, Back in a row, then the D-pad.
constexpr uint32_t kFirstGlyphSlot = 235;
constexpr uint32_t kDpadSlot = 264;
// The load menu's D-pad (a Mefc with the pointer arrow after it).
constexpr uint32_t kLoadDpadSlot = 147;

// The art for each glyph in slot order, ending with Start and Back or Select.
constexpr const char* kXboxGlyphs[] = {"button_xbox_a",  "button_xbox_b",  "button_xbox_x",
                                       "button_xbox_y",  "button_xbox_lb", "button_xbox_lt",
                                       "button_xbox_rb", "button_xbox_rt", "button_xbox_start",
                                       "button_xbox_back"};
constexpr const char* kPlayStationGlyphs[] = {
    "button_ps_cross", "button_ps_circle", "button_ps_square", "button_ps_triangle",
    "button_ps_l1",    "button_ps_l2",     "button_ps_r1",     "button_ps_r2",
    "button_ps_start", "button_ps_select"};

// Null where the keyboard has no counterpart. The colours follow the pad art, so
// PlayStation prompts get the PlayStation colours.
constexpr const char* kPcGlyphs[] = {
    "button_pc_360_left_click", "button_pc_360_right_click", "button_pc_360_middle_click",
    "button_pc_360_space",      "button_pc_360_lb",          "button_pc_360_lt",
    "button_pc_360_rb",         "button_pc_360_rt",                     "button_pc_esc",
    nullptr};
constexpr const char* kPcPlayStationGlyphs[] = {
    "button_pc_ps3_left_click", "button_pc_ps3_right_click", "button_pc_ps3_middle_click",
    "button_pc_ps3_space",      "button_pc_ps3_lb",          "button_pc_ps3_lt",
    "button_pc_ps3_rb",         "button_pc_ps3_rt",                     "button_pc_esc",
    nullptr};

const eternalsonata::ResImage* FindArt(const char* name) {
  for (const auto* art = eternalsonata::kResImages; art->name; ++art) {
    if (std::strcmp(art->name, name) == 0)
      return art;
  }
  return nullptr;
}

// Fits the art into the square, centred and keeping its aspect, since the
// shoulder buttons are not square. Box filter on premultiplied alpha, so a
// transparent corner does not bleed its colour into the edge.
std::vector<uint8_t> Downscale(const std::vector<uint8_t>& src, int width, int height,
                               uint32_t out_w = kGlyphSize, uint32_t out_h = kGlyphSize) {
  const double scale = std::min(double(out_w) / width, double(out_h) / height);
  const double fit_w = width * scale;
  const double fit_h = height * scale;
  const double left = (out_w - fit_w) / 2;
  const double top = (out_h - fit_h) / 2;
  std::vector<uint8_t> out(size_t{out_w} * out_h * 4);
  for (uint32_t y = 0; y < out_h; ++y) {
    for (uint32_t x = 0; x < out_w; ++x) {
      const int x0 = static_cast<int>(std::floor((x - left) / scale));
      const int y0 = static_cast<int>(std::floor((y - top) / scale));
      const int x1 = std::max(x0 + 1, static_cast<int>(std::floor((x + 1 - left) / scale)));
      const int y1 = std::max(y0 + 1, static_cast<int>(std::floor((y + 1 - top) / scale)));
      double sum[4] = {};
      int count = 0;
      for (int sy = y0; sy < y1; ++sy) {
        for (int sx = x0; sx < x1; ++sx) {
          ++count;
          if (sx < 0 || sy < 0 || sx >= width || sy >= height)
            continue;
          const uint8_t* p = &src[(size_t(sy) * width + sx) * 4];
          for (int c = 0; c < 3; ++c)
            sum[c] += double(p[c]) * p[3];
          sum[3] += p[3];
        }
      }
      uint8_t* q = &out[(size_t(y) * out_w + x) * 4];
      for (int c = 0; c < 3; ++c)
        q[c] = sum[3] > 0.0 ? uint8_t(sum[c] / sum[3] + 0.5) : 0;
      q[3] = uint8_t(sum[3] / count + 0.5);
    }
  }
  return out;
}

// The file's own pixels per slot, taken before the first rewrite so "disabled" and
// the release's own choice can put them back.
struct Original {
  uint32_t pixels = 0;  // guest address of the pixel region
  std::vector<uint8_t> bytes;
};

std::mutex g_mutex;
std::map<uint32_t, Original> g_originals;
bool g_loaded = false;
// What auto last decided; read and written under g_mutex.
bool g_auto_keyboard = false;

uint8_t* Guest(uint32_t address) {
  return rex::Runtime::instance()->memory()->TranslateVirtual<uint8_t*>(address);
}

// Writes `art` (or the saved original when null) into the slot's texture.
void Write(uint32_t slot, const char* art_name) {
  const uint32_t entry =
      rex::memory::load_and_swap<uint32_t>(Guest(eternalsonata::kAppKeepSlotArray + 4 * slot));
  if (!entry)
    return;
  uint8_t* chunk = Guest(entry);
  // A bare NTEX, or an effect container whose first texture is the glyph.
  const bool container = std::memcmp(chunk, "Mefc", 4) == 0;
  if (!container && std::memcmp(chunk, "NTEX", 4) != 0) {
    REXLOG_WARN("[button_glyphs] slot {} is not a texture", slot);
    return;
  }
  const uint32_t size = rex::memory::load_and_swap<uint32_t>(chunk + 4);
  std::vector<uint8_t> copy(chunk, chunk + size);
  const auto refs = eternalsonata::assets::FindTextures(copy);
  if (refs.empty() || refs[0].width != kGlyphSize || refs[0].height != kGlyphSize) {
    REXLOG_WARN("[button_glyphs] slot {} is not a 48x48 texture", slot);
    return;
  }
  const auto& ref = refs[0];
  const uint32_t pix = uint32_t(ref.chunk_offset) + ref.pix_offset;
  auto original = g_originals.find(slot);
  if (original == g_originals.end()) {
    original = g_originals
                   .emplace(slot, Original{entry + pix,
                                           std::vector<uint8_t>(copy.begin() + pix,
                                                                copy.begin() + pix +
                                                                    ref.pix_size)})
                   .first;
  }
  if (!art_name) {
    std::memcpy(chunk + pix, original->second.bytes.data(),
                original->second.bytes.size());
    return;
  }

  const auto* art = FindArt(art_name);
  if (!art) {
    REXLOG_WARN("[button_glyphs] no art named {}", art_name);
    return;
  }
  int width = 0;
  int height = 0;
  auto pixels = rex::ui::DecodeImageRGBA(art->data, art->size, width, height);
  if (pixels.empty() || width <= 0 || height <= 0) {
    REXLOG_WARN("[button_glyphs] {} did not decode", art_name);
    return;
  }
  if (static_cast<uint32_t>(width) != kGlyphSize || static_cast<uint32_t>(height) != kGlyphSize)
    pixels = Downscale(pixels, width, height);

  eternalsonata::assets::SourceImage image;
  image.width = kGlyphSize;
  image.height = kGlyphSize;
  image.pixels = std::move(pixels);
  std::string error;
  if (eternalsonata::assets::ApplyTextureEdit(copy, ref, image, &error) !=
      eternalsonata::assets::EditStatus::kOk) {
    REXLOG_WARN("[button_glyphs] slot {} {}", slot, error);
    return;
  }
  std::memcpy(chunk + pix, copy.data() + pix, ref.pix_size);
}

// A rectangle of a texture that is cleared and given one glyph.
struct Edit {
  int x0, y0, x1, y1;  // cleared, in texels
  int art_w, art_h;    // the glyph, centred in the cleared box
  int glyph;           // index into the glyph names: 0 = A ... 4 = LB, 6 = RB
  bool dim;            // the greyed copy of a button
};

// The top mip of a DXT5 texture.
std::vector<uint8_t> DecodeDxt5(const uint8_t* src, uint32_t width, uint32_t height) {
  std::vector<uint8_t> out(size_t{width} * height * 4);
  auto expand = [](uint16_t c, uint8_t* rgb) {
    rgb[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
    rgb[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
    rgb[2] = uint8_t((c & 31) * 255 / 31);
  };
  for (uint32_t by = 0; by < height / 4; ++by) {
    for (uint32_t bx = 0; bx < width / 4; ++bx) {
      const uint8_t* block = src + (size_t(by) * (width / 4) + bx) * 16;
      uint8_t alpha[8] = {block[0], block[1]};
      for (int i = 1; i < 7; ++i) {
        if (alpha[0] > alpha[1])
          alpha[i + 1] = uint8_t(((7 - i) * alpha[0] + i * alpha[1]) / 7);
        else if (i < 5)
          alpha[i + 1] = uint8_t(((5 - i) * alpha[0] + i * alpha[1]) / 5);
      }
      if (alpha[0] <= alpha[1]) {
        alpha[6] = 0;
        alpha[7] = 255;
      }
      uint64_t alpha_bits = 0;
      for (int i = 0; i < 6; ++i)
        alpha_bits |= uint64_t(block[2 + i]) << (8 * i);
      uint8_t palette[4][3];
      expand(uint16_t(block[8] | (block[9] << 8)), palette[0]);
      expand(uint16_t(block[10] | (block[11] << 8)), palette[1]);
      for (int c = 0; c < 3; ++c) {
        palette[2][c] = uint8_t((2 * palette[0][c] + palette[1][c]) / 3);
        palette[3][c] = uint8_t((palette[0][c] + 2 * palette[1][c]) / 3);
      }
      const uint32_t indices = block[12] | (block[13] << 8) | (block[14] << 16) |
                               (uint32_t(block[15]) << 24);
      for (int i = 0; i < 16; ++i) {
        uint8_t* q = &out[((size_t(by) * 4 + i / 4) * width + bx * 4 + i % 4) * 4];
        const uint8_t* rgb = palette[(indices >> (2 * i)) & 3];
        q[0] = rgb[0];
        q[1] = rgb[1];
        q[2] = rgb[2];
        q[3] = alpha[(alpha_bits >> (3 * i)) & 7];
      }
    }
  }
  return out;
}

// The shipped pixels of each sheet, by the chunk's guest address.
std::map<uint32_t, std::vector<uint8_t>> g_sheet_originals;

// Rewrites the NTEX chunk at `chunk`: the shipped pixels with each edit's box
// replaced by its glyph's art, or the shipped pixels when no edit has art.
// `alts` are other layouts for textures whose buttons sit elsewhere; the first one
// with a button under every box wins when the shipped pixels have none under `primary`.
void WriteSheet(uint32_t chunk, const std::vector<Edit>& primary, const char* const* names,
                std::initializer_list<const std::vector<Edit>*> alts = {}) {
  uint8_t* data = Guest(chunk);
  if (std::memcmp(data, "NTEX", 4) != 0)
    return;
  const uint32_t size = rex::memory::load_and_swap<uint32_t>(data + 4);
  std::vector<uint8_t> copy(data, data + size);
  const auto refs = eternalsonata::assets::FindTextures(copy);
  if (refs.empty() || refs[0].chunk_offset != 0 ||
      refs[0].format != eternalsonata::assets::TextureFormat::kDXT5) {
    REXLOG_WARN("[button_glyphs] sheet at {:08X} is not a DXT5 texture", chunk);
    return;
  }
  const auto& ref = refs[0];
  auto original = g_sheet_originals.find(chunk);
  if (original == g_sheet_originals.end()) {
    original = g_sheet_originals
                   .emplace(chunk, std::vector<uint8_t>(copy.begin() + ref.pix_offset,
                                                        copy.begin() + ref.pix_offset +
                                                            ref.pix_size))
                   .first;
  }
  bool any = false;
  for (const auto& edit : primary)
    any |= names[edit.glyph] != nullptr;
  if (!any) {
    std::memcpy(data + ref.pix_offset, original->second.data(), original->second.size());
    return;
  }

  const uint32_t width = ref.width;
  auto pixels = DecodeDxt5(original->second.data(), width, ref.height);
  const auto opaque_under = [&](const std::vector<Edit>& list) {
    for (const auto& e : list) {
      const size_t at = (size_t((e.y0 + e.y1) / 2) * width + (e.x0 + e.x1) / 2) * 4;
      if (pixels[at + 3] < 128)
        return false;
    }
    return true;
  };
  const std::vector<Edit>* chosen = &primary;
  if (!opaque_under(primary)) {
    for (const auto* alt : alts) {
      if (opaque_under(*alt)) {
        chosen = alt;
        break;
      }
    }
  }
  const std::vector<Edit>& edits = *chosen;
  for (const auto& edit : edits) {
    const char* name = names[edit.glyph];
    const auto* art = name ? FindArt(name) : nullptr;
    if (!art)
      continue;
    int art_w = 0;
    int art_h = 0;
    auto glyph = rex::ui::DecodeImageRGBA(art->data, art->size, art_w, art_h);
    if (glyph.empty() || art_w <= 0 || art_h <= 0) {
      REXLOG_WARN("[button_glyphs] {} did not decode", name);
      continue;
    }
    glyph = Downscale(glyph, art_w, art_h, edit.art_w, edit.art_h);
    if (edit.dim) {
      // The shipped greyed shoulder lettering is lighter than the greyed discs.
      const float grey = edit.glyph >= 4 ? 0.9f : 0.6f;
      for (size_t i = 0; i < glyph.size(); i += 4) {
        const float lum = 0.299f * glyph[i] + 0.587f * glyph[i + 1] + 0.114f * glyph[i + 2];
        for (int c = 0; c < 3; ++c)
          glyph[i + c] = uint8_t(grey * (0.8f * lum + 0.2f * glyph[i + c]));
      }
    }
    const int left = (edit.x0 + edit.x1 - edit.art_w) / 2;
    const int top = (edit.y0 + edit.y1 - edit.art_h) / 2;
    for (int y = edit.y0; y < edit.y1; ++y) {
      for (int x = edit.x0; x < edit.x1; ++x) {
        uint8_t* q = &pixels[(size_t(y) * width + x) * 4];
        const int gx = x - left;
        const int gy = y - top;
        if (gx >= 0 && gy >= 0 && gx < edit.art_w && gy < edit.art_h)
          std::memcpy(q, &glyph[(size_t(gy) * edit.art_w + gx) * 4], 4);
        else
          std::memset(q, 0, 4);
      }
    }
  }
  eternalsonata::assets::SourceImage image;
  image.width = width;
  image.height = ref.height;
  image.pixels = std::move(pixels);
  std::string error;
  if (eternalsonata::assets::ApplyTextureEdit(copy, ref, image, &error) !=
      eternalsonata::assets::EditStatus::kOk) {
    REXLOG_WARN("[button_glyphs] sheet at {:08X} {}", chunk, error);
    return;
  }
  std::memcpy(data + ref.pix_offset, copy.data() + ref.pix_offset, ref.pix_size);
}

// The battle HUD's sheet: four face buttons lit and greyed, then LB and RB each
// lit and greyed, whose arrows stay.
const std::vector<Edit>& BattleEdits() {
  static const std::vector<Edit> edits = [] {
    std::vector<Edit> e;
    for (int i = 0; i < 4; ++i) {
      e.push_back({7 + 64 * i, 9, 55 + 64 * i, 57, 44, 44, i, false});
      e.push_back({7 + 64 * i, 73, 55 + 64 * i, 122, 44, 44, i, true});
    }
    e.push_back({17, 128, 53, 152, 34, 22, 4, false});
    e.push_back({81, 128, 117, 152, 34, 22, 4, true});
    e.push_back({146, 166, 184, 192, 34, 22, 6, false});
    e.push_back({210, 166, 248, 192, 34, 22, 6, true});
    return e;
  }();
  return edits;
}

// The skip prompt's A and B, the same in every language's texture.
const std::vector<Edit>& SkipEdits() {
  static const std::vector<Edit> edits = {{57, 67, 105, 113, 44, 44, 0, false},
                                          {321, 67, 371, 113, 44, 44, 1, false}};
  return edits;
}

// The Japanese skip prompts have their own artwork: buttons above the text on the
// PS3 release, below it on the 360.
const std::vector<Edit>& SkipEditsJapanesePs3() {
  static const std::vector<Edit> edits = {{32, 20, 80, 67, 44, 44, 0, false},
                                          {249, 20, 297, 67, 44, 44, 1, false}};
  return edits;
}

const std::vector<Edit>& SkipEditsJapanese360() {
  static const std::vector<Edit> edits = {{32, 80, 80, 127, 44, 44, 0, false},
                                          {249, 80, 297, 127, 44, 44, 1, false}};
  return edits;
}

constexpr uint32_t kBattleSheetSlot = 39;
constexpr uint32_t kSkipPromptSlot = 352;

// The 512x128 NTEX chunks of the skip prompt's entry, one per language.
std::vector<uint32_t> SkipPromptChunks() {
  const uint32_t entry = rex::memory::load_and_swap<uint32_t>(
      Guest(eternalsonata::kAppKeepSlotArray + 4 * kSkipPromptSlot));
  std::vector<uint32_t> chunks;
  if (!entry || std::memcmp(Guest(entry), "Mefc", 4) != 0)
    return chunks;
  const uint8_t* bytes = Guest(entry);
  const uint32_t size = rex::memory::load_and_swap<uint32_t>(bytes + 4);
  const std::vector<uint8_t> copy(bytes, bytes + size);
  for (const auto& ref : eternalsonata::assets::FindTextures(copy)) {
    if (ref.ntex && ref.width == 512 && ref.height == 128)
      chunks.push_back(entry + uint32_t(ref.chunk_offset));
  }
  return chunks;
}

uint32_t g_battle_sheet = 0;  // the battle button sheet's NTEX chunk, 0 until loaded

bool KeyboardPrompts() {
  const std::string device = REXCVAR_GET(prompt_device);
  return device == "mouse_keyboard" || (device == "auto" && g_auto_keyboard);
}

// The art to show, or null for the file's own.
void Apply() {
  if (!g_loaded && !g_battle_sheet)
    return;
  const std::string choice = REXCVAR_GET(override_button_glyphs);
  const bool native = choice != (eternalsonata::IsPs3Target() ? "xbox" : "ps3");
  const bool xbox = choice == "xbox";
  const char* const* glyphs = xbox ? kXboxGlyphs : kPlayStationGlyphs;
  const char* const* keys = UsePlayStationArt() ? kPcPlayStationGlyphs : kPcGlyphs;
  const bool keyboard = KeyboardPrompts();
  const char* names[std::size(kXboxGlyphs)];
  for (uint32_t i = 0; i < std::size(kXboxGlyphs); ++i)
    names[i] = keyboard && keys[i] ? keys[i] : (native ? nullptr : glyphs[i]);
  if (g_battle_sheet) {
    // The HUD draws its shoulder prompts as lettering, so the wheel loses its plate.
    const char* battle[std::size(names)];
    std::copy(std::begin(names), std::end(names), battle);
    if (keyboard) {
      battle[4] = "button_pc_battle_wheel_up";
      battle[6] = "button_pc_battle_wheel_down";
    }
    WriteSheet(g_battle_sheet, BattleEdits(), battle);
  }
  if (!g_loaded)
    return;
  for (uint32_t i = 0; i < std::size(kXboxGlyphs); ++i)
    Write(kFirstGlyphSlot + i, names[i]);
  const char* dpad = keyboard ? "dpad_pc_neutral"
                              : (native ? nullptr : (xbox ? "dpad_xbox_neutral" : "dpad_ps_neutral"));
  Write(kDpadSlot, dpad);
  Write(kLoadDpadSlot, dpad);
  for (const uint32_t chunk : SkipPromptChunks())
    WriteSheet(chunk, SkipEdits(), names, {&SkipEditsJapanesePs3(), &SkipEditsJapanese360()});
}

}  // namespace

void Register() {
  const auto reapply = [](std::string_view, std::string_view) {
    std::lock_guard lock(g_mutex);
    Apply();
  };
  rex::cvar::RegisterChangeCallback("override_button_glyphs", reapply);
  rex::cvar::RegisterChangeCallback("prompt_device", reapply);
}

void Update() {
  if (REXCVAR_GET(prompt_device) != "auto")
    return;
  const bool keyboard =
      eternalsonata::ActiveInput() == eternalsonata::ActiveInputKind::kKeyboardMouse;
  std::lock_guard lock(g_mutex);
  if (keyboard != g_auto_keyboard) {
    g_auto_keyboard = keyboard;
    Apply();
  }
}

bool UsePlayStationArt() {
  const std::string choice = REXCVAR_GET(override_button_glyphs);
  if (choice == "ps3")
    return true;
  if (choice == "xbox" || choice == "pc")
    return false;
  return eternalsonata::IsPs3Target();
}

void OnBattleKeepLoaded(uint32_t slot_array) {
  const uint32_t entry = rex::memory::load_and_swap<uint32_t>(Guest(slot_array + 4 * kBattleSheetSlot));
  uint32_t sheet = 0;
  if (entry) {
    // A 2D animation file: {kind byte, offset} rows, kind 1 a texture. The sheet is
    // the second texture.
    const uint8_t* tex = Guest(entry);
    if (rex::memory::load_and_swap<uint32_t>(tex) == 0x03339010u) {
      const uint32_t rows = rex::memory::load_and_swap<uint16_t>(tex + 4);
      uint32_t textures = 0;
      for (uint32_t i = 0; i < rows; ++i) {
        const uint8_t* row = tex + 8 + 8 * i;
        if (row[0] == 1 && textures++ == 1)
          sheet = entry + rex::memory::load_and_swap<uint32_t>(row + 4);
      }
    }
  }
  std::lock_guard lock(g_mutex);
  g_battle_sheet = sheet;
  if (sheet) {
    g_sheet_originals.erase(sheet);
    Apply();
  }
}

void OnAppKeepLoaded() {
  std::lock_guard lock(g_mutex);
  g_originals.clear();
  for (const uint32_t chunk : SkipPromptChunks())
    g_sheet_originals.erase(chunk);
  g_loaded = true;
  Apply();
}

}  // namespace button_glyphs
