// eternalsonata - F11 overlay choosing the characters' costumes.

#pragma once

#include <memory>

namespace rex::ui {
class ImGuiDialog;
class ImGuiDrawer;
}

namespace eternalsonata {

std::unique_ptr<rex::ui::ImGuiDialog> CreateCostumeOverlay(rex::ui::ImGuiDrawer* drawer);

}  // namespace eternalsonata
