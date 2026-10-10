#include "input_overlay.h"

#include <algorithm>
#include <cstdint>
#include <string>

#include <rex/input/input_system.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>

#include <imgui.h>

#include "ui_text.h"

namespace eternalsonata {
namespace {

constexpr char kBindName[] = "bind_input_overlay";
constexpr char kWindowId[] = "###eternalsonata_input";

std::string WindowTitle() { return std::string(Tr("input_overlay", "title")) + kWindowId; }

class InputOverlay final : public rex::ui::ImGuiDialog {
 public:
  InputOverlay(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input_system)
      : rex::ui::ImGuiDialog(drawer), input_system_(input_system) {
    rex::ui::RegisterBind(kBindName, "F5", "Toggle input overlay",
                          [this] { visible_ = !visible_; }, [this] { return visible_; },
                          WindowTitle().c_str());
  }

  ~InputOverlay() override { rex::ui::UnregisterBind(kBindName); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!visible_ || !input_system_) {
      return;
    }

    rex::ui::SetNextWindowFittedSize(io, 520.0f, 260.0f);
    ImGui::SetNextWindowBgAlpha(0.9f);
    if (!ImGui::Begin(WindowTitle().c_str(), &visible_, ImGuiWindowFlags_NoCollapse)) {
      ImGui::End();
      return;
    }

    std::string player_items = Tr("input_overlay", "ignore");
    float combo_width = ImGui::CalcTextSize(player_items.c_str()).x;
    player_items.push_back('\0');
    for (int n = 1; n <= 4; n++) {
      const std::string name = Tr("input_overlay", "player_n", std::to_string(n));
      combo_width = std::max(combo_width, ImGui::CalcTextSize(name.c_str()).x);
      player_items += name;
      player_items.push_back('\0');
    }
    const ImGuiStyle& style = ImGui::GetStyle();
    combo_width += style.FramePadding.x * 2.0f + ImGui::GetFrameHeight();
    const auto devices = input_system_->SnapshotDevices();
    size_t shown = 0;
    if (ImGui::BeginTable("##devices", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn(Tr("input_overlay", "col_player"), ImGuiTableColumnFlags_WidthFixed,
                              combo_width + style.CellPadding.x * 2.0f);
      ImGui::TableSetupColumn(Tr("input_overlay", "col_input"), ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn(Tr("input_overlay", "col_status"), ImGuiTableColumnFlags_WidthFixed, 90.0f);
      ImGui::TableHeadersRow();

      for (const auto& view : devices) {
        if (view.device.kind == rex::input::DeviceKind::kPlaceholder) {
          continue;
        }
        shown++;
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        int selected_user = 0;
        for (uint32_t user = 0; user < rex::input::kMaxGuestUsers; user++) {
          if (view.guest_user_mask & (1u << user)) {
            selected_user = static_cast<int>(user) + 1;
            break;
          }
        }
        const uint64_t device_id = static_cast<uint64_t>(view.device.id);
        ImGui::PushID(static_cast<int>(device_id >> 32));
        ImGui::PushID(static_cast<int>(device_id));
        ImGui::SetNextItemWidth(combo_width);
        if (ImGui::Combo("##player", &selected_user, player_items.c_str())) {
          const uint32_t user = selected_user == 0
                                    ? rex::input::kGuestUserUnassigned
                                    : static_cast<uint32_t>(selected_user - 1);
          input_system_->AssignDeviceToUser(view.device.id, user);
        }
        ImGui::PopID();
        ImGui::PopID();
        ImGui::TableSetColumnIndex(1);
        const char* device_name =
            view.device.kind == rex::input::DeviceKind::kKeyboardMouse
                ? Tr("input_overlay", "keyboard_mouse")
                : view.device.kind == rex::input::DeviceKind::kTouch
                      ? Tr("input_overlay", "touchscreen")
                : view.device.name == "XInput Controller"
                      ? Tr("input_overlay", "xinput_controller")
                : view.device.name.empty() ? Tr("input_overlay", "controller")
                                           : view.device.name.c_str();
        ImGui::TextUnformatted(device_name);
        ImGui::TableSetColumnIndex(2);
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.0f), "%s",
                           Tr("input_overlay", "connected"));
      }
      ImGui::EndTable();
    }

    if (!shown) {
      ImGui::TextDisabled("%s", Tr("input_overlay", "none"));
    }
    ImGui::End();
  }

 private:
  rex::input::InputSystem* input_system_ = nullptr;
  bool visible_ = false;
};

}  // namespace

std::unique_ptr<rex::ui::ImGuiDialog> CreateInputOverlay(
    rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input_system) {
  return std::make_unique<InputOverlay>(drawer, input_system);
}

}  // namespace eternalsonata
