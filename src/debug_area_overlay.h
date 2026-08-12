// eternalsonata - Debug overlay: lists known field areas and force-loads one
// via ForceLoadArea (see force_load_area.h) on click. Opened from a button in
// the curated settings dialog's Advanced section (settings.cpp).
#pragma once

#include <memory>

#include <rex/ui/imgui_dialog.h>

namespace rex::ui {
class ImGuiDrawer;
}  // namespace rex::ui

namespace eternalsonata {

std::unique_ptr<rex::ui::ImGuiDialog> CreateDebugAreaOverlay(rex::ui::ImGuiDrawer* drawer);

}  // namespace eternalsonata
