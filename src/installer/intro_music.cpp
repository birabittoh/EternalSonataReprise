#include "intro_music.h"

#include <memory>
#include <vector>

#include <rex/audio/wma_player.h>
#include <rex/logging.h>

#include "bgm.generated.h"

namespace eternalsonata {
namespace {

std::unique_ptr<rex::audio::WmaPlayer> g_player;
bool g_enabled = true;

}  // namespace

void StartIntroMusic() {
  if (g_player || kBgmSize == 0)
    return;
  auto player = std::make_unique<rex::audio::WmaPlayer>();
  std::vector<std::vector<uint8_t>> songs(1, std::vector<uint8_t>(kBgm, kBgm + kBgmSize));
  if (!player->PlayPlaylist(std::move(songs), 0, true)) {
    REXLOG_WARN("Intro music: res/bgm.wma did not play");
    return;
  }
  if (!g_enabled)
    player->Pause();
  g_player = std::move(player);
}

void StopIntroMusic() {
  if (!g_player)
    return;
  g_player->Stop();
  g_player.reset();
}

bool IntroMusicEnabled() {
  return g_enabled;
}

void SetIntroMusicEnabled(bool enabled) {
  g_enabled = enabled;
  if (!g_player)
    return;
  if (enabled)
    g_player->Resume();
  else
    g_player->Pause();
}

}  // namespace eternalsonata
