#pragma once

namespace eternalsonata {

// The start screen's looping music. Start and Stop bracket the screen; the
// toggle only silences it, so turning it back on resumes where it was.
void StartIntroMusic();
void StopIntroMusic();
bool IntroMusicEnabled();
void SetIntroMusicEnabled(bool enabled);

}  // namespace eternalsonata
