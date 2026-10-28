#pragma once

// The game's ten save slots on disk, for the start screen's Saves page, which
// runs before a Runtime exists. Imports fill an empty slot from a slot export, a 360 container's
// savedata.txt (any region) or a PS3 SAVEDATA (any region; a console's
// encrypted one also needs its PARAM.PFD); the
// layouts are in scripts/convert_save.py, which this ports.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace eternalsonata {

inline constexpr int kSaveSlotCount = 10;
inline constexpr int kSaveThumbWidth = 320;
inline constexpr int kSaveThumbHeight = 160;

struct SaveSlotInfo {
  bool used = false;
  // The data the save was made on, "360" or "ps3".
  std::string target;
  uint32_t play_seconds = 0;
  // From the slot header, as the load menu reads them: the save point's
  // place name id, the chapter (0 is the prologue), and whether it earns the
  // reward icon (a Game Clear save, or any save of an Encore run).
  int location = 0;
  int chapter = 0;
  bool beaten = false;
  // Local time of the last write, "YYYY/MM/DD HH:MM".
  std::string modified;
  // RGBA, kSaveThumbWidth x kSaveThumbHeight; empty when unreadable.
  std::vector<uint8_t> thumbnail;
};

// A save converted for a slot, held until it is written.
struct PreparedSave {
  std::vector<uint8_t> savedata;
  std::string record;
  // The data the save was made on, "360" or "ps3".
  std::string kind;
  uint32_t play_seconds = 0;
  // An encrypted PS3 SAVEDATA with no PARAM.PFD beside it, held in `savedata`
  // until FinishEncryptedImport gets one.
  bool needs_pfd = false;
};

std::vector<SaveSlotInfo> ReadSaveSlots(const std::filesystem::path& user_data_root);

// Each returns an error message, empty on success.
std::string ExportSaveSlot(const std::filesystem::path& user_data_root, int slot,
                           const std::filesystem::path& archive);
std::string PrepareSaveImport(const std::filesystem::path& user_data_root,
                              const std::filesystem::path& source, PreparedSave& out);
std::string FinishEncryptedImport(const std::filesystem::path& pfd, PreparedSave& out);
std::string WriteSaveSlot(const std::filesystem::path& user_data_root, int slot,
                          const PreparedSave& save);
std::string DeleteSaveSlot(const std::filesystem::path& user_data_root, int slot);

// "eternalsonata-save-01.zip" for slot 0.
std::string SaveSlotExportName(int slot);

// "06:34:59".
std::string FormatPlayTime(uint32_t seconds);

// The load menu's text in the start screen's language; empty when unknown.
std::string SaveLocationName(int location);
std::string SaveChapterName(int chapter);

// Every language's chapter and place names, for baking the start screen's glyphs.
std::string SaveTextGlyphs();

}  // namespace eternalsonata
