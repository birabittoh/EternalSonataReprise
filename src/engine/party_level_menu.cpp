// eternalsonata - Lets the Party Level menu change the level outside Encore.
//
// sub_82228410 builds the menu and sub_822289C8 runs it each frame. Both test
// the Encore counter (docs/flags.md §8): without it the menu only shows the
// level, with it confirming a row sets dword_8243F3E8 to any level up to the
// highest reached, dword_8243F3EC. Nothing else in either routine depends on
// Encore, so running them with the counter faked to 1 is the whole change.

#include "generated/eternalsonata_init.h"

#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "flag_system.h"

REXCVAR_DEFINE_STRING(party_level_select, "encore", "Eternal Sonata",
                      "When the Party Level menu can change the level: encore (as the game "
                      "does) or always")
    .allowed({"encore", "always"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

class ScopedEncore {
 public:
  ScopedEncore() : active_(REXCVAR_GET(party_level_select) == "always") {
    if (active_) {
      eternalsonata::BeginEncoreOverride();
    }
  }
  ~ScopedEncore() {
    if (active_) {
      eternalsonata::EndEncoreOverride();
    }
  }

 private:
  bool active_;
};

}  // namespace

REX_EXTERN(__imp__sub_82228410);
REX_HOOK_RAW(sub_82228410) {
  ScopedEncore encore;
  __imp__sub_82228410(ctx, base);
}

REX_EXTERN(__imp__sub_822289C8);
REX_HOOK_RAW(sub_822289C8) {
  ScopedEncore encore;
  __imp__sub_822289C8(ctx, base);
}
