#include "UiPanel.h"
#include "../style/UiTokens.h"

namespace Ui {

bool BeginDockPanel(const char* name, bool* open, ImGuiWindowFlags flags) {
    auto& T = Tokens();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, T.rMd);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(T.s3, T.s3));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, T.rSm);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, T.rSm);

    // Elevated-ish surface
    ImGui::PushStyleColor(ImGuiCol_WindowBg, T.bgWindow);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(T.bgElevated.x, T.bgElevated.y, T.bgElevated.z, T.isDark ? 0.55f : 0.70f));
    ImGui::PushStyleColor(ImGuiCol_Border, T.strokeHairline);
    ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(T.bgElevated.x, T.bgElevated.y, T.bgElevated.z, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(T.bgElevated.x, T.bgElevated.y, T.bgElevated.z, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, T.textPrimary);

    return ImGui::Begin(name, open, flags | ImGuiWindowFlags_NoCollapse);
}

void EndDockPanel() {
    ImGui::End();
    ImGui::PopStyleColor(6);
    ImGui::PopStyleVar(5);
}

void SectionLabel(const char* text) {
    auto& T = Tokens();
    ImGui::PushStyleColor(ImGuiCol_Text, T.textSecondary);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::Spacing();
}

} // namespace Ui
