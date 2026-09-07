#pragma once

#include <memory>

namespace rex::input {
class InputSystem;
}

namespace rex::ui {
class ImGuiDialog;
class ImGuiDrawer;
}

namespace eternalsonata {

std::unique_ptr<rex::ui::ImGuiDialog> CreateInputOverlay(
    rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input_system);

}  // namespace eternalsonata
