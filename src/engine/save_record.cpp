// eternalsonata - Host state kept with each save slot (save_record.h).
//
// The record is taken when a save starts, so it matches the guest state the
// save gathers, and written once the content worker has created the
// container. Loading restores it right after sub_82240AF8; the title screen
// resets it, so a new game starts from the defaults.

#include "save_record.h"

#include "character_roster.h"
#include "costume_system.h"
#include "party_arrays.h"
#include "generated/eternalsonata_init.h"
#include "ps3_natives.h"
#include "save_system.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>

namespace {

constexpr char kFileName[] = "reprise.txt";

// sub_822404E8 points the controller at its slot's 32 byte entry, which
// starts at +2936, before it reads the save.
constexpr uint32_t kSlotEntryField = 2571324u;
constexpr uint32_t kSlotEntries = 2936u;
constexpr uint32_t kSlotEntrySize = 32u;
constexpr int kSlots = 10;

// The title screen's controller, created by sub_8223E940 when it is null.
constexpr uint32_t kTitleAddr = 0x824409ECu;

std::mutex g_mutex;
eternalsonata::SaveRecord g_captured;

uint32_t LoadGuestU32(uint32_t address) {
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  return memory ? rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(address)) : 0;
}

void Reset() {
  eternalsonata::ResetPs3Record();
  eternalsonata::ResetCostumeRecord();
}

bool Read(const std::filesystem::path& path, eternalsonata::SaveRecord& record) {
  std::ifstream in(path);
  if (!in)
    return false;
  std::string line;
  while (std::getline(in, line)) {
    const size_t eq = line.find(" = ");
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (eq != std::string::npos && line[0] != '#')
      record[line.substr(0, eq)] = line.substr(eq + 3);
  }
  return true;
}

}  // namespace

namespace eternalsonata {

void CaptureSaveRecord() {
  SaveRecord record;
  record["version"] = "1";
  SaveCostumeRecord(record);
  SavePs3Record(record);
  SavePartyRecord(record);
  SaveRosterRecord(record);
  std::lock_guard lock(g_mutex);
  g_captured = std::move(record);
}

void CommitSaveRecord(int slot) {
  SaveRecord record;
  {
    std::lock_guard lock(g_mutex);
    record = g_captured;
  }
  const std::filesystem::path dir = SaveContainerDirectory(slot);
  std::error_code ec;
  if (dir.empty() || !std::filesystem::is_directory(dir, ec)) {
    REXLOG_ERROR("save record: no container for slot {}", slot);
    return;
  }
  // Written aside and renamed, so a crash mid write keeps the old record.
  const std::filesystem::path path = dir / kFileName;
  std::filesystem::path temp = path;
  temp += ".tmp";
  {
    std::ofstream out(temp, std::ios::trunc);
    for (const auto& [key, value] : record)
      out << key << " = " << value << '\n';
    if (!out) {
      REXLOG_ERROR("save record: cannot write {}", temp.string());
      return;
    }
  }
  std::filesystem::rename(temp, path, ec);
  if (ec)
    REXLOG_ERROR("save record: cannot replace {}: {}", path.string(), ec.message());
  else
    REXLOG_INFO("save record: slot {} saved, {} entries", slot, record.size());
}

void NotifySaveRecordLoaded(uint32_t save) {
  const uint32_t entry = LoadGuestU32(save + kSlotEntryField);
  const int slot = entry ? static_cast<int>((entry - save - kSlotEntries) / kSlotEntrySize) : -1;
  SaveRecord record;
  if (slot >= 0 && slot < kSlots)
    Read(SaveContainerDirectory(slot) / kFileName, record);
  else
    REXLOG_WARN("save record: loaded a save from no known slot");
  // Anything the record lacks takes its new game value.
  ResetPs3Record();
  LoadPs3Record(record);
  LoadCostumeRecord(record);
  LoadPartyRecord(record);
  LoadRosterRecord(record);
  REXLOG_INFO("save record: slot {} loaded, {} entries", slot, record.size());
}

}  // namespace eternalsonata

// sub_8223E940 builds the title screen when there is none, which every new
// game and load goes through.
REX_EXTERN(__imp__sub_8223E940);

REX_HOOK_RAW(sub_8223E940) {
  const bool building = REX_LOAD_U32(kTitleAddr) == 0;
  __imp__sub_8223E940(ctx, base);
  if (building)
    Reset();
}
