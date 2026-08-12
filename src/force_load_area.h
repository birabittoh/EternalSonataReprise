// eternalsonata - Debug tool: force-loads a field ".e" area on demand.
//
// Wraps the field-area loader (sub_820FAFB0, the same function
// eternalsonata_presence.cpp's CaptureFieldArea hook observes -- see that
// file's "Field area tracking" comment) so an ImGui button can trigger a real
// area load. The loader mutates a chunk of shared guest state (task-pool
// bookkeeping, cached cfdata buffers) that the game normally touches only
// from one call site with specific pre-conditions already established, so
// this is a debug convenience, not something to leave wired to gameplay: it
// can misbehave (or worse) if triggered mid-battle, mid-menu, or mid another
// transition. Requests are queued from the ImGui draw thread and executed on
// the next guest-frame tick, never called directly from the draw callback --
// the guest call machinery (rex::ppc::ImportFunction's auto-isolating call)
// dereferences the current guest ThreadState, which does not exist on the
// host draw thread and previously crashed the game when tried (see
// docs/debug-hooks.md, "Do not call guest functions re-entrantly ... from
// inside a draw hook").
#pragma once

#include <mutex>
#include <string>

namespace rex {
class Runtime;
}  // namespace rex

namespace eternalsonata {

class ForceLoadArea {
 public:
  ForceLoadArea() = default;

  // Registers the guest-frame tick that services queued requests. Call once
  // KernelState and the runtime are both live (OnPostSetup).
  void Bind(rex::Runtime* runtime);

  // Queues an area id (e.g. "hno01") to be force-loaded via sub_820FAFB0 on
  // the next guest-frame tick. Thread-safe; safe to call from the ImGui draw
  // thread.
  void Request(std::string area_id);

 private:
  void Tick();

  std::mutex mutex_;
  std::string pending_area_id_;
  bool has_pending_ = false;
};

// Process-wide instance shared between the debug overlay and the app hooks.
ForceLoadArea& GetForceLoadArea();

// Expose warp data for the field resume hook to apply (used by field_player_model_override.cpp)
std::mutex& GetWarpMutex();
uint32_t& GetWarpEncodedDword();
bool& GetWarpPending();

}  // namespace eternalsonata
