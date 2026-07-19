#include "ViewportNavPanel.h"
#include "../EditorPanels.h"
#include "../widgets/UiPanel.h"
#include "../widgets/UiIconToggle.h"
#include "../widgets/UiVisualSlider.h"
#include <imgui.h>

namespace UI {

void DrawViewportNavPanel(UIState& state, Canvas& canvas) {
    if (!state.showViewportNav) return;

    Ui::BeginDockPanel("Viewport Navigation", &state.showViewportNav);
    ImGui::Text("Zoom %.0f%%   ·   Pan (%.0f, %.0f)",
        canvas.GetZoom() * 100.0f, canvas.GetPan().x, canvas.GetPan().y);
    ImGui::Spacing();
    bool flipH = canvas.GetViewportFlipH();
    bool flipV = canvas.GetViewportFlipV();
    if (Ui::IconToggle("##fliph", "ts_mirror_h", &flipH, ImVec2(32, 32),
            "Flip Horizontal (on)", "Flip Horizontal (off)"))
        canvas.SetViewportFlipH(flipH);
    ImGui::SameLine();
    if (Ui::IconToggle("##flipv", "ts_mirror_v", &flipV, ImVec2(32, 32),
            "Flip Vertical (on)", "Flip Vertical (off)"))
        canvas.SetViewportFlipV(flipV);
    float rotAngle = canvas.GetRotationAngle() * (180.0f / 3.14159265f);
    if (Ui::SmartSliderFloat("Rotation", &rotAngle, -180.0f, 180.0f, 0.f, 45.f, "%.0f°")) {
        canvas.SetRotationAngle(rotAngle * (3.14159265f / 180.0f));
    }
    ImGui::TextDisabled("Hover slider + Backspace: reset  ·  Ctrl: 45° snap");
    Ui::EndDockPanel();
}

} // namespace UI
