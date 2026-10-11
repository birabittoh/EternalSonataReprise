// eternalsonata - ReXGlue Recompiled Project
//
// See native_renderer_shader_debug.h. The implementation is shared with the
// other ReXGlue titles (KryptonRecomp/native_renderer_shader_debug_impl.h);
// this file only provides the includes, the debug pack symbols, and the
// namespace to define it in.

#include "native_renderer_shader_debug.h"

#include <atomic>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <rex/logging.h>

#include "guest_shaders.h"

extern "C" {
extern const uint8_t kGuestShaderDebugPackData[];
extern const uint8_t kGuestShaderDebugPackDataEnd[];
}

namespace eternalsonata {
namespace {

// 'ESGD', with the point sprite shader past the two slot tables.
constexpr krypton::DebugPackTraits kGuestShaderDebugPackTraits{0x44475345, 2, 1};

// Echo the overlay id of the vs_040/ps_058 pair under investigation, so the
// number on screen can be compared with the one a toggle sends back.
void LogPublishedId(bool pixel, uint32_t slot, uint64_t id) {
  if (!((slot == 40 && !pixel) || (slot == 58 && pixel)))
    return;
  static uint32_t published = 0;
  if (published < 4) {
    ++published;
    REXLOG_WARN("native_renderer: shader debugger published {}_{:03d} as id 0x{:016X}",
                pixel ? "ps" : "vs", slot, id);
  }
}

}  // namespace

#define KRYPTON_GUEST_SHADER_DEBUG_ON_PUBLISH LogPublishedId
#include "native_renderer_shader_debug_impl.h"
}  // namespace eternalsonata
