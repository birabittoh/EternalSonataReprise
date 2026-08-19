#include "debug_area_overlay.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include <imgui.h>
#include <rex/cvar.h>

#include "area_names.generated.h"
#include "force_load_area.h"
#include "room_presence.h"
#include "settings.h"

namespace eternalsonata {

namespace {

class DebugAreaOverlay : public rex::ui::ImGuiDialog {
 public:
  explicit DebugAreaOverlay(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& /*io*/) override {
    ImGui::SetNextWindowSize(ImVec2(420, 480), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.9f);
    if (!ImGui::Begin("Debug##rex", nullptr, ImGuiWindowFlags_NoCollapse)) {
      ImGui::End();
      return;
    }

    // Warping to an area mid-battle would tear the field out from under the
    // battle, so the whole window is disabled for the duration. RoomPresence
    // already tracks that distinction for the Discord state row (a battle
    // leaves the field loaded underneath it, so "is a field loaded" cannot
    // tell them apart), so reuse it rather than re-deriving it here.
    const bool in_battle = GetRoomPresence().IsBattleActive();

    ImGui::BeginDisabled(in_battle);
    if (in_battle) {
      ImGui::TextUnformatted("Area loading is unavailable during a battle.");
    }

    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##filter", "Filter by id or name... or paste to load", filter_buf_,
                                 sizeof(filter_buf_),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
      RequestLoad(filter_buf_);
    }

    bool show_all_maps = rex::cvar::GetFlagByName("debug_show_all_maps") == "true";
    if (ImGui::Checkbox("Show all maps", &show_all_maps)) {
      rex::cvar::SetFlagByName("debug_show_all_maps", show_all_maps ? "true" : "false");
      SaveUserSettings();
    }

    ImGui::BeginChild("##arealist", ImVec2(0, 0), ImGuiChildFlags_Borders);
    std::string filter = ToLower(filter_buf_);
    for (const auto& [id, name] : SortedAreas()) {
      // Filter out unnamed areas unless "Show all maps" is enabled
      if (!show_all_maps && name[0] == '\0') {
        continue;
      }
      if (!filter.empty() && ToLower(id).find(filter) == std::string::npos &&
          ToLower(name).find(filter) == std::string::npos) {
        continue;
      }
      std::string label = name[0] != '\0' ? id + " - " + name : id;
      if (ImGui::Selectable(label.c_str())) {
        RequestLoad(id);
      }
    }
    ImGui::EndChild();
    ImGui::EndDisabled();

    ImGui::End();
  }

 private:
  static std::string ToLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
  }

  // AreaNameTable() is an unordered_map; sort once for a stable, readable list.
  static const std::vector<std::pair<std::string, std::string>>& SortedAreas() {
    static const std::vector<std::pair<std::string, std::string>> sorted = [] {
      std::vector<std::pair<std::string, std::string>> v;
      for (const auto& [id, name] : AreaNameTable()) {
        v.emplace_back(id, name);
      }
      std::sort(v.begin(), v.end());
      return v;
    }();
    return sorted;
  }

  void RequestLoad(const std::string& id) {
    if (id.empty()) {
      return;
    }
    GetForceLoadArea().Request(id);
  }

  char filter_buf_[64] = {};
};

}  // namespace

std::unique_ptr<rex::ui::ImGuiDialog> CreateDebugAreaOverlay(rex::ui::ImGuiDrawer* drawer) {
  return std::make_unique<DebugAreaOverlay>(drawer);
}

}  // namespace eternalsonata
