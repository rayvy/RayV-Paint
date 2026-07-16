#include "UiPanel.h"
#include "../style/UiTokens.h"
#include <imgui_internal.h>
#include <cmath>
#include <algorithm>

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

    // Default: outer panel does not own a scrollbar (content children do).
    const ImGuiWindowFlags noOuterScroll =
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    return ImGui::Begin(name, open, flags | ImGuiWindowFlags_NoCollapse | noOuterScroll);
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

void ClampDockLeafCrossAxis(bool verticalStrip, float minPx, float maxPx) {
    ImGuiWindow* tw = ImGui::GetCurrentWindow();
    if (!tw || !tw->DockNode || tw->DockNode->IsFloatingNode() || tw->DockNode->IsSplitNode())
        return;
    ImGuiDockNode* node = tw->DockNode;
    if (verticalStrip) {
        float cur = node->SizeRef.x > 1.f ? node->SizeRef.x : node->Size.x;
        float w = std::clamp(cur, minPx, maxPx);
        node->SizeRef.x = w;
        // Also clamp live Size so drag cannot overshoot (no rubber-band).
        if (std::fabs(node->Size.x - w) > 0.5f)
            ImGui::DockBuilderSetNodeSize(node->ID, ImVec2(w, node->Size.y));
    } else {
        float cur = node->SizeRef.y > 1.f ? node->SizeRef.y : node->Size.y;
        float h = std::clamp(cur, minPx, maxPx);
        node->SizeRef.y = h;
        if (std::fabs(node->Size.y - h) > 0.5f)
            ImGui::DockBuilderSetNodeSize(node->ID, ImVec2(node->Size.x, h));
    }
}

void ClampDockLeafBox(float minW, float maxW, float minH, float maxH) {
    ImGuiWindow* tw = ImGui::GetCurrentWindow();
    if (!tw || !tw->DockNode || tw->DockNode->IsFloatingNode() || tw->DockNode->IsSplitNode())
        return;
    ImGuiDockNode* node = tw->DockNode;
    float w = std::clamp(node->Size.x, minW, maxW);
    float h = std::clamp(node->Size.y, minH, maxH);
    node->SizeRef.x = w;
    node->SizeRef.y = h;
    if (std::fabs(node->Size.x - w) > 0.5f || std::fabs(node->Size.y - h) > 0.5f)
        ImGui::DockBuilderSetNodeSize(node->ID, ImVec2(w, h));
}

} // namespace Ui
