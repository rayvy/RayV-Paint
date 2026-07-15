#include "HubUi.h"
#include "imgui.h"

namespace helpers {

AppMode DrawHubUi(Dx11ImGuiApp& app) {
    AppMode next = AppMode::Hub;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("RayV Helpers", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse);

    ImGui::TextUnformatted("Micro-tools (isolated from RayVPaint editor)");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("PNG → DDS Converter", ImVec2(-1, 48)))
        next = AppMode::Convert;
    ImGui::TextDisabled("Batch convert next to source. Context menu: multi-select PNGs.");

    ImGui::Spacing();
    if (ImGui::Button("Texture Atlas Creator", ImVec2(-1, 48)))
        next = AppMode::Atlas;
    ImGui::TextDisabled("Pack sprites → PNG + JSON. Optional DDS export of the atlas.");

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(120, 0)))
        app.RequestClose();

    ImGui::End();
    return next;
}

} // namespace helpers
