#include "input_overlay.h"

#include <cstdint>

#include <rex/input/input_system.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>

#include <imgui.h>

namespace eternalsonata {
namespace {

constexpr char kBindName[] = "bind_input_overlay";
constexpr char kWindowTitle[] = "Input##eternalsonata_input";

class InputOverlay final : public rex::ui::ImGuiDialog {
 public:
  InputOverlay(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input_system)
      : rex::ui::ImGuiDialog(drawer), input_system_(input_system) {
    rex::ui::RegisterBind(kBindName, "F5", "Toggle input overlay",
                          [this] { visible_ = !visible_; }, [this] { return visible_; },
                          kWindowTitle);
  }

  ~InputOverlay() override { rex::ui::UnregisterBind(kBindName); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!visible_ || !input_system_) {
      return;
    }

    rex::ui::SetNextWindowFittedSize(io, 520.0f, 260.0f);
    ImGui::SetNextWindowBgAlpha(0.9f);
    if (!ImGui::Begin(kWindowTitle, &visible_, ImGuiWindowFlags_NoCollapse)) {
      ImGui::End();
      return;
    }

    const auto devices = input_system_->SnapshotDevices();
    size_t shown = 0;
    if (ImGui::BeginTable("##devices", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthFixed, 105.0f);
      ImGui::TableSetupColumn("Input", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 90.0f);
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
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::Combo("##player", &selected_user,
                         "Ignore\0Player 1\0Player 2\0Player 3\0Player 4\0")) {
          const uint32_t user = selected_user == 0
                                    ? rex::input::kGuestUserUnassigned
                                    : static_cast<uint32_t>(selected_user - 1);
          input_system_->AssignDeviceToUser(view.device.id, user);
        }
        ImGui::PopID();
        ImGui::PopID();
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(view.device.name.empty() ? "Controller" : view.device.name.c_str());
        ImGui::TableSetColumnIndex(2);
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.0f), "Connected");
      }
      ImGui::EndTable();
    }

    if (!shown) {
      ImGui::TextDisabled("No controllers detected.");
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
