// eternalsonata - ReXGlue Recompiled Project
//
// Container model behind the granular asset API: the index.vmtoc reader, the
// two codec layers, and the `BTX ` text section's parser and serialiser.
//
// This is a C++ port of tooling that is already correct and tested:
// scripts/unpack_e.c (codecs), scripts/btx.py (parse) and scripts/btx_edit.py
// (rebuild + relocation fixups). Formats are documented in
// docs/asset-formats.md sections 1, 2 and 3.

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace eternalsonata::assets {

// Lowercase, '/'-separated, no "game:\" prefix: the form every reference and
// every map key in this subsystem uses.
std::string NormalizeGuestPath(std::string_view path);

// ---------------------------------------------------------------------------
// index.vmtoc
// ---------------------------------------------------------------------------
struct TocEntry {
  std::string path;  // normalized
  uint32_t size = 0;
  uint8_t flag = 0;
};

class Toc {
 public:
  bool Load(const std::filesystem::path& file);
  bool loaded() const { return !raw_.empty(); }

  const TocEntry* Find(std::string_view guest_path) const;

  // The other half of the invariant in eternalsonata_asset_api.h: a patched
  // container is only ever served with its record rewritten to "stored, this
  // many bytes". Only PatchedContainer's writer calls this.
  bool SetStored(std::string_view guest_path, uint32_t decoded_size);

  const std::vector<uint8_t>& bytes() const { return raw_; }

 private:
  std::vector<uint8_t> raw_;
  std::vector<TocEntry> entries_;
  std::unordered_map<std::string, size_t> index_;
};

// docs/asset-formats.md section 2. `flag` is the TOC codec flag; `decoded_size`
// is the TOC's size field and bounds the output exactly.
bool DecodeAsset(const uint8_t* data, size_t size, uint32_t decoded_size, uint8_t flag,
                 std::vector<uint8_t>& out);

// ---------------------------------------------------------------------------
// BTX text sections
// ---------------------------------------------------------------------------
extern const char* const kBtxLanguages[7];  // "JPN ", "USA ", ... trailing space

struct BtxLang {
  std::string fourcc;      // 4 chars, trailing space included
  size_t sub_offset = 0;   // absolute file offset of the sub-block
  size_t sub_length = 0;   // its byte length
  uint32_t f12 = 0;        // +0x0C, a constant the reader ignores but we keep
  std::map<uint32_t, std::string> entries;
};

struct BtxBlob {
  size_t offset = 0;  // absolute file offset of the 'BTX ' magic
  uint32_t size = 0;
  uint32_t f12 = 0;
  bool terminated = false;  // last sub-block's `next` is 0 (both shapes ship)
  std::vector<BtxLang> langs;

  const BtxLang* Find(std::string_view fourcc) const;
};

// Scans for the magic and validates by parsing, the way scripts/btx.py does:
// the bulk section has no directory of its own. Blobs come back in ascending
// file order.
std::vector<BtxBlob> FindBtxBlobs(const std::vector<uint8_t>& data);

enum class EditStatus {
  kOk,
  kNotFound,   // no such blob / language / string id
  kTooLarge,   // did not fit and ALLOW_RESIZE was not set
  kBadData,    // the container is not a well-formed .e
};

struct TextEdit {
  size_t blob = 0;
  std::string lang;  // fourcc with trailing space, or empty for every language
  uint32_t id = 0;
  std::string value;  // already in the game's single-byte encoding
  bool allow_resize = false;

  // Filled in by ApplyTextEdits so the caller can report per-patch outcomes.
  EditStatus status = EditStatus::kOk;
};

// Applies every edit in ONE rebuild pass per blob, blobs in ascending file
// offset, fixing the .e relocation tables and header for any size change.
// Returns true if `data` was modified. Edits that could not be applied are
// marked in `edits` and leave the container untouched.
bool ApplyTextEdits(std::vector<uint8_t>& data, std::vector<TextEdit>& edits);

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------
// The game draws one glyph per byte. Mod-authored text arrives as UTF-8, so it
// is transcoded at this boundary and characters with no mapping fail loudly
// rather than becoming '?'. JPN is Shift-JIS and is passed through untouched.
bool TranscodeToGameEncoding(std::string_view utf8, std::string_view lang, std::string& out,
                             std::string* error);

}  // namespace eternalsonata::assets
