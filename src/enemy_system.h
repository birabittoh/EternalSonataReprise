// eternalsonata - Enemy stats and the rebalance overrides, for mods.
//
// The public C ABI mods call is eternalsonata_enemy_api.h; this header is the
// small internal surface the rest of the exe needs.
#pragma once

namespace rex {
class Runtime;
}  // namespace rex

namespace eternalsonata {

// Binds the enemy system to the runtime. Call once the runtime is live
// (OnPostSetup). Until then every API entry point answers "unavailable" and no
// override event is published (the events need the runtime's mod registry).
void BindEnemySystem(rex::Runtime* runtime);

// Reapplies the per-type overrides to every live enemy. Called once per guest
// frame from the render pump's present hook, i.e. on the guest main thread, so
// that an override set from a UI takes effect on the next frame. Cheap and a
// no-op when no battle is running or no override is set.
void EnemySystemTick();

}  // namespace eternalsonata
