// eternalsonata - ReXGlue Recompiled Project
//
// See native_renderer_shader_debug.h.

#include "native_renderer_shader_debug.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <rex/filesystem.h>
#include <rex/logging.h>

#include "guest_shaders.h"

namespace eternalsonata {
namespace {

// Both guest tables are 256 entries, the same bound guest_shaders.cpp uses.
constexpr uint32_t kSlots = 256;

// One shader's live state. Every field is written on the guest thread and read
// on the UI thread, so all of it is atomic: this is a debug view, and a torn
// counter in it would be a worse bug than the one it is there to find.
struct ShaderState {
  std::atomic<bool> disabled{false};
  std::atomic<uint32_t> active_frame{0};
  std::atomic<uint64_t> draw_count{0};
  std::atomic<uint64_t> profile_ns{0};
  std::atomic<uint64_t> profile_draws{0};
};

ShaderState g_vertex[kSlots];
ShaderState g_pixel[kSlots];

std::atomic<bool> g_profiling{false};
// Bumped once per guest swap. A shader is "active" when it drew in this frame
// or the one before it: the overlay draws at its own rate, so testing only the
// current frame makes every row flicker.
std::atomic<uint32_t> g_frame{1};

ShaderState* Lookup(bool pixel, uint32_t slot) {
  if (slot >= kSlots)
    return nullptr;
  return pixel ? &g_pixel[slot] : &g_vertex[slot];
}

const GuestShader& PackEntry(bool pixel, uint32_t slot) {
  return pixel ? GuestPixelShader(slot) : GuestVertexShader(slot);
}

// --- The sidecar text pack -------------------------------------------------
//
// guest_shaders_debug.bin carries, per shader, the name the extractor gave it,
// the microcode disassembly and the HLSL the emitter produced. It is a separate
// file from guest_shaders.bin, and read a span at a time rather than loaded,
// because it is roughly twice the size of the pack the renderer actually needs
// and none of it is touched unless the debugger is opened. Its absence costs
// the details pane and nothing else.

constexpr uint32_t kDebugMagic = 0x44475345;  // 'ESGD', little endian
constexpr uint32_t kDebugVersion = 1;
constexpr char kDebugPackName[] = "guest_shaders_debug.bin";

#pragma pack(push, 1)
struct DebugHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t slots;
  uint32_t text_bytes;
};

struct DebugEntry {
  uint32_t name_offset, name_size;
  uint32_t ucode_offset, ucode_size;
  uint32_t hlsl_offset, hlsl_size;
};
#pragma pack(pop)

static_assert(sizeof(DebugHeader) == 16, "debug pack header layout");
static_assert(sizeof(DebugEntry) == 24, "debug pack entry layout");

// UI thread only, so no locking: the details provider is the only caller.
bool g_text_attempted = false;
std::ifstream g_text_file;
std::vector<DebugEntry> g_text_entries;
uint64_t g_text_base = 0;  // byte offset of the text section within the file
uint32_t g_text_bytes = 0;

bool EnsureTextPack() {
  if (g_text_attempted)
    return g_text_file.is_open();
  g_text_attempted = true;

  const std::filesystem::path path = rex::filesystem::GetExecutableFolder() / kDebugPackName;
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    REXLOG_INFO(
        "native_renderer: no {} next to the exe, so the shader debugger lists shaders but shows "
        "no source",
        kDebugPackName);
    return false;
  }

  DebugHeader header{};
  file.read(reinterpret_cast<char*>(&header), sizeof(header));
  if (!file || header.magic != kDebugMagic || header.version != kDebugVersion ||
      header.slots != kSlots) {
    REXLOG_WARN("native_renderer: {} is not a v{} debug pack", kDebugPackName, kDebugVersion);
    return false;
  }

  g_text_entries.resize(size_t(header.slots) * 2);
  file.read(reinterpret_cast<char*>(g_text_entries.data()),
            std::streamsize(g_text_entries.size() * sizeof(DebugEntry)));
  if (!file) {
    REXLOG_WARN("native_renderer: {} is truncated", kDebugPackName);
    g_text_entries.clear();
    return false;
  }

  g_text_base = sizeof(DebugHeader) + g_text_entries.size() * sizeof(DebugEntry);
  g_text_bytes = header.text_bytes;
  g_text_file = std::move(file);
  return true;
}

std::string ReadText(uint32_t offset, uint32_t size) {
  if (size == 0 || offset > g_text_bytes || size > g_text_bytes - offset)
    return {};
  std::string out(size, '\0');
  g_text_file.seekg(std::streamoff(g_text_base + offset));
  g_text_file.read(out.data(), std::streamsize(size));
  if (!g_text_file) {
    g_text_file.clear();
    return {};
  }
  return out;
}

// Name, microcode disassembly and HLSL for one shader, or empties when the
// sidecar is missing.
struct ShaderText {
  std::string name;
  std::string ucode;
  std::string hlsl;
};

ShaderText LoadText(bool pixel, uint32_t slot) {
  ShaderText out;
  if (!EnsureTextPack() || slot >= kSlots)
    return out;
  const DebugEntry& entry = g_text_entries[(pixel ? kSlots : 0) + slot];
  out.name = ReadText(entry.name_offset, entry.name_size);
  out.ucode = ReadText(entry.ucode_offset, entry.ucode_size);
  out.hlsl = ReadText(entry.hlsl_offset, entry.hlsl_size);
  return out;
}

}  // namespace

uint64_t GuestShaderDebugId(bool pixel, uint32_t slot) {
  return (uint64_t(pixel ? 1u : 0u) << 32) | slot;
}

bool GuestShaderDrawDisabled(int vertex_slot, int pixel_slot) {
  if (vertex_slot >= 0 && uint32_t(vertex_slot) < kSlots &&
      g_vertex[vertex_slot].disabled.load(std::memory_order_relaxed))
    return true;
  if (pixel_slot >= 0 && uint32_t(pixel_slot) < kSlots &&
      g_pixel[pixel_slot].disabled.load(std::memory_order_relaxed))
    return true;
  return false;
}

void NoteGuestShaderDraw(int vertex_slot, int pixel_slot, uint64_t elapsed_ns) {
  const uint32_t frame = g_frame.load(std::memory_order_relaxed);
  const bool profiling = elapsed_ns != 0;
  for (int pass = 0; pass < 2; ++pass) {
    const int slot = pass == 0 ? vertex_slot : pixel_slot;
    ShaderState* state = slot >= 0 ? Lookup(pass != 0, uint32_t(slot)) : nullptr;
    if (state == nullptr)
      continue;
    state->active_frame.store(frame, std::memory_order_relaxed);
    state->draw_count.fetch_add(1, std::memory_order_relaxed);
    if (profiling) {
      // The same elapsed time is charged to both stages. A host draw does not
      // separate them, and pretending otherwise would invent a split that the
      // measurement does not contain.
      state->profile_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
      state->profile_draws.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

bool GuestShaderProfilingEnabled() { return g_profiling.load(std::memory_order_relaxed); }

void GuestShaderDebugEndFrame() { g_frame.fetch_add(1, std::memory_order_relaxed); }

std::vector<rex::ui::ShaderDebuggerEntry> GuestShaderSnapshot() {
  std::vector<rex::ui::ShaderDebuggerEntry> out;
  const uint32_t frame = g_frame.load(std::memory_order_relaxed);

  for (uint32_t pass = 0; pass < 2; ++pass) {
    const bool pixel = pass != 0;
    for (uint32_t slot = 0; slot < kSlots; ++slot) {
      const GuestShader& shader = PackEntry(pixel, slot);
      if (!shader.valid())
        continue;  // The game never had a shader in this table slot.
      const ShaderState& state = pixel ? g_pixel[slot] : g_vertex[slot];

      rex::ui::ShaderDebuggerEntry entry;
      entry.ucode_hash = GuestShaderDebugId(pixel, slot);
      entry.type = pixel ? 1u : 0u;
      // The dialog labels this as the shader's size. Nothing at runtime holds
      // the microcode, so the compiled blob's dword count is what is reported;
      // it is the size of the thing that actually runs.
      entry.dword_count = (shader.dxil_size ? shader.dxil_size : shader.spirv_size) / 4;
      entry.disabled = state.disabled.load(std::memory_order_relaxed);
      const uint32_t active = state.active_frame.load(std::memory_order_relaxed);
      entry.active = active != 0 && frame - active <= 1;
      entry.profile_total_ns = state.profile_ns.load(std::memory_order_relaxed);
      entry.profile_draw_count = state.profile_draws.load(std::memory_order_relaxed);
      out.push_back(entry);
    }
  }
  return out;
}

rex::ui::ShaderDebuggerDetails GuestShaderDetails(uint64_t id) {
  rex::ui::ShaderDebuggerDetails out;
  const bool pixel = (id >> 32) != 0;
  const uint32_t slot = uint32_t(id & 0xFFFFFFFFu);
  const GuestShader& shader = PackEntry(pixel, slot);
  if (slot >= kSlots || !shader.valid())
    return out;

  out.found = true;
  const ShaderState& state = pixel ? g_pixel[slot] : g_vertex[slot];
  out.info.ucode_hash = id;
  out.info.type = pixel ? 1u : 0u;
  out.info.dword_count = (shader.dxil_size ? shader.dxil_size : shader.spirv_size) / 4;
  out.info.disabled = state.disabled.load(std::memory_order_relaxed);
  const uint32_t frame = g_frame.load(std::memory_order_relaxed);
  const uint32_t active = state.active_frame.load(std::memory_order_relaxed);
  out.info.active = active != 0 && frame - active <= 1;
  out.info.profile_total_ns = state.profile_ns.load(std::memory_order_relaxed);
  out.info.profile_draw_count = state.profile_draws.load(std::memory_order_relaxed);

  const ShaderText text = LoadText(pixel, slot);

  // A header the dialog shows above the disassembly. Everything in it is
  // something a wrong-looking draw makes you want to check: which slot this is,
  // what the pipeline builder will match its inputs against, which texture
  // slots the texture mirror will visit for it, and whether it carries the
  // literal constant pool that used to be the reason text was invisible.
  std::string preamble;
  preamble += (pixel ? "; pixel shader, guest table slot " : "; vertex shader, guest table slot ") +
              std::to_string(slot);
  if (!text.name.empty())
    preamble += " (" + text.name + ")";
  preamble += "\n; draws so far: " + std::to_string(state.draw_count.load(std::memory_order_relaxed));
  preamble += "\n; literal constant pool (c252..c255): ";
  preamble += shader.has_literals ? "present" : "none";
  if (pixel) {
    preamble += "\n; texture slots declared:";
    if (shader.texture_mask == 0) {
      preamble += " none";
    } else {
      for (uint32_t i = 0; i < 32; ++i) {
        if (shader.texture_mask & (1u << i))
          preamble += " t" + std::to_string(i);
      }
    }
    if (shader.has_cube_texture)
      preamble += "\n; samples a cube map";
  } else {
    preamble += "\n; vertex inputs (D3DDECLUSAGE, index):";
    for (uint32_t i = 0; i < shader.input_count; ++i) {
      preamble += " (" + std::to_string(shader.inputs[i].usage) + "," +
                  std::to_string(shader.inputs[i].usage_index) + ")";
    }
    if (shader.exports_point_size)
      preamble += "\n; exports point size";
  }
  preamble += "\n; interpolator keys:";
  for (uint32_t i = 0; i < shader.interpolator_key_count; ++i)
    preamble += " " + std::to_string(shader.interpolator_keys[i]);
  preamble += "\n\n";

  out.ucode_disassembly =
      preamble + (text.ucode.empty() ? "; no microcode disassembly in the debug pack" : text.ucode);

  // One translation, not one per format: the HLSL is the same source both
  // backends were compiled from, and the blob offered for saving is whichever
  // one this build produced.
  rex::ui::ShaderDebuggerTranslation translation;
  translation.modification = 0;
  translation.is_translated = true;
  translation.is_valid = true;
  translation.host_disassembly =
      text.hlsl.empty() ? "// no HLSL in the debug pack" : text.hlsl;
  const void* blob = shader.dxil ? shader.dxil : shader.spirv;
  const uint32_t blob_size = shader.dxil ? shader.dxil_size : shader.spirv_size;
  if (blob != nullptr) {
    translation.translated_binary.assign(static_cast<const uint8_t*>(blob),
                                         static_cast<const uint8_t*>(blob) + blob_size);
  }
  out.translations.push_back(std::move(translation));
  return out;
}

bool SetGuestShaderDisabled(uint64_t id, bool disabled) {
  // An identifier this renderer never issued is not an error: shaders.toml is
  // shared with the emulated-Xenos backend, whose identifiers are real microcode
  // hashes and mean nothing here.
  ShaderState* state = (id >> 33) == 0 ? Lookup((id >> 32) != 0, uint32_t(id & 0xFFFFFFFFu))
                                       : nullptr;
  if (state == nullptr)
    return false;
  state->disabled.store(disabled, std::memory_order_relaxed);
  return true;
}

void SetGuestShaderProfiling(bool enabled) {
  g_profiling.store(enabled, std::memory_order_relaxed);
}

void ResetGuestShaderProfiling() {
  for (uint32_t slot = 0; slot < kSlots; ++slot) {
    for (ShaderState* state : {&g_vertex[slot], &g_pixel[slot]}) {
      state->profile_ns.store(0, std::memory_order_relaxed);
      state->profile_draws.store(0, std::memory_order_relaxed);
    }
  }
}

void SetGuestShaderBlacklist(const std::vector<uint64_t>& ids) {
  size_t applied = 0;
  for (uint64_t id : ids)
    applied += SetGuestShaderDisabled(id, true) ? 1 : 0;
  if (applied != 0) {
    REXLOG_INFO("native_renderer: {} of {} shader(s) in shaders.toml disabled at startup", applied,
                ids.size());
  }
}

}  // namespace eternalsonata
