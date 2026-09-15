// eternalsonata - ReXGlue Recompiled Project
//
// Translates the achievement catalogue into the language the process booted in,
// from the strings mods published (see settings.h's RegisterLanguageListeners
// and the declarative [language.strings] table in assets.toml).
//
// The catalogue itself comes out of the guest's own XDBF, which the SDK reads
// once during XEX load in whatever language user_language named at the time.
// That covers the five languages the disc shipped with and nothing else, so a
// language a mod invented has no metadata to fall back on beyond its donor's.
// This fills that in.

#pragma once

#include <rex/runtime.h>

namespace eternalsonata {

// Rewrites the label, description and locked description of every achievement a
// mod translated, and leaves the rest exactly as the XDBF supplied them. Both
// the F7 overlay and the unlock toast read the same catalogue, so one pass
// covers both.
//
// Translation happens once, at startup, rather than at draw time: user_language
// is a restart-required setting, so the language cannot change under a running
// process anyway. Call from OnPostSetup, after the mods have registered and
// after ApplyBootLanguageDonorSlot.
void ApplyAchievementTranslations(rex::Runtime* runtime);

}  // namespace eternalsonata
