#include "UiPathField.h"
#include "UiTooltip.h"
#include "../style/UiTokens.h"
#include "../style/UiMotion.h"
#include <cstring>
#include <cstdio>
#include <algorithm>

namespace Ui {

bool PathField(const char* id, const char* label, char* pathBuf, size_t pathBufSize,
               bool (*browseFn)(char*, size_t, const char*), const char* browseFilter,
               const char* tooltip) {
    if (!pathBuf || pathBufSize == 0) return false;
    ImGui::PushID(id);
    auto& T = Tokens();
    bool changed = false;

    if (label && label[0]) {
        ImGui::PushStyleColor(ImGuiCol_Text, T.textSecondary);
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
    }

    const float browseW = 36.f;
    float full = ImGui::GetContentRegionAvail().x;
    float fieldW = std::max(80.f, full - browseW);

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, T.rSm);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(T.bgElevated.x, T.bgElevated.y, T.bgElevated.z, 0.75f));
    ImGui::SetNextItemWidth(fieldW);
    if (ImGui::InputText("##path", pathBuf, pathBufSize))
        changed = true;
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    ImGui::SameLine(0, 0);
    // Overlap style: button sits at end of field visually
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - 2.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, T.rSm);
    if (ImGui::Button("…##br", ImVec2(browseW, 0))) {
        if (browseFn && browseFn(pathBuf, pathBufSize, browseFilter ? browseFilter : "All\0*.*\0"))
            changed = true;
    }
    ImGui::PopStyleVar();
    if (ImGui::IsItemHovered())
        Tooltip(tooltip && tooltip[0] ? tooltip : "Browse…");

    ImGui::PopID();
    return changed;
}

} // namespace Ui
