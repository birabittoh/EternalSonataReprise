// eternalsonata - F11 overlay choosing the characters' costumes.
//
// Stands in for the PS3's camp menu "Costumes" page until that exists. Every
// registered costume is offered, unlocked or not.

#include "costume_overlay.h"

#include "costume_system.h"

#include <array>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>

#include <imgui.h>

#include "eternalsonata_costume_api.h"

namespace eternalsonata {
namespace {

constexpr char kBindName[] = "bind_costumes";
constexpr char kWindowTitle[] = "Costumes##eternalsonata_costumes";

constexpr std::array<const char*, ETERNALSONATA_COSTUME_CHARACTER_COUNT> kCharacters = {
    "Allegretto", "Polka", "Beat",     "Frederic", "Viola",
    "Salsa",      "Jazz",  "Falsetto", "Claves",   "March",
};

class CostumeOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit CostumeOverlay(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {
    rex::ui::RegisterBind(kBindName, "F11", "Toggle costume overlay",
                          [this] { visible_ = !visible_; }, [this] { return visible_; },
                          kWindowTitle);
  }

  ~CostumeOverlay() override { rex::ui::UnregisterBind(kBindName); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!visible_) {
      return;
    }

    rex::ui::SetNextWindowFittedSize(io, 380.0f, 220.0f);
    ImGui::SetNextWindowBgAlpha(0.9f);
    if (!ImGui::Begin(kWindowTitle, &visible_, ImGuiWindowFlags_NoCollapse)) {
      ImGui::End();
      return;
    }

    bool any = false;
    for (int c = 1; c <= ETERNALSONATA_COSTUME_CHARACTER_COUNT; ++c) {
      const int count = CostumeCount(c);
      if (count <= 1) {
        continue;
      }
      any = true;
      ImGui::PushID(c);
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(kCharacters[c - 1]);
      ImGui::SameLine(110.0f);
      ImGui::SetNextItemWidth(-1.0f);
      const int worn = WornCostume(c);
      if (ImGui::BeginCombo("##costume", CostumeLabel(c, worn))) {
        for (int i = 0; i < count; ++i) {
          if (ImGui::Selectable(CostumeLabel(c, i), i == worn) && i != worn &&
              WearCostume(c, i) < 0) {
            failed_ = true;
          }
        }
        ImGui::EndCombo();
      }
      ImGui::PopID();
    }

    if (!any) {
      ImGui::TextWrapped("No costumes installed. Mods add them; see docs/costumes.md.");
    } else {
      ImGui::Spacing();
      ImGui::TextDisabled("The field leader changes at once, outside cutscenes.");
    }
    if (failed_) {
      ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1.0f), "A costume failed to load; see the log.");
    }
    ImGui::End();
  }

 private:
  bool visible_ = false;
  bool failed_ = false;
};

}  // namespace

std::unique_ptr<rex::ui::ImGuiDialog> CreateCostumeOverlay(rex::ui::ImGuiDrawer* drawer) {
  return std::make_unique<CostumeOverlay>(drawer);
}

}  // namespace eternalsonata
