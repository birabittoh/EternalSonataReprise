// eternalsonata - ReXGlue Recompiled Project
//
// A Plume-backed ui::ImmediateDrawer, which is what brings the SDK's overlays
// (F3 debug, F4 settings, toasts, the mod manager) back in native rendering
// mode.
//
// They are not missing because anything broke. With no GPU plugin the SDK sets
// `config.graphics` to null and never creates a presenter; instead it asks the
// app for a drawer through ReXApp::OnCreateImmediateDrawer, and the default
// returns nullptr, whose documented meaning is "no overlay". So the overlays
// are off for exactly as long as nothing here answers that call.
//
// Two parts of the SDK's contract for this mode shape the implementation, and
// both are easy to violate:
//
//  * the drawer is constructed presenter-less, and OnEnterPresenter /
//    OnLeavePresenter are never called, so all GPU setup has to be lazy;
//  * CreateTexture MUST return nullptr rather than fail loudly when the device
//    is not up yet, because the SDK uploads the ImGui font atlas lazily on the
//    first draw and that can happen before the backend exists.

#pragma once

#include <cstdint>
#include <memory>

namespace rex::ui {
class ImmediateDrawer;
}

namespace eternalsonata {

// Build the overlay drawer. Returns null when the Plume backend is not up, in
// which case the SDK simply runs without overlays, exactly as it does now.
std::unique_ptr<rex::ui::ImmediateDrawer> CreatePlumeImmediateDrawer();

}  // namespace eternalsonata
