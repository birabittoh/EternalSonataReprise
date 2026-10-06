// eternalsonata - Character costumes (docs/costumes.md).
//
// The field, battle and event code all take a character's model from
// dword_82420AF8, a table of raw NOBJ pointers into AppKeep.bmd that
// sub_82162058 fills at boot, and build it in place. A costume is another
// NOBJ: wearing one points the character's slot at it, so every reader builds
// it the next time, as the PS3 does with its WEAR slots. The field leader is
// the one model that outlives a change, so it is respawned.
//
// Each costume's model loads into physical guest memory the first time it is
// worn and stays there: models already built read it in place, so a buffer is
// never reused for another costume.

#include "costume_system.h"

#include "field_player_model_override.h"
#include "generated/eternalsonata_init.h"
#include "party_arrays.h"
#include "ps3_appkeep.h"
#include "target.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
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
constexpr uint32_t kModelTableAddr = 0x82420AF8u;
constexpr int kPortraitKinds = ETERNALSONATA_COSTUME_PORTRAIT_KIND_COUNT;

// The camp's image ids per character, AppKeep slot plus one: the one to
// three member panel's (sub_821DDD00 reads it for characters 1..4 only) and
// the two face sets.
constexpr uint32_t kPanelTable = 0x8202C9B4u;
constexpr uint32_t kFaceTables[] = {0x8202CA28u, 0x8202CA3Cu};
constexpr eternalsonata::PartyArray kFaceArrays[] = {eternalsonata::PartyArray::kPortraitA,
                                                     eternalsonata::PartyArray::kPortraitB};
constexpr int kPanelCharacters = 4;

// dword_82420AF8 slot of each character. The table follows AppKeep.bmd, where
// March comes before Claves; the party numbering has them the other way.
constexpr std::array<uint32_t, kCharacters> kModelSlot = {1, 2, 3, 4, 5, 6, 7, 8, 10, 9};

constexpr std::array<const char*, kCharacters> kCharacterNames = {
    "allegretto", "polka", "beat", "frederic", "viola",
    "salsa",      "jazz",  "falsetto", "claves", "march",
};

// The NOBJ chunk header: tag and u32 size, the size including the header.
constexpr size_t kChunkHeader = 8;

struct Costume {
  std::string id, label;
  std::vector<uint8_t> model;  // owned copy, or empty when read from `path`
  std::filesystem::path path;
  uint32_t guest = 0;          // loaded model, 0 until first worn
  bool failed = false;         // did not load; not retried
  bool starts_unlocked = true; // in a new game
  bool unlocked = true;        // in the game in progress
  // Portraits by ETERNALSONATA_COSTUME_PORTRAIT_* kind: AppKeep image ids, or
  // .dds / NTEX files placed in free AppKeep slots the first time they show.
  std::array<uint32_t, kPortraitKinds> portrait{};
  std::array<std::filesystem::path, kPortraitKinds> portrait_path;
};

struct State {
  // Index 0 of each list is the default model. A deque keeps the strings
  // handed out through the C ABI at stable addresses.
  std::array<std::deque<Costume>, kCharacters> costumes;
  std::array<int, kCharacters> worn{};
  // What the costumes cvar put on, which a new game starts from.
  std::array<int, kCharacters> boot{};
  // The slots as AppKeep.bmd filled them; all 0 until the boot hook ran.
  std::array<uint32_t, kCharacters> defaults{};
  uint32_t next_portrait_slot = eternalsonata::kFirstFreeAppKeepSlot;
  // Characters whose camp images a costume replaced, so taking it off puts
  // the retail ones back.
  std::array<bool, kCharacters> camp_art{};
  bool live = false;
  bool builtins = false;
  rex::Runtime* runtime = nullptr;
};

std::mutex g_mutex;

State& state() {
  static State s;
  return s;
}

bool ValidCharacter(int character) {
  return character >= 1 && character <= kCharacters;
}

int CharacterFromName(std::string_view name) {
  for (int c = 0; c < kCharacters; ++c) {
    if (name == kCharacterNames[c])
      return c + 1;
  }
  const int number = std::atoi(std::string(name).c_str());
  return ValidCharacter(number) ? number : 0;
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
  costume.id = id;
  costume.label = *label ? label : id;
  auto& list = state().costumes[character - 1];
  list.push_back(std::move(costume));
  REXLOG_INFO("costumes: {} can wear '{}' ({})", kCharacterNames[character - 1], list.back().label,
              list.back().id);
  return static_cast<int>(list.size() - 1);
}

// The guest model a costume puts in the table, loading it the first time.
uint32_t GuestModel(int character, int index, int* error) {
  State& s = state();
  if (index == ETERNALSONATA_COSTUME_DEFAULT)
    return s.defaults[character - 1];
  Costume& costume = s.costumes[character - 1][index];
  if (costume.guest || costume.failed) {
    *error = ETERNALSONATA_COSTUME_ERR_INVALID_MODEL;
    return costume.guest;
  }
  std::vector<uint8_t> file;
  const std::vector<uint8_t>* model = &costume.model;
  if (model->empty()) {
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
  auto* memory = s.runtime ? s.runtime->memory() : rex::Runtime::instance()->memory();
  const uint32_t size = static_cast<uint32_t>(model->size());
  const uint32_t at = memory->SystemHeapAlloc(size, 0x1000, rex::memory::kSystemHeapPhysical);
  if (!at) {
    REXLOG_ERROR("costumes: no guest memory for '{}' ({} bytes)", costume.id, size);
    *error = ETERNALSONATA_COSTUME_ERR_NO_MEMORY;
    return 0;
  }
  std::memcpy(memory->TranslateVirtual(at), model->data(), size);
  costume.guest = at;
  costume.model.clear();
  costume.model.shrink_to_fit();
  REXLOG_INFO("costumes: '{}' loaded at {:08X}, {} bytes", costume.id, at, size);
  return at;
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

// The image id of a costume's portrait, placing its file the first time;
// 0 for the character's own.
uint32_t PortraitImage(Costume& costume, int kind) {
  State& s = state();
  if (costume.portrait[kind] || costume.portrait_path[kind].empty())
    return costume.portrait[kind];
  const std::filesystem::path path = std::move(costume.portrait_path[kind]);
  costume.portrait_path[kind].clear();
  std::vector<uint8_t> chunk;
  if (!ReadPortrait(path, chunk)) {
    REXLOG_ERROR("costumes: {} is not a .dds or NTEX portrait", path.string());
    return 0;
  }
  if (s.next_portrait_slot >= eternalsonata::kAppKeepSlotCount) {
    REXLOG_ERROR("costumes: no AppKeep slot left for the portrait of '{}'", costume.id);
    return 0;
  }
  auto* memory = s.runtime ? s.runtime->memory() : rex::Runtime::instance()->memory();
  const uint32_t size = static_cast<uint32_t>(chunk.size());
  const uint32_t at = memory->SystemHeapAlloc(size, 0x1000, rex::memory::kSystemHeapPhysical);
  if (!at) {
    REXLOG_ERROR("costumes: no guest memory for the portrait of '{}'", costume.id);
    return 0;
  }
  std::memcpy(memory->TranslateVirtual(at), chunk.data(), size);
  const uint32_t slot = s.next_portrait_slot++;
  rex::memory::store_and_swap<uint32_t>(
      memory->TranslateVirtual(eternalsonata::kAppKeepSlotArray + 4 * slot), at);
  costume.portrait[kind] = slot + 1;
  return costume.portrait[kind];
}

void StoreU16(rex::memory::Memory* memory, uint32_t address, uint16_t value) {
  rex::memory::store_and_swap<uint16_t>(memory->TranslateVirtual(address), value);
}

uint16_t LoadU16(rex::memory::Memory* memory, uint32_t address) {
  return rex::memory::load_and_swap<uint16_t>(memory->TranslateVirtual(address));
}

// Points the camp's tables at the worn costume's images, or back at the
// character's own. The panel table is .rdata; the face tables are read from
// their twelve wide copies, so the retail ones keep the defaults.
void ApplyCampPortraits(int character) {
  State& s = state();
  Costume& costume = s.costumes[character - 1][s.worn[character - 1]];
  std::array<uint32_t, kPortraitKinds> ids{};
  bool any = false;
  for (int kind = ETERNALSONATA_COSTUME_PORTRAIT_PANEL; kind < kPortraitKinds; ++kind)
    any |= (ids[kind] = PortraitImage(costume, kind)) != 0;
  if (!any && !s.camp_art[character - 1])
    return;
  s.camp_art[character - 1] = any;
  auto* memory = s.runtime ? s.runtime->memory() : rex::Runtime::instance()->memory();
  const uint32_t i = static_cast<uint32_t>(character - 1);
  if (character <= kPanelCharacters) {
    const uint32_t at = kPanelTable + 2 * i;
    uint32_t& panel = ids[ETERNALSONATA_COSTUME_PORTRAIT_PANEL];
    if (!panel)
      panel = 0xCE + i;
    auto* heap = memory->LookupHeap(at);
    uint32_t old_protect = 0;
    if (heap && heap->Protect(at, 2,
                              rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite,
                              &old_protect)) {
      StoreU16(memory, at, static_cast<uint16_t>(panel));
      heap->Protect(at, 2, old_protect, nullptr);
    }
  }
  for (int set = 0; set < 2; ++set) {
    const int kind = ETERNALSONATA_COSTUME_PORTRAIT_FACE + set;
    const uint32_t id = ids[kind] ? ids[kind] : LoadU16(memory, kFaceTables[set] + 2 * i);
    StoreU16(memory, eternalsonata::PartyArrayAddress(kFaceArrays[set], i),
             static_cast<uint16_t>(id));
  }
}

// Puts the costume in the table. Before the boot hook there is no table yet,
// so the choice is only recorded and applied there.
int WearLocked(int character, int index) {
  State& s = state();
  if (s.live) {
    int error = ETERNALSONATA_COSTUME_OK;
    const uint32_t model = GuestModel(character, index, &error);
    if (!model)
      return error;
    auto* memory = s.runtime ? s.runtime->memory() : rex::Runtime::instance()->memory();
    rex::memory::store_and_swap<uint32_t>(
        memory->TranslateVirtual(kModelTableAddr + 4 * kModelSlot[character - 1]), model);
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
//   character = "polka"            # or its number, 1..10
//   name = "swimsuit"              # id becomes "<mod folder>/swimsuit"
//   label = "Swimsuit"
//   model = "costumes/plk_swim.nobj"  # relative to the mod folder
//   locked = true                  # optional: a new game starts it locked
//   portrait = "costumes/plk_swim.dds"  # optional: status page portrait
//   panel_portrait, face_portrait, small_face_portrait  # optional: camp art
//
// Hand-parsed, like the asset system's [[language]] tables.
struct DeclaredCostume {
  std::string character, name, label, model;
  std::array<std::string, kPortraitKinds> portraits;
  bool locked = false;
};

std::vector<DeclaredCostume> ReadDeclaredCostumes(const std::filesystem::path& path) {
  std::vector<DeclaredCostume> costumes;
  std::ifstream in(path);
  if (!in)
    return costumes;
  std::string line;
  bool inside = false;
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
      inside = line == "[[costume]]";
      if (inside)
        costumes.emplace_back();
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
    DeclaredCostume& costume = costumes.back();
    if (key == "character")
      costume.character = value;
    else if (key == "name")
      costume.name = value;
    else if (key == "label")
      costume.label = value;
    else if (key == "model")
      costume.model = value;
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

void ScanModCostumes(rex::Runtime* runtime) {
  for (const auto& mod : runtime->EnabledModsInfo()) {
    for (const auto& declared : ReadDeclaredCostumes(mod.mod_root / "assets.toml")) {
      const int character = CharacterFromName(declared.character);
      const std::filesystem::path model = mod.mod_root / std::filesystem::u8path(declared.model);
      std::error_code ec;
      if (!character || declared.name.empty() || declared.model.empty() ||
          !std::filesystem::is_regular_file(model, ec)) {
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
      const std::string id = mod.folder_name + "/" + declared.name;
      const std::string label = declared.label.empty() ? declared.name : declared.label;
      std::lock_guard lock(g_mutex);
      if (RegisterLocked(character, id.c_str(), label.c_str(), std::move(costume)) < 0)
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

int NextUnlockedCostume(int character) {
  if (!ValidCharacter(character))
    return ETERNALSONATA_COSTUME_ERR_INVALID_CHARACTER;
  std::lock_guard lock(g_mutex);
  EnsureCharacterLists();
  const auto& list = state().costumes[character - 1];
  const int worn = state().worn[character - 1];
  const int count = static_cast<int>(list.size());
  for (int k = 1; k < count; ++k) {
    const int index = (worn + k) % count;
    if (list[index].unlocked)
      return index;
  }
  return worn;
}

uint32_t WornCostumePortrait(int character) {
  if (!ValidCharacter(character))
    return 0;
  std::lock_guard lock(g_mutex);
  EnsureCharacterLists();
  State& s = state();
  if (!s.live)
    return 0;
  return PortraitImage(s.costumes[character - 1][s.worn[character - 1]],
                       ETERNALSONATA_COSTUME_PORTRAIT_STATUS);
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
                kCharacterNames[character - 1], costume);
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
    WearCostume(c, boot[c - 1]);
}

// Keys are "costume.<character>" for the costume worn and
// "unlocked.<character>.<id>" for a lock that differs from a new game's.
void SaveCostumeRecord(SaveRecord& record) {
  std::lock_guard lock(g_mutex);
  EnsureCharacterLists();
  const State& s = state();
  for (int c = 0; c < kCharacters; ++c) {
    const std::string name = kCharacterNames[c];
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
      const std::string prefix = std::string("unlocked.") + kCharacterNames[c] + ".";
      for (auto& costume : state().costumes[c]) {
        const auto it = record.find(prefix + costume.id);
        costume.unlocked = it != record.end() ? it->second == "1" : costume.starts_unlocked;
      }
    }
  }
  for (int c = 1; c <= kCharacters; ++c) {
    int index = ETERNALSONATA_COSTUME_DEFAULT;
    const auto it = record.find(std::string("costume.") + kCharacterNames[c - 1]);
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
  std::lock_guard lock(g_mutex);
  EnsureCharacterLists();
  State& s = state();
  for (int c = 0; c < kCharacters; ++c)
    s.defaults[c] = REX_LOAD_U32(kModelTableAddr + 4 * kModelSlot[c]);
  s.live = true;
  WearBootCostumes();
  for (int c = 1; c <= kCharacters; ++c) {
    const int index = s.worn[c - 1];
    if (index != ETERNALSONATA_COSTUME_DEFAULT && WearLocked(c, index) < 0)
      s.worn[c - 1] = ETERNALSONATA_COSTUME_DEFAULT;
  }
  s.boot = s.worn;
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
  if (!model)
    return ETERNALSONATA_COSTUME_ERR_INVALID_ARGUMENT;
  if (!ValidModel(model, size))
    return ETERNALSONATA_COSTUME_ERR_INVALID_MODEL;
  Costume costume;
  costume.model.assign(model, model + size);
  std::lock_guard lock(g_mutex);
  return RegisterLocked(character, id, label, std::move(costume));
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataRegisterCostumeFile(int character,
                                                                      const char* id,
                                                                      const char* label,
                                                                      const char* path) {
  if (!path || !*path)
    return ETERNALSONATA_COSTUME_ERR_INVALID_ARGUMENT;
  Costume costume;
  costume.path = std::filesystem::u8path(path);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(costume.path, ec))
    return ETERNALSONATA_COSTUME_ERR_INVALID_MODEL;
  std::lock_guard lock(g_mutex);
  return RegisterLocked(character, id, label, std::move(costume));
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSetCostumePortraitFile(int character,
                                                                         int costume, int kind,
                                                                         const char* path) {
  return eternalsonata::SetCostumePortrait(character, costume, kind, path);
}
