// eternalsonata - ReXGlue Recompiled Project
//
// Translates the achievement catalogue into the language the process booted in,
// from the strings mods published (see settings.h's RegisterLanguageListeners
// and the declarative [language.strings] table in assets.toml).
//
// The catalogue comes from achievement_text.h, the PAL and JP XDBFs dumped as
// code, so the guest image's XDBF is not read for text. A language a mod
// invented falls back to its donor's strings.

#pragma once

#include <rex/runtime.h>

namespace eternalsonata {

// Replaces the catalogue with the boot language's strings, then applies every
// label, description and locked description a mod translated. Both
// the F7 overlay and the unlock toast read the same catalogue, so one pass
// covers both.
//
// Translation happens once, at startup, rather than at draw time: user_language
// is a restart-required setting, so the language cannot change under a running
// process anyway. Call from OnPostSetup, after the mods have registered and
// after ApplyBootLanguageDonorSlot.
void ApplyAchievementTranslations(rex::Runtime* runtime);

}  // namespace eternalsonata
