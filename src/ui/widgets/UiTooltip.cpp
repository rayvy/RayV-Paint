#include "UiTooltip.h"
#include "../style/UiTokens.h"
#include "../style/UiMotion.h"
#include <cstring>
#include <cmath>
#include <cstdio>
#include <algorithm>

namespace Ui {

// Hover-delay tooltip state machine:
//   - Tooltip() only arms request for current item + accumulates hover time
//   - TooltipEndFrame() fades/draws; must NOT reset hover timer while still armed
static float s_Delay = 1.0f;
static ImGuiID s_HoverId = 0;
static float s_HoverTime = 0.f;
static float s_ShowAnim = 0.f;
static char s_Text[512] = {};
static bool s_ArmedThisFrame = false;

void TooltipSetDelay(float seconds) {
    s_Delay = std::max(0.05f, seconds);
}

void Tooltip(const char* text) {
    if (!text || !text[0]) return;
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) return;
    // No tooltip while pressing (click / hold menus)
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Right))
        return;

    ImGuiID id = ImGui::GetItemID();
    if (id == 0) return;

    float dt = DeltaTime();
    if (id != s_HoverId) {
        s_HoverId = id;
        s_HoverTime = 0.f;
        // Keep brief fade if switching items quickly
        if (s_ShowAnim > 0.f)
            s_ShowAnim = 0.f;
    }

    s_HoverTime += dt;
    std::snprintf(s_Text, sizeof(s_Text), "%s", text);
    s_ArmedThisFrame = true;
}

void TooltipEndFrame() {
    auto& T = Tokens();
    float dt = DeltaTime();

    const bool wantVisible = s_ArmedThisFrame && s_Text[0] && (s_HoverTime >= s_Delay);

    if (wantVisible) {
        s_ShowAnim = Clampf(s_ShowAnim + dt / std::max(0.001f, T.durMed), 0.f, 1.f);
    } else {
        s_ShowAnim = Clampf(s_ShowAnim - dt / std::max(0.001f, T.durFast), 0.f, 1.f);
        // Only clear hover tracking when fully faded AND not still hovering
        if (!s_ArmedThisFrame && s_ShowAnim <= 0.f) {
            s_HoverId = 0;
            s_HoverTime = 0.f;
            s_Text[0] = 0;
            s_ArmedThisFrame = false;
            return;
        }
    }

    // Clear arm flag for next frame (Tooltip must re-arm every frame while hovered)
    s_ArmedThisFrame = false;

    if (s_ShowAnim <= 0.001f || !s_Text[0]) return;

    float e = Ease(EaseKind::EaseOutBack, s_ShowAnim);
    float scaleX = 0.92f + 0.08f * e;
    float scaleY = 1.08f - 0.08f * e;
    float alpha = Ease(EaseKind::EaseOutCubic, s_ShowAnim);

    ImVec2 mouse = ImGui::GetIO().MousePos;
    ImVec2 textSize = ImGui::CalcTextSize(s_Text, nullptr, false, 0.f);
    ImVec2 pad(10.f, 8.f);
    ImVec2 box(textSize.x * scaleX + pad.x * 2.f, textSize.y * scaleY + pad.y * 2.f);
    ImVec2 pos(mouse.x + 14.f, mouse.y + 18.f);

    if (ImGuiViewport* vp = ImGui::GetMainViewport()) {
        ImVec2 br(vp->WorkPos.x + vp->WorkSize.x - 8.f, vp->WorkPos.y + vp->WorkSize.y - 8.f);
        if (pos.x + box.x > br.x) pos.x = br.x - box.x;
        if (pos.y + box.y > br.y) pos.y = br.y - box.y;
        if (pos.x < vp->WorkPos.x + 4.f) pos.x = vp->WorkPos.x + 4.f;
        if (pos.y < vp->WorkPos.y + 4.f) pos.y = vp->WorkPos.y + 4.f;
    }

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImU32 bg = ImGui::ColorConvertFloat4ToU32(
        ImVec4(T.bgElevated.x, T.bgElevated.y, T.bgElevated.z, 0.94f * alpha));
    ImU32 stroke = ImGui::ColorConvertFloat4ToU32(
        ImVec4(T.strokeHairline.x, T.strokeHairline.y, T.strokeHairline.z, alpha));
    ImU32 tc = ImGui::ColorConvertFloat4ToU32(
        ImVec4(T.textPrimary.x, T.textPrimary.y, T.textPrimary.z, alpha));

    dl->AddRectFilled(pos, ImVec2(pos.x + box.x, pos.y + box.y), bg, T.rMd);
    dl->AddRect(pos, ImVec2(pos.x + box.x, pos.y + box.y), stroke, T.rMd, 0, 1.0f);
    dl->AddText(ImVec2(pos.x + pad.x, pos.y + pad.y), tc, s_Text);
}

} // namespace Ui
