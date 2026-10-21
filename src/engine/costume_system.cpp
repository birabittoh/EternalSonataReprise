// eternalsonata - Character costumes (docs/costumes.md).
//
// The field, battle and event code all take a character's model from
// dword_82420AF8, a table of raw NOBJ pointers into AppKeep.bmd that
// sub_82162058 fills at boot, and build it in place. A costume is another
// NOBJ: wearing one points the character's slot at it, so every reader builds
// it the next time, as the PS3 does with its WEAR slots. Crescendo and
// Serenade have no slot; their readers ask CostumeModel instead. The field
// leader is the one model that outlives a change, so it is respawned.
//
// A costume's model loads into physical guest memory when it is worn. Scene
// objects read it in place, so once it is taken off it stays until every
// object built from it is gone (sub_82176398 / sub_82175620), then is freed:
// what costumes hold stays near one model per character however many exist.
// Portraits borrow free AppKeep slots and give them back the same way.

#include "costume_system.h"
#include "character_roster.h"
#include "costume_tint.h"

#include "field_player_model_override.h"
#include "generated/eternalsonata_init.h"
#include "party_arrays.h"
#include "button_glyphs.h"
#include "ps3_appkeep.h"
#include "target.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/mod_plugin.h>
#include <rex/system/mod_registry.h>
#include <rex/system/xmemory.h>

#include "eternalsonata_costume_api.h"

REXCVAR_DEFINE_STRING(costumes, "", "Eternal Sonata",
                      "Costumes worn at boot, as character=id pairs: \"polka=ps3/3,beat=ps3/2\"")
    .debug_only();

namespace {

constexpr int kCharacters = ETERNALSONATA_COSTUME_CHARACTER_COUNT;
constexpr int kRetailCharacters = 10;
constexpr uint32_t kModelTableAddr = 0x82420AF8u;
constexpr int kPortraitKinds = ETERNALSONATA_COSTUME_PORTRAIT_KIND_COUNT;

// The camp's two face sets, AppKeep image ids per character.
constexpr eternalsonata::PartyArray kFaceArrays[] = {eternalsonata::PartyArray::kPortraitA,
                                                     eternalsonata::PartyArray::kPortraitB};

// dword_82420AF8 slot of each character, 0 for none. The table follows
// AppKeep.bmd, where March comes before Claves; the party numbering has them
// the other way.
constexpr std::array<uint32_t, kCharacters> kModelSlot = {1, 2, 3, 4, 5, 6, 7, 8, 10, 9, 0, 0};


// The NOBJ chunk header: tag and u32 size, the size including the header.
constexpr size_t kChunkHeader = 8;

// A model buffer taken off is freed this long after its last scene object
// went, as the GPU may still draw from it.
constexpr auto kModelFreeGrace = std::chrono::milliseconds(500);

struct Costume {
  std::string id, label;
  std::vector<uint8_t> model;  // owned copy, or empty when read from `path`
  std::filesystem::path path;
  bool own_model = false;      // registered with neither: the character's own
  uint32_t guest = 0;          // loaded model, 0 while not in guest memory
  bool failed = false;         // did not load; not retried
  bool starts_unlocked = true; // in a new game
  bool unlocked = true;        // in the game in progress
  // A costume made from another with some colours replaced (costume_tint.h):
  // that costume's id, empty for none, and the rules for its model, when
  // this one ships none, and for the portraits it ships none of.
  std::string base;
  std::vector<eternalsonata::TintRule> model_tints, portrait_tints;
  // Portraits by ETERNALSONATA_COSTUME_PORTRAIT_* kind: AppKeep image ids, or
  // .dds / NTEX files placed in a free AppKeep slot when shown.
  std::array<uint32_t, kPortraitKinds> portrait{};
  std::array<std::filesystem::path, kPortraitKinds> portrait_path;
  std::array<bool, kPortraitKinds> portrait_failed{};
};

// sub_821E8F58's cache: the image list, whose handle of image id n sits at
// +4 * (n + 3), -1 when not built.
constexpr uint32_t kImageList = 0x824400DCu;

// A free AppKeep slot lent to one costume's portrait of one kind. The game
// caches the texture it decodes from a slot by image id (sub_821E8F58), so a
// slot is only lent again once that cache has let go of it.
struct PortraitEntry {
  int character = 0;  // 0 while free
  std::string costume;
  int kind = 0;
  uint32_t guest = 0;
};

struct State {
  // Index 0 of each list is the default model. A deque keeps the strings
  // handed out through the C ABI at stable addresses.
  std::array<std::deque<Costume>, kCharacters> costumes;
  std::array<int, kCharacters> worn{};
  // What the costumes cvar put on, which a new game starts from.
  std::array<int, kCharacters> boot{};
  // The models as AppKeep.bmd filled them, and the model worn now; all 0
  // until the boot hook ran.
  std::array<uint32_t, kCharacters> defaults{};
  std::array<uint32_t, kCharacters> models{};
  // The face sets' own ids, from the twelve wide copies before any costume.
  std::array<std::array<uint16_t, kCharacters>, 2> default_faces{};
  // By slot, from kFirstFreeAppKeepSlot.
  std::array<PortraitEntry, eternalsonata::kAppKeepSlotCount - eternalsonata::kFirstFreeAppKeepSlot>
      portraits;
  // What tinted costumes are made from: the default models and portrait
  // chunks (by image id) they start from, copied at boot before the game
  // builds anything from them.
  std::array<std::vector<uint8_t>, kCharacters> own_model;
  std::unordered_map<uint32_t, std::vector<uint8_t>> images;
  // Model buffers of costumes a redefined slot dropped, freed once unread.
  std::vector<uint32_t> retired;
  bool live = false;
  bool builtins = false;
  rex::Runtime* runtime = nullptr;
  // [[costume]] tables naming a character nobody is yet: a modded one a code
  // mod defines later. Registered when it is.
  struct Pending {
    std::string character, id, label;
    Costume costume;
  };
  std::vector<Pending> pending;
};

std::mutex g_mutex;

State& state() {
  static State s;
  return s;
}

// Scene objects built from costume models, kept by the guest hooks. Its own
// lock, taken after g_mutex when both are.
struct ModelRefs {
  int objects = 0;
  bool built = false;  // an object was ever built from it
  std::chrono::steady_clock::time_point idle_since;
};
std::mutex g_ref_mutex;
std::unordered_map<uint32_t, ModelRefs> g_model_refs;  // by costume buffer
std::unordered_map<uint32_t, uint32_t> g_object_model; // scene handle -> buffer

// Retail, the PS3's pair on PS3 data, or a modded slot once defined.
bool ValidCharacter(int character) {
  return eternalsonata::CharacterExists(character);
}

int CharacterFromName(std::string_view name) {
  return eternalsonata::CharacterFromToken(name);
}

std::string Token(int character) {
  return eternalsonata::CharacterToken(character);
}

uint32_t ReadBe32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

bool ValidModel(const uint8_t* data, size_t size) {
  return size >= kChunkHeader && std::memcmp(data, "NOBJ", 4) == 0 &&
         ReadBe32(data + 4) >= kChunkHeader && ReadBe32(data + 4) <= size;
}

bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

// Called with g_mutex held, as is everything below that touches state().
void EnsureCharacterLists() {
  State& s = state();
  if (s.builtins)
    return;
  s.builtins = true;
  for (auto& list : s.costumes)
    list.push_back({"default", "Default"});
  if (!eternalsonata::IsPs3Target())
    return;
  // The PS3's own, under the names its camp menu gives them.
  struct Ps3Costume {
    int character;
    const char* file;
    int variant;
    const char* label;
  };
  // In the order of ps3_appkeep's costume portraits.
  constexpr Ps3Costume kPs3[] = {
      {ETERNALSONATA_COSTUME_CHAR_ALLEGRETTO, "pcalg_v2.p3obj", 2, "New Costume"},
      {ETERNALSONATA_COSTUME_CHAR_POLKA, "pcplk_v2.p3obj", 2, "New Costume"},
      {ETERNALSONATA_COSTUME_CHAR_POLKA, "pcplk_v3.p3obj", 3, "New Costume 2"},
      {ETERNALSONATA_COSTUME_CHAR_BEAT, "pcbet_v2.p3obj", 2, "New Costume"},
  };
  for (size_t k = 0; k < std::size(kPs3); ++k) {
    const auto& p = kPs3[k];
    Costume costume;
    costume.id = "ps3/" + std::to_string(p.variant);
    costume.label = p.label;
    costume.path = eternalsonata::GameDataRoot() / p.file;
    costume.portrait[ETERNALSONATA_COSTUME_PORTRAIT_STATUS] =
        eternalsonata::kPs3CostumePortraitSlot + static_cast<uint32_t>(k) + 1;
    for (int kind = ETERNALSONATA_COSTUME_PORTRAIT_PANEL; kind < kPortraitKinds; ++kind)
      costume.portrait[kind] =
          eternalsonata::kPs3CostumeArtSlot + 3 * static_cast<uint32_t>(k) + kind;
    costume.starts_unlocked = costume.unlocked = false;
    s.costumes[p.character - 1].push_back(std::move(costume));
  }
}

int FindLocked(int character, std::string_view id) {
  const auto& list = state().costumes[character - 1];
  for (size_t i = 0; i < list.size(); ++i) {
    if (list[i].id == id)
      return static_cast<int>(i);
  }
  return ETERNALSONATA_COSTUME_ERR_INVALID_COSTUME;
}

int RegisterLocked(int character, const char* id, const char* label, Costume costume) {
  if (!ValidCharacter(character))
    return ETERNALSONATA_COSTUME_ERR_INVALID_CHARACTER;
  if (!id || !*id || !label)
    return ETERNALSONATA_COSTUME_ERR_INVALID_ARGUMENT;
  EnsureCharacterLists();
  if (FindLocked(character, id) >= 0)
    return ETERNALSONATA_COSTUME_ERR_DUPLICATE_ID;
  auto& list = state().costumes[character - 1];
  if (!costume.base.empty()) {
    // One a release lacks, as the PS3's on Xbox 360 data, or one made from
    // another itself.
    const int base = FindLocked(character, costume.base);
    if (base < 0 || !list[base].base.empty())
      return ETERNALSONATA_COSTUME_ERR_INVALID_COSTUME;
  }
  costume.id = id;
  costume.label = *label ? label : id;
  costume.own_model = costume.model.empty() && costume.path.empty() &&
                      (costume.base.empty() ||
                       (costume.base == "default" && costume.model_tints.empty()));
  list.push_back(std::move(costume));
  REXLOG_INFO("costumes: {} can wear '{}' ({})", Token(character), list.back().label,
              list.back().id);
  return static_cast<int>(list.size() - 1);
}

rex::memory::Memory* GuestMemory() {
  State& s = state();
  return s.runtime ? s.runtime->memory() : rex::Runtime::instance()->memory();
}

// Frees a model buffer unless a scene object still reads it. With
// g_ref_mutex held.
bool FreeIfUnread(uint32_t guest, std::chrono::steady_clock::time_point now) {
  const auto it = g_model_refs.find(guest);
  if (it != g_model_refs.end()) {
    const ModelRefs& refs = it->second;
    if (refs.objects > 0 || (refs.built && now - refs.idle_since < kModelFreeGrace))
      return false;
    g_model_refs.erase(it);
  }
  GuestMemory()->SystemHeapFree(guest);
  REXLOG_INFO("costumes: model freed from {:08X}", guest);
  return true;
}

// Frees the models of costumes nobody wears once no scene object reads them,
// and those a redefined slot 11 or 12 left behind. The default and a modded
// character's own file stay.
void CollectModels() {
  State& s = state();
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard refs_lock(g_ref_mutex);
  for (int c = 0; c < kCharacters; ++c) {
    auto& list = s.costumes[c];
    for (size_t i = 1; i < list.size(); ++i) {
      Costume& costume = list[i];
      if (costume.guest && static_cast<int>(i) != s.worn[c] && FreeIfUnread(costume.guest, now))
        costume.guest = 0;
    }
  }
  std::erase_if(s.retired, [&](uint32_t guest) { return FreeIfUnread(guest, now); });
}

// The chunk at a guest address, by its own header's size.
std::vector<uint8_t> CopyChunk(uint32_t address) {
  if (!address)
    return {};
  const uint8_t* p = GuestMemory()->TranslateVirtual<uint8_t*>(address);
  return std::vector<uint8_t>(p, p + ReadBe32(p + 4));
}

// The model of a character's costume `id`, which a tinted costume starts
// from.
std::vector<uint8_t> BaseModel(int character, const std::string& id) {
  State& s = state();
  const int c = character - 1;
  const int index = FindLocked(character, id);
  if (index < 0)
    return {};
  const Costume& base = s.costumes[c][index];
  if (!base.model.empty())
    return base.model;
  std::vector<uint8_t> file;
  if (!base.path.empty()) {
    if (!ReadFile(base.path, file))
      REXLOG_ERROR("costumes: cannot read {}", base.path.string());
    return file;
  }
  return s.own_model[c].empty() ? CopyChunk(s.defaults[c]) : s.own_model[c];
}

// Reads a costume's model into guest memory, or makes it from its base.
uint32_t LoadModel(Costume& costume, int* error, int character = 0) {
  if (costume.guest || costume.failed) {
    *error = ETERNALSONATA_COSTUME_ERR_INVALID_MODEL;
    return costume.guest;
  }
  std::vector<uint8_t> file;
  const std::vector<uint8_t>* model = &costume.model;
  if (model->empty() && costume.path.empty() && !costume.base.empty() && character) {
    file = BaseModel(character, costume.base);
    if (!eternalsonata::TintModel(costume.model_tints, file))
      file.clear();
    model = &file;
  } else if (model->empty()) {
    if (!ReadFile(costume.path, file))
      REXLOG_ERROR("costumes: cannot read {}", costume.path.string());
    model = &file;
  }
  if (!ValidModel(model->data(), model->size())) {
    REXLOG_ERROR("costumes: '{}' is not a NOBJ model", costume.id);
    costume.failed = true;
    *error = ETERNALSONATA_COSTUME_ERR_INVALID_MODEL;
    return 0;
  }
  CollectModels();
  auto* memory = GuestMemory();
  const uint32_t size = static_cast<uint32_t>(model->size());
  const uint32_t at = memory->SystemHeapAlloc(size, 0x1000, rex::memory::kSystemHeapPhysical);
  if (!at) {
    REXLOG_ERROR("costumes: no guest memory for '{}' ({} bytes)", costume.id, size);
    *error = ETERNALSONATA_COSTUME_ERR_NO_MEMORY;
    return 0;
  }
  std::memcpy(memory->TranslateVirtual(at), model->data(), size);
  costume.guest = at;
  {
    std::lock_guard refs_lock(g_ref_mutex);
    g_model_refs[at] = {};
  }
  REXLOG_INFO("costumes: '{}' loaded at {:08X}, {} bytes", costume.id, at, size);
  return at;
}

// The guest model a costume puts in the table, loading it the first time.
uint32_t GuestModel(int character, int index, int* error) {
  State& s = state();
  Costume& costume = s.costumes[character - 1][index];
  if (index == ETERNALSONATA_COSTUME_DEFAULT || costume.own_model)
    return s.defaults[character - 1];
  return LoadModel(costume, error, character);
}

// An NTEX chunk is "NTEX", its size with this header, then a PC DDS.
bool ReadPortrait(const std::filesystem::path& path, std::vector<uint8_t>& out) {
  std::vector<uint8_t> file;
  if (!ReadFile(path, file) || file.size() < 12)
    return false;
  if (std::memcmp(file.data(), "NTEX", 4) == 0) {
    out = std::move(file);
    return true;
  }
  if (std::memcmp(file.data(), "DDS ", 4) != 0)
    return false;
  const uint32_t size = static_cast<uint32_t>(file.size() + kChunkHeader);
  out = {'N', 'T', 'E', 'X', uint8_t(size >> 24), uint8_t(size >> 16), uint8_t(size >> 8),
         uint8_t(size)};
  out.insert(out.end(), file.begin(), file.end());
  return true;
}

uint32_t LoadGuestU32(uint32_t address) {
  return rex::memory::load_and_swap<uint32_t>(GuestMemory()->TranslateVirtual(address));
}

void StoreGuestU32(uint32_t address, uint32_t value) {
  rex::memory::store_and_swap<uint32_t>(GuestMemory()->TranslateVirtual(address), value);
}

// AppKeep image ids are the slot plus one.
uint32_t PoolImage(size_t entry) {
  return eternalsonata::kFirstFreeAppKeepSlot + static_cast<uint32_t>(entry) + 1;
}

// Whether the game holds a texture built from this image id, or one is
// still being decoded from it.
bool ImageCached(uint32_t image) {
  const uint32_t list = LoadGuestU32(kImageList);
  return list && LoadGuestU32(list + 4 * (image + 3)) != 0xFFFFFFFFu;
}

// Gives back the slots of costumes taken off whose textures the game let go.
// Those of costumes worn stay, as the face tables and the panel hand their
// ids out again.
void CollectPortraits() {
  State& s = state();
  for (size_t i = 0; i < s.portraits.size(); ++i) {
    PortraitEntry& entry = s.portraits[i];
    if (!entry.character)
      continue;
    const auto& list = s.costumes[entry.character - 1];
    if (list[s.worn[entry.character - 1]].id == entry.costume || ImageCached(PoolImage(i)))
      continue;
    StoreGuestU32(eternalsonata::kAppKeepSlotArray + 4 * (PoolImage(i) - 1), 0);
    GuestMemory()->SystemHeapFree(entry.guest);
    entry = {};
  }
}

uint16_t LoadU16(rex::memory::Memory* memory, uint32_t address) {
  return rex::memory::load_and_swap<uint16_t>(memory->TranslateVirtual(address));
}

// The image id of a character's own portrait of one kind.
uint32_t OwnPortraitId(int character, int kind) {
  State& s = state();
  if (kind >= ETERNALSONATA_COSTUME_PORTRAIT_FACE)
    return s.default_faces[kind - ETERNALSONATA_COSTUME_PORTRAIT_FACE][character - 1];
  int art = character;
  if (character > kRetailCharacters) {
    if (eternalsonata::IsPs3Target())
      return eternalsonata::kPs3MenuPortraitSlot + character - kRetailCharacters;
    art = eternalsonata::CharacterBase(character);
    if (!art)
      return 0;
  }
  // word_8202C894 is the status art; word_8202C9B4 the panel's of the first
  // four, the rest showing the status art there.
  const uint32_t table =
      kind == ETERNALSONATA_COSTUME_PORTRAIT_PANEL && art <= 4 ? 0x8202C9B4u : 0x8202C894u;
  return LoadU16(GuestMemory(), table + 2u * static_cast<uint32_t>(art - 1));
}

// The image id a base costume shows for one kind, before any slot is lent.
uint32_t BaseImageId(int character, const Costume& base, int kind) {
  return base.portrait[kind] ? base.portrait[kind] : OwnPortraitId(character, kind);
}

// An AppKeep image's NTEX chunk: the boot copy, else what the slot holds.
std::vector<uint8_t> ImageChunk(uint32_t id) {
  State& s = state();
  if (!id)
    return {};
  if (const auto it = s.images.find(id); it != s.images.end())
    return it->second;
  const uint32_t at = LoadGuestU32(eternalsonata::kAppKeepSlotArray + 4 * (id - 1));
  if (!at || std::memcmp(GuestMemory()->TranslateVirtual<uint8_t*>(at), "NTEX", 4) != 0)
    return {};
  return CopyChunk(at);
}

bool TintsKind(const Costume& costume, int kind) {
  return std::any_of(costume.portrait_tints.begin(), costume.portrait_tints.end(),
                     [kind](const eternalsonata::TintRule& rule) {
                       return ((rule.kinds >> kind) & 1) != 0;
                     });
}

// The image id of a costume's portrait, placing its file or its tinted base
// art in a free AppKeep slot when it has none; 0 for the character's own.
uint32_t PortraitImage(int character, Costume& costume, int kind) {
  State& s = state();
  const bool derived = costume.portrait_path[kind].empty() && !costume.base.empty();
  if (costume.portrait[kind] || costume.portrait_failed[kind] ||
      (costume.portrait_path[kind].empty() && !derived))
    return costume.portrait[kind];
  Costume* base = nullptr;
  if (derived) {
    const int index = FindLocked(character, costume.base);
    if (index < 0)
      return 0;
    base = &s.costumes[character - 1][index];
    // Untinted, it shows what the base does.
    if (!TintsKind(costume, kind))
      return PortraitImage(character, *base, kind);
  }
  for (size_t i = 0; i < s.portraits.size(); ++i) {
    const PortraitEntry& entry = s.portraits[i];
    if (entry.character == character && entry.kind == kind && entry.costume == costume.id)
      return PoolImage(i);
  }
  CollectPortraits();
  const auto free = std::find_if(s.portraits.begin(), s.portraits.end(),
                                 [](const PortraitEntry& entry) { return !entry.character; });
  if (free == s.portraits.end()) {
    REXLOG_ERROR("costumes: every free AppKeep slot holds a portrait in use, '{}' shows "
                 "the character's own",
                 costume.id);
    return 0;
  }
  std::vector<uint8_t> chunk;
  if (base) {
    if (base->portrait_path[kind].empty())
      chunk = ImageChunk(BaseImageId(character, *base, kind));
    else
      ReadPortrait(base->portrait_path[kind], chunk);
    if (!eternalsonata::TintPortrait(costume.portrait_tints, kind, chunk)) {
      REXLOG_ERROR("costumes: cannot tint the portrait of '{}'", costume.id);
      costume.portrait_failed[kind] = true;
      return 0;
    }
  } else if (!ReadPortrait(costume.portrait_path[kind], chunk)) {
    REXLOG_ERROR("costumes: {} is not a .dds or NTEX portrait",
                 costume.portrait_path[kind].string());
    costume.portrait_failed[kind] = true;
    return 0;
  }
  auto* memory = GuestMemory();
  const uint32_t size = static_cast<uint32_t>(chunk.size());
  const uint32_t at = memory->SystemHeapAlloc(size, 0x1000, rex::memory::kSystemHeapPhysical);
  if (!at) {
    REXLOG_ERROR("costumes: no guest memory for the portrait of '{}'", costume.id);
    return 0;
  }
  std::memcpy(memory->TranslateVirtual(at), chunk.data(), size);
  *free = {character, costume.id, kind, at};
  const uint32_t image = PoolImage(static_cast<size_t>(free - s.portraits.begin()));
  StoreGuestU32(eternalsonata::kAppKeepSlotArray + 4 * (image - 1), at);
  return image;
}

void StoreU16(rex::memory::Memory* memory, uint32_t address, uint16_t value) {
  rex::memory::store_and_swap<uint16_t>(memory->TranslateVirtual(address), value);
}

// Points the twelve wide face tables at the worn costume's faces, or back at
// the character's own. The panel is substituted where it is built
// (CostumePortrait).
void ApplyCampPortraits(int character) {
  State& s = state();
  Costume& costume = s.costumes[character - 1][s.worn[character - 1]];
  auto* memory = GuestMemory();
  const uint32_t i = static_cast<uint32_t>(character - 1);
  PortraitImage(character, costume, ETERNALSONATA_COSTUME_PORTRAIT_PANEL);
  for (int set = 0; set < 2; ++set) {
    const uint32_t id =
        PortraitImage(character, costume, ETERNALSONATA_COSTUME_PORTRAIT_FACE + set);
    StoreU16(memory, eternalsonata::PartyArrayAddress(kFaceArrays[set], i),
             static_cast<uint16_t>(id ? id : s.default_faces[set][i]));
  }
}

// Copies what a character's tinted costumes start from, before the game
// builds anything from it. With the boot hook's defaults and face sets read.
void SnapshotTintSources(int character) {
  State& s = state();
  const int c = character - 1;
  for (const Costume& costume : s.costumes[c]) {
    if (costume.base.empty())
      continue;
    const int index = FindLocked(character, costume.base);
    if (index < 0)
      continue;
    const Costume& base = s.costumes[c][index];
    if (base.model.empty() && base.path.empty() && s.own_model[c].empty())
      s.own_model[c] = CopyChunk(s.defaults[c]);
    for (int kind = 0; kind < kPortraitKinds; ++kind) {
      if (!TintsKind(costume, kind) || !base.portrait_path[kind].empty())
        continue;
      const uint32_t id = BaseImageId(character, base, kind);
      if (id && !s.images.contains(id))
        if (auto chunk = ImageChunk(id); !chunk.empty())
          s.images.emplace(id, std::move(chunk));
    }
  }
}

// Slot 11 or 12's default model and faces once the boot hook ran: the PS3's
// own, a modded character's own file or its base's, or none.
void SetupAddedSlot(int character) {
  State& s = state();
  const int c = character - 1;
  uint32_t model = 0;
  if (eternalsonata::IsPs3Target()) {
    model = eternalsonata::Ps3AppKeep2Model(static_cast<uint32_t>(c - kRetailCharacters));
  } else if (const int base = eternalsonata::CharacterBase(character)) {
    Costume& own = s.costumes[c][ETERNALSONATA_COSTUME_DEFAULT];
    int error = ETERNALSONATA_COSTUME_OK;
    model = own.path.empty() ? 0 : LoadModel(own, &error);
    if (!model)
      model = s.defaults[base - 1];
  }
  s.defaults[c] = model;
  if (s.worn[c] == ETERNALSONATA_COSTUME_DEFAULT)
    s.models[c] = model;
  auto* memory = s.runtime ? s.runtime->memory() : rex::Runtime::instance()->memory();
  for (int set = 0; set < 2; ++set)
    s.default_faces[set][c] =
        LoadU16(memory, eternalsonata::PartyArrayAddress(kFaceArrays[set], static_cast<uint32_t>(c)));
  if (eternalsonata::CharacterExists(character))
    ApplyCampPortraits(character);
}

// Puts the costume in the table. Before the boot hook there is no table yet,
// so the choice is only recorded and applied there.
int WearLocked(int character, int index) {
  State& s = state();
  if (s.live) {
    int error = ETERNALSONATA_COSTUME_OK;
    const uint32_t model = GuestModel(character, index, &error);
    if (!model && error < 0)
      return error;
    if (kModelSlot[character - 1]) {
      auto* memory = s.runtime ? s.runtime->memory() : rex::Runtime::instance()->memory();
      rex::memory::store_and_swap<uint32_t>(
          memory->TranslateVirtual(kModelTableAddr + 4 * kModelSlot[character - 1]), model);
    }
    s.models[character - 1] = model;
  }
  s.worn[character - 1] = index;
  if (s.live)
    ApplyCampPortraits(character);
  return ETERNALSONATA_COSTUME_OK;
}

void Publish(const char* event, int character, int index) {
  rex::Runtime* runtime = nullptr;
  {
    std::lock_guard lock(g_mutex);
    runtime = state().runtime;
  }
  auto* registry = runtime ? runtime->mod_registry() : nullptr;
  if (!registry)
    return;
  rex::system::ModRegistry::EventPayload payload;
  payload.u64 = static_cast<uint64_t>(character);
  payload.f64 = index;
  registry->Publish(event, payload);
}

// The costumes cvar: "polka=ps3/3,beat=ps3/2".
void WearBootCostumes() {
  const std::string spec = REXCVAR_GET(costumes);
  size_t at = 0;
  while (at < spec.size()) {
    size_t end = spec.find_first_of(",;", at);
    if (end == std::string::npos)
      end = spec.size();
    const std::string pair = spec.substr(at, end - at);
    at = end + 1;
    const size_t eq = pair.find('=');
    if (eq == std::string::npos)
      continue;
    const int character = CharacterFromName(pair.substr(0, eq));
    const int index = character ? FindLocked(character, pair.substr(eq + 1)) : -1;
    if (index < 0) {
      REXLOG_WARN("costumes: no costume '{}' in the costumes cvar", pair);
      continue;
    }
    state().worn[character - 1] = index;
  }
}

// mods/<name>/assets.toml:
//
//   [[costume]]
//   character = "polka"            # or its number, 1..12 (11, 12: PS3 data)
//   name = "swimsuit"              # id becomes "<mod folder>/swimsuit"
//   label = "Swimsuit"
//   model = "costumes/plk_swim.nobj"  # optional, relative to the mod folder
//   locked = true                  # optional: a new game starts it locked
//   portrait = "costumes/plk_swim.dds"  # optional: status page portrait
//   panel_portrait, face_portrait, small_face_portrait  # optional: camp art
//   base = "ps3/2"                 # optional: the costume tints start from
//
//   [[costume.tint]]               # optional, any number: model colours
//   from = "#BD6952"               # the colour to replace
//   to = "#517EBD"
//   textures = ["skirt.tga", 4]    # optional: names or indices, else all
//   hue_range = 20                 # optional: degrees either side of from
//   min_saturation = 0.4           # optional, 0..1
//   max_saturation, min_value, max_value  # optional, 0..1
//
//   [[costume.portrait_tint]]      # the same for the portraits, with
//   kinds = ["status", "face"]     # optional: which, else all four
//
// Hand-parsed, like the asset system's [[language]] tables.
struct DeclaredTint {
  std::string from, to, hue_range;
  std::map<std::string, std::string> bounds;  // min_saturation .. max_value
  std::vector<std::pair<std::string, bool>> textures, kinds;  // item, quoted
};

struct DeclaredCostume {
  std::string character, name, label, model, base;
  std::array<std::string, kPortraitKinds> portraits;
  bool locked = false;
  std::vector<DeclaredTint> tints, portrait_tints;
};

// The items of a one line array, each with whether it was quoted.
std::vector<std::pair<std::string, bool>> ParseArray(std::string_view text) {
  std::vector<std::pair<std::string, bool>> items;
  if (text.size() < 2 || text.front() != '[' || text.back() != ']')
    return items;
  text = text.substr(1, text.size() - 2);
  while (!text.empty()) {
    const size_t comma = text.find(',');
    std::string_view item = text.substr(0, comma);
    text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
    const size_t first = item.find_first_not_of(" \t");
    if (first == std::string_view::npos)
      continue;
    item = item.substr(first, item.find_last_not_of(" \t") - first + 1);
    const bool quoted = item.size() >= 2 && item.front() == '"' && item.back() == '"';
    items.emplace_back(std::string(quoted ? item.substr(1, item.size() - 2) : item), quoted);
  }
  return items;
}

std::vector<DeclaredCostume> ReadDeclaredCostumes(const std::filesystem::path& path) {
  std::vector<DeclaredCostume> costumes;
  std::ifstream in(path);
  if (!in)
    return costumes;
  std::string line;
  bool inside = false;
  DeclaredTint* tint = nullptr;  // the [[costume.*tint]] being read
  while (std::getline(in, line)) {
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
      if (line[i] == '"')
        quoted = !quoted;
      else if (line[i] == '#' && !quoted) {
        line.resize(i);
        break;
      }
    }
    const size_t first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos)
      continue;
    line = line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
    if (line.front() == '[') {
      tint = nullptr;
      if (line == "[[costume]]") {
        costumes.emplace_back();
      } else if (inside && line == "[[costume.tint]]") {
        tint = &costumes.back().tints.emplace_back();
        continue;
      } else if (inside && line == "[[costume.portrait_tint]]") {
        tint = &costumes.back().portrait_tints.emplace_back();
        continue;
      }
      inside = line == "[[costume]]";
      continue;
    }
    const size_t eq = line.find('=');
    if (!inside || eq == std::string::npos)
      continue;
    std::string key = line.substr(0, eq);
    std::string value = line.substr(eq + 1);
    key.erase(key.find_last_not_of(" \t") + 1);
    const size_t vstart = value.find_first_not_of(" \t");
    value = vstart == std::string::npos ? std::string() : value.substr(vstart);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
      value = value.substr(1, value.size() - 2);
    if (tint) {
      if (key == "from")
        tint->from = value;
      else if (key == "to")
        tint->to = value;
      else if (key == "hue_range")
        tint->hue_range = value;
      else if (key == "min_saturation" || key == "max_saturation" || key == "min_value" ||
               key == "max_value")
        tint->bounds[key] = value;
      else if (key == "textures")
        tint->textures = ParseArray(value);
      else if (key == "kinds")
        tint->kinds = ParseArray(value);
      continue;
    }
    DeclaredCostume& costume = costumes.back();
    if (key == "character")
      costume.character = value;
    else if (key == "name")
      costume.name = value;
    else if (key == "label")
      costume.label = value;
    else if (key == "model")
      costume.model = value;
    else if (key == "base")
      costume.base = value;
    else if (key == "portrait")
      costume.portraits[ETERNALSONATA_COSTUME_PORTRAIT_STATUS] = value;
    else if (key == "panel_portrait")
      costume.portraits[ETERNALSONATA_COSTUME_PORTRAIT_PANEL] = value;
    else if (key == "face_portrait")
      costume.portraits[ETERNALSONATA_COSTUME_PORTRAIT_FACE] = value;
    else if (key == "small_face_portrait")
      costume.portraits[ETERNALSONATA_COSTUME_PORTRAIT_SMALL_FACE] = value;
    else if (key == "locked")
      costume.locked = value == "true";
  }
  return costumes;
}

// "#RRGGBB" or "RRGGBB".
bool ParseColour(std::string_view text, uint32_t& rgb) {
  if (!text.empty() && text.front() == '#')
    text.remove_prefix(1);
  if (text.size() != 6)
    return false;
  char* end = nullptr;
  const std::string digits(text);
  rgb = static_cast<uint32_t>(std::strtoul(digits.c_str(), &end, 16));
  return end == digits.c_str() + 6;
}

bool ParseNumber(const std::string& text, float& out) {
  char* end = nullptr;
  out = std::strtof(text.c_str(), &end);
  return !text.empty() && end == text.c_str() + text.size();
}

// A declared rule, or an empty optional with `error` saying what is wrong.
std::optional<eternalsonata::TintRule> ParseTint(const DeclaredTint& declared,
                                                 std::string& error) {
  eternalsonata::TintRule rule;
  if (!ParseColour(declared.from, rule.from) || !ParseColour(declared.to, rule.to)) {
    error = "needs from and to as #RRGGBB";
    return std::nullopt;
  }
  float number = 0;
  if (!declared.hue_range.empty()) {
    if (!ParseNumber(declared.hue_range, number) || number < 0 || number > 180) {
      error = "has a hue_range that is not 0..180 degrees";
      return std::nullopt;
    }
    rule.hue_range = number / 360.f;
  }
  const std::pair<const char*, float*> bounds[] = {
      {"min_saturation", &rule.min_saturation},
      {"max_saturation", &rule.max_saturation},
      {"min_value", &rule.min_value},
      {"max_value", &rule.max_value}};
  for (const auto& [key, field] : bounds) {
    const auto it = declared.bounds.find(key);
    if (it == declared.bounds.end())
      continue;
    if (!ParseNumber(it->second, number) || number < 0 || number > 1) {
      error = std::string("has a ") + key + " that is not 0..1";
      return std::nullopt;
    }
    *field = number;
  }
  for (const auto& [item, quoted] : declared.textures) {
    char* end = nullptr;
    const long index = std::strtol(item.c_str(), &end, 10);
    if (quoted)
      rule.names.push_back(item);
    else if (!item.empty() && end == item.c_str() + item.size() && index >= 0)
      rule.indices.push_back(static_cast<int>(index));
    else {
      error = "names a texture '" + item + "' that is neither a quoted name nor an index";
      return std::nullopt;
    }
  }
  if (!declared.kinds.empty()) {
    constexpr std::string_view kKinds[kPortraitKinds] = {"status", "panel", "face",
                                                         "small_face"};
    rule.kinds = 0;
    for (const auto& [item, quoted] : declared.kinds) {
      const auto it = std::find(std::begin(kKinds), std::end(kKinds), item);
      if (it == std::end(kKinds)) {
        error = "names a portrait kind '" + item + "'";
        return std::nullopt;
      }
      rule.kinds |= 1u << (it - std::begin(kKinds));
    }
  }
  return rule;
}

void ScanModCostumes(rex::Runtime* runtime) {
  for (const auto& mod : runtime->EnabledModsInfo()) {
    for (const auto& declared : ReadDeclaredCostumes(mod.mod_root / "assets.toml")) {
      const int character = CharacterFromName(declared.character);
      std::filesystem::path model;
      if (!declared.model.empty())
        model = mod.mod_root / std::filesystem::u8path(declared.model);
      std::error_code ec;
      if (declared.character.empty() || declared.name.empty() ||
          (!model.empty() && !std::filesystem::is_regular_file(model, ec))) {
        REXLOG_WARN("costumes: mod '{}' declares a [[costume]] without a character, a name or "
                    "a model file it ships ({})",
                    mod.folder_name, declared.model);
        continue;
      }
      Costume costume;
      costume.path = model;
      for (int kind = 0; kind < kPortraitKinds; ++kind)
        if (!declared.portraits[kind].empty())
          costume.portrait_path[kind] =
              mod.mod_root / std::filesystem::u8path(declared.portraits[kind]);
      costume.starts_unlocked = costume.unlocked = !declared.locked;
      bool valid = true;
      for (const auto* list : {&declared.tints, &declared.portrait_tints}) {
        for (const DeclaredTint& tint : *list) {
          std::string error;
          if (auto rule = ParseTint(tint, error))
            (list == &declared.tints ? costume.model_tints : costume.portrait_tints)
                .push_back(std::move(*rule));
          else {
            REXLOG_WARN("costumes: a tint of mod '{}' costume '{}' {}", mod.folder_name,
                        declared.name, error);
            valid = false;
          }
        }
      }
      if (!valid)
        continue;
      costume.base = declared.base;
      if (costume.base.empty() && (!costume.model_tints.empty() || !costume.portrait_tints.empty()))
        costume.base = "default";
      const std::string id = mod.folder_name + "/" + declared.name;
      const std::string label = declared.label.empty() ? declared.name : declared.label;
      if (!character) {
        // Crescendo and Serenade on 360 data stay here for good.
        std::lock_guard lock(g_mutex);
        state().pending.push_back({declared.character, id, label, std::move(costume)});
        continue;
      }
      std::lock_guard lock(g_mutex);
      const std::string base = costume.base;
      const int result = RegisterLocked(character, id.c_str(), label.c_str(), std::move(costume));
      if (result == ETERNALSONATA_COSTUME_ERR_INVALID_COSTUME)
        REXLOG_INFO("costumes: mod '{}' costume '{}' starts from '{}', which this game data "
                    "does not have; skipped",
                    mod.folder_name, id, base);
      else if (result < 0)
        REXLOG_WARN("costumes: mod '{}' declares costume '{}' twice", mod.folder_name, id);
    }
  }
}

}  // namespace

namespace eternalsonata {

void BindCostumeSystem(rex::Runtime* runtime) {
  {
    std::lock_guard lock(g_mutex);
    state().runtime = runtime;
    EnsureCharacterLists();
  }
  ScanModCostumes(runtime);
}

void CostumeCharacterDefined(int character) {
  std::lock_guard lock(g_mutex);
  EnsureCharacterLists();
  State& s = state();
  const int c = character - 1;
  // A new occupant starts from its own default; costumes of the last one go.
  auto& list = s.costumes[c];
  for (const Costume& costume : list)
    if (costume.guest)
      s.retired.push_back(costume.guest);
  list.clear();
  list.push_back({"default", "Default"});
  s.worn[c] = s.boot[c] = ETERNALSONATA_COSTUME_DEFAULT;
  // The new occupant's costumes may reuse the old one's ids.
  for (auto& entry : s.portraits)
    if (entry.character == character)
      entry.costume.clear();
  ModdedCharacter definition;
  if (ModdedDefinition(character, definition)) {
    list.front().path = definition.model;
    for (int kind = 0; kind < kPortraitKinds; ++kind)
      list.front().portrait_path[kind] = definition.portraits[kind];
    for (auto it = s.pending.begin(); it != s.pending.end();) {
      if (CharacterFromToken(it->character) != character) {
        ++it;
        continue;
      }
      RegisterLocked(character, it->id.c_str(), it->label.c_str(), std::move(it->costume));
      it = s.pending.erase(it);
    }
  }
  if (s.live)
    SetupAddedSlot(character);
}

int CostumeCount(int character) {
  if (!ValidCharacter(character))
    return ETERNALSONATA_COSTUME_ERR_INVALID_CHARACTER;
  std::lock_guard lock(g_mutex);
  EnsureCharacterLists();
  return static_cast<int>(state().costumes[character - 1].size());
}

const char* CostumeLabel(int character, int costume) {
  if (!ValidCharacter(character))
    return "";
  std::lock_guard lock(g_mutex);
  EnsureCharacterLists();
  const auto& list = state().costumes[character - 1];
  return costume >= 0 && costume < static_cast<int>(list.size()) ? list[costume].label.c_str()
                                                                  : "";
}

int WornCostume(int character) {
  if (!ValidCharacter(character))
    return ETERNALSONATA_COSTUME_ERR_INVALID_CHARACTER;
  std::lock_guard lock(g_mutex);
  return state().worn[character - 1];
}

// The unlocked costume after `from`, wrapping, skipping models known not to
// load; `from` itself when there is none.
int NextUnlockedAfter(int character, int from) {
  const auto& list = state().costumes[character - 1];
  const int count = static_cast<int>(list.size());
  for (int k = 1; k < count; ++k) {
    const int index = (from + k) % count;
    if (list[index].unlocked && !list[index].failed)
      return index;
  }
  return from;
}

int NextUnlockedCostume(int character) {
  if (!ValidCharacter(character))
    return ETERNALSONATA_COSTUME_ERR_INVALID_CHARACTER;
  std::lock_guard lock(g_mutex);
  EnsureCharacterLists();
  return NextUnlockedAfter(character, state().worn[character - 1]);
}

int WearNextCostume(int character) {
  if (!ValidCharacter(character))
    return ETERNALSONATA_COSTUME_ERR_INVALID_CHARACTER;
  int worn, count;
  {
    std::lock_guard lock(g_mutex);
    EnsureCharacterLists();
    worn = state().worn[character - 1];
    count = static_cast<int>(state().costumes[character - 1].size());
  }
  // A costume that does not go on is passed over; the default always does,
  // so the walk ends there at the latest.
  for (int k = 1; k < count; ++k) {
    const int index = (worn + k) % count;
    {
      std::lock_guard lock(g_mutex);
      const auto& list = state().costumes[character - 1];
      if (index >= static_cast<int>(list.size()) || !list[index].unlocked || list[index].failed)
        continue;
    }
    if (WearCostume(character, index) >= 0)
      return index;
    REXLOG_WARN("costumes: {} cannot wear costume {}, trying the next", Token(character), index);
  }
  return ETERNALSONATA_COSTUME_ERR_INVALID_COSTUME;
}

uint32_t CostumePortrait(int character, int kind) {
  if (!ValidCharacter(character) || kind < 0 || kind >= kPortraitKinds)
    return 0;
  std::lock_guard lock(g_mutex);
  EnsureCharacterLists();
  State& s = state();
  if (!s.live)
    return 0;
  return PortraitImage(character, s.costumes[character - 1][s.worn[character - 1]], kind);
}

uint32_t CostumeModel(int character) {
  if (!ValidCharacter(character))
    return 0;
  std::lock_guard lock(g_mutex);
  return state().models[character - 1];
}

int SetCostumePortrait(int character, int costume, int kind, const char* path) {
  if (!ValidCharacter(character))
    return ETERNALSONATA_COSTUME_ERR_INVALID_CHARACTER;
  if (!path || !*path || kind < 0 || kind >= kPortraitKinds)
    return ETERNALSONATA_COSTUME_ERR_INVALID_ARGUMENT;
  std::filesystem::path file = std::filesystem::u8path(path);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(file, ec))
    return ETERNALSONATA_COSTUME_ERR_INVALID_MODEL;
  std::lock_guard lock(g_mutex);
  EnsureCharacterLists();
  auto& list = state().costumes[character - 1];
  if (costume <= ETERNALSONATA_COSTUME_DEFAULT || costume >= static_cast<int>(list.size()))
    return ETERNALSONATA_COSTUME_ERR_INVALID_COSTUME;
  if (list[costume].portrait[kind] || !list[costume].portrait_path[kind].empty())
    return ETERNALSONATA_COSTUME_ERR_INVALID_ARGUMENT;
  list[costume].portrait_path[kind] = std::move(file);
  return ETERNALSONATA_COSTUME_OK;
}

int WearCostume(int character, int costume) {
  if (!ValidCharacter(character))
    return ETERNALSONATA_COSTUME_ERR_INVALID_CHARACTER;
  bool changed = false;
  {
    std::lock_guard lock(g_mutex);
    EnsureCharacterLists();
    if (costume < 0 || costume >= static_cast<int>(state().costumes[character - 1].size()))
      return ETERNALSONATA_COSTUME_ERR_INVALID_COSTUME;
    changed = state().worn[character - 1] != costume;
    const int result = WearLocked(character, costume);
    if (result < 0)
      return result;
  }
  if (changed) {
    FieldPlayerModelOverride::RequestRespawn();
    Publish(ETERNALSONATA_COSTUME_EVENT_CHANGED, character, costume);
  }
  return ETERNALSONATA_COSTUME_OK;
}

int CostumeUnlocked(int character, int costume) {
  if (!ValidCharacter(character))
    return ETERNALSONATA_COSTUME_ERR_INVALID_CHARACTER;
  std::lock_guard lock(g_mutex);
  EnsureCharacterLists();
  const auto& list = state().costumes[character - 1];
  if (costume < 0 || costume >= static_cast<int>(list.size()))
    return ETERNALSONATA_COSTUME_ERR_INVALID_COSTUME;
  return list[costume].unlocked ? 1 : 0;
}

int SetCostumeUnlocked(int character, int costume, bool unlocked) {
  if (!ValidCharacter(character))
    return ETERNALSONATA_COSTUME_ERR_INVALID_CHARACTER;
  bool changed = false, worn = false;
  {
    std::lock_guard lock(g_mutex);
    EnsureCharacterLists();
    auto& list = state().costumes[character - 1];
    if (costume < 0 || costume >= static_cast<int>(list.size()))
      return ETERNALSONATA_COSTUME_ERR_INVALID_COSTUME;
    if (costume == ETERNALSONATA_COSTUME_DEFAULT)
      return unlocked ? ETERNALSONATA_COSTUME_OK : ETERNALSONATA_COSTUME_ERR_DEFAULT_LOCK;
    changed = list[costume].unlocked != unlocked;
    list[costume].unlocked = unlocked;
    worn = state().worn[character - 1] == costume;
  }
  if (!unlocked && worn)
    WearCostume(character, ETERNALSONATA_COSTUME_DEFAULT);
  if (changed) {
    REXLOG_INFO("costumes: {} {} costume {}", unlocked ? "unlocked" : "locked",
                Token(character), costume);
    Publish(unlocked ? ETERNALSONATA_COSTUME_EVENT_UNLOCKED : ETERNALSONATA_COSTUME_EVENT_LOCKED,
            character, costume);
  }
  return ETERNALSONATA_COSTUME_OK;
}

int SetCostumeStartsLocked(int character, int costume, bool locked) {
  if (!ValidCharacter(character))
    return ETERNALSONATA_COSTUME_ERR_INVALID_CHARACTER;
  {
    std::lock_guard lock(g_mutex);
    EnsureCharacterLists();
    auto& list = state().costumes[character - 1];
    if (costume < 0 || costume >= static_cast<int>(list.size()))
      return ETERNALSONATA_COSTUME_ERR_INVALID_COSTUME;
    if (costume == ETERNALSONATA_COSTUME_DEFAULT && locked)
      return ETERNALSONATA_COSTUME_ERR_DEFAULT_LOCK;
    list[costume].starts_unlocked = !locked;
  }
  return SetCostumeUnlocked(character, costume, !locked);
}

void ResetCostumeRecord() {
  std::array<int, kCharacters> boot;
  {
    std::lock_guard lock(g_mutex);
    EnsureCharacterLists();
    for (auto& list : state().costumes) {
      for (auto& costume : list)
        costume.unlocked = costume.starts_unlocked;
    }
    boot = state().boot;
  }
  for (int c = 1; c <= kCharacters; ++c)
    if (ValidCharacter(c))
      WearCostume(c, boot[c - 1]);
}

// Keys are "costume.<character>" for the costume worn and
// "unlocked.<character>.<id>" for a lock that differs from a new game's.
void SaveCostumeRecord(SaveRecord& record) {
  std::lock_guard lock(g_mutex);
  EnsureCharacterLists();
  const State& s = state();
  for (int c = 0; c < kCharacters; ++c) {
    const std::string name = Token(c + 1);
    if (name.empty())
      continue;
    const auto& list = s.costumes[c];
    if (s.worn[c] != ETERNALSONATA_COSTUME_DEFAULT)
      record["costume." + name] = list[s.worn[c]].id;
    for (const auto& costume : list) {
      if (costume.unlocked != costume.starts_unlocked)
        record["unlocked." + name + "." + costume.id] = costume.unlocked ? "1" : "0";
    }
  }
}

// A costume the record names that is not registered any more (its mod is
// gone) is skipped, and leaves the character in the default.
void LoadCostumeRecord(const SaveRecord& record) {
  {
    std::lock_guard lock(g_mutex);
    EnsureCharacterLists();
    for (int c = 0; c < kCharacters; ++c) {
      const std::string prefix = "unlocked." + Token(c + 1) + ".";
      for (auto& costume : state().costumes[c]) {
        const auto it = record.find(prefix + costume.id);
        costume.unlocked = it != record.end() ? it->second == "1" : costume.starts_unlocked;
      }
    }
  }
  for (int c = 1; c <= kCharacters; ++c) {
    if (!ValidCharacter(c))
      continue;
    int index = ETERNALSONATA_COSTUME_DEFAULT;
    const auto it = record.find("costume." + Token(c));
    if (it != record.end()) {
      std::lock_guard lock(g_mutex);
      index = FindLocked(c, it->second);
      if (index < 0) {
        REXLOG_WARN("costumes: the save wears '{}', which is not installed", it->second);
        index = ETERNALSONATA_COSTUME_DEFAULT;
      }
    }
    WearCostume(c, index);
  }
}

// The PS3's costumes are "ps3/<variant>"; variant 1 is the default.
int Ps3CostumeIndex(int32_t character, int32_t variant) {
  if (character < 0 || character > 2)
    return ETERNALSONATA_COSTUME_ERR_INVALID_CHARACTER;
  if (variant == 1)
    return ETERNALSONATA_COSTUME_DEFAULT;
  std::lock_guard lock(g_mutex);
  EnsureCharacterLists();
  return FindLocked(character + 1, "ps3/" + std::to_string(variant));
}

bool Ps3CostumeUnlocked(int32_t character, int32_t variant) {
  const int costume = Ps3CostumeIndex(character, variant);
  return costume >= 0 && CostumeUnlocked(character + 1, costume) == 1;
}

void Ps3UnlockCostume(int32_t character, int32_t variant) {
  const int costume = Ps3CostumeIndex(character, variant);
  if (costume > 0)
    SetCostumeUnlocked(character + 1, costume, true);
}

int32_t Ps3WornCostume(int32_t character) {
  if (character < 0 || character > 2)
    return 1;
  std::lock_guard lock(g_mutex);
  EnsureCharacterLists();
  const auto& list = state().costumes[character];
  const std::string& id = list[state().worn[character]].id;
  return id.rfind("ps3/", 0) == 0 ? std::atoi(id.c_str() + 4) : 1;
}

}  // namespace eternalsonata

// sub_82162058 loads AppKeep.bmd into the APPKEEP heap at boot and fills the
// model table (on PS3 data, from the PS3's files), so it is where the
// costumes chosen so far go on.
REX_EXTERN(__imp__sub_82162058);

REX_HOOK_RAW(sub_82162058) {
  __imp__sub_82162058(ctx, base);
  if (eternalsonata::IsPs3Target())
    eternalsonata::BuildPs3AppKeep(ctx, base);
  button_glyphs::OnAppKeepLoaded();
  std::lock_guard lock(g_mutex);
  EnsureCharacterLists();
  State& s = state();
  auto* memory = s.runtime ? s.runtime->memory() : rex::Runtime::instance()->memory();
  for (int c = 0; c < kRetailCharacters; ++c) {
    s.defaults[c] = REX_LOAD_U32(kModelTableAddr + 4 * kModelSlot[c]);
    s.models[c] = s.defaults[c];
    for (int set = 0; set < 2; ++set)
      s.default_faces[set][c] = LoadU16(
          memory, eternalsonata::PartyArrayAddress(kFaceArrays[set], static_cast<uint32_t>(c)));
  }
  s.live = true;
  for (int c = kRetailCharacters + 1; c <= kCharacters; ++c)
    SetupAddedSlot(c);
  for (int c = 1; c <= kCharacters; ++c)
    if (ValidCharacter(c))
      SnapshotTintSources(c);
  WearBootCostumes();
  for (int c = 1; c <= kCharacters; ++c) {
    if (!ValidCharacter(c))
      continue;
    const int index = s.worn[c - 1];
    if (index != ETERNALSONATA_COSTUME_DEFAULT && WearLocked(c, index) < 0)
      s.worn[c - 1] = ETERNALSONATA_COSTUME_DEFAULT;
  }
  s.boot = s.worn;
}

// sub_82176398(list, model, desc) makes every scene object, from a raw model
// or none, and answers its handle or -1; sub_82175620(element) tears one down,
// handle at +0. Together they count the objects reading each costume model.
REX_EXTERN(__imp__sub_82176398);

REX_HOOK_RAW(sub_82176398) {
  const uint32_t model = ctx.r4.u32;
  __imp__sub_82176398(ctx, base);
  const uint32_t handle = ctx.r3.u32;
  if (!model || handle == 0xFFFFFFFFu)
    return;
  std::lock_guard lock(g_ref_mutex);
  const auto it = g_model_refs.find(model);
  if (it == g_model_refs.end())
    return;
  ++it->second.objects;
  it->second.built = true;
  g_object_model[handle] = model;
}

REX_EXTERN(__imp__sub_82175620);

REX_HOOK_RAW(sub_82175620) {
  const uint32_t handle = REX_LOAD_U32(ctx.r3.u32);
  {
    std::lock_guard lock(g_ref_mutex);
    const auto it = g_object_model.find(handle);
    if (it != g_object_model.end()) {
      const auto refs = g_model_refs.find(it->second);
      if (refs != g_model_refs.end() && --refs->second.objects == 0)
        refs->second.idle_since = std::chrono::steady_clock::now();
      g_object_model.erase(it);
    }
  }
  __imp__sub_82175620(ctx, base);
}

// ---------------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------------

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t EternalSonataCostumeAbiVersion(void) {
  return ETERNALSONATA_COSTUME_ABI_VERSION;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetCostumeCount(int character) {
  return eternalsonata::CostumeCount(character);
}

extern "C" REX_MOD_PLUGIN_EXPORT const char* EternalSonataGetCostumeId(int character,
                                                                       int costume) {
  if (!ValidCharacter(character))
    return "";
  std::lock_guard lock(g_mutex);
  EnsureCharacterLists();
  const auto& list = state().costumes[character - 1];
  return costume >= 0 && costume < static_cast<int>(list.size()) ? list[costume].id.c_str() : "";
}

extern "C" REX_MOD_PLUGIN_EXPORT const char* EternalSonataGetCostumeLabel(int character,
                                                                          int costume) {
  return eternalsonata::CostumeLabel(character, costume);
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataFindCostume(int character, const char* id) {
  if (!ValidCharacter(character))
    return ETERNALSONATA_COSTUME_ERR_INVALID_CHARACTER;
  if (!id)
    return ETERNALSONATA_COSTUME_ERR_INVALID_ARGUMENT;
  std::lock_guard lock(g_mutex);
  EnsureCharacterLists();
  return FindLocked(character, id);
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetWornCostume(int character) {
  return eternalsonata::WornCostume(character);
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataWearCostume(int character, int costume) {
  return eternalsonata::WearCostume(character, costume);
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataIsCostumeUnlocked(int character, int costume) {
  return eternalsonata::CostumeUnlocked(character, costume);
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataUnlockCostume(int character, int costume) {
  return eternalsonata::SetCostumeUnlocked(character, costume, true);
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataLockCostume(int character, int costume) {
  return eternalsonata::SetCostumeUnlocked(character, costume, false);
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSetCostumeStartsLocked(int character,
                                                                         int costume, int locked) {
  return eternalsonata::SetCostumeStartsLocked(character, costume, locked != 0);
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataRegisterCostume(int character, const char* id,
                                                                  const char* label,
                                                                  const uint8_t* model,
                                                                  uint32_t size) {
  if (model && !ValidModel(model, size))
    return ETERNALSONATA_COSTUME_ERR_INVALID_MODEL;
  Costume costume;
  if (model)
    costume.model.assign(model, model + size);
  std::lock_guard lock(g_mutex);
  return RegisterLocked(character, id, label, std::move(costume));
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataRegisterCostumeFile(int character,
                                                                      const char* id,
                                                                      const char* label,
                                                                      const char* path) {
  Costume costume;
  if (path && *path) {
    costume.path = std::filesystem::u8path(path);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(costume.path, ec))
      return ETERNALSONATA_COSTUME_ERR_INVALID_MODEL;
  }
  std::lock_guard lock(g_mutex);
  return RegisterLocked(character, id, label, std::move(costume));
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSetCostumePortraitFile(int character,
                                                                         int costume, int kind,
                                                                         const char* path) {
  return eternalsonata::SetCostumePortrait(character, costume, kind, path);
}
