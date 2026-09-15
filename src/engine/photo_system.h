// eternalsonata - Photo album state for mods.
//
// The public C ABI mods call is eternalsonata_photo_api.h; this header is the
// small internal surface the rest of the exe needs.
#pragma once

namespace rex {
class Runtime;
}  // namespace rex

namespace eternalsonata {

// Binds the photo system to the runtime and starts the per-frame poll that
// publishes the album's events. Call once the runtime is live (OnPostSetup).
// Until then every API entry point answers "unavailable" and no photo event is
// published (the events need the runtime's mod registry).
void BindPhotoSystem(rex::Runtime* runtime);

}  // namespace eternalsonata
