#include "UiTooltip.h"
#include "../style/UiTokens.h"
#include "../style/UiMotion.h"
#include <cstring>
#include <cmath>
#include <cstdio>
#include <algorithm>

namespace Ui {

static float s_Delay = 1.0f;
static ImGuiID s_HoverId = 0;
static float s_HoverTime = 0.f;
static float s_ShowAnim = 0.f;
static char s_Text[512] = {};
static bool s_WantShow = false;

void TooltipSetDelay(float seconds) { s_Delay = std::max(0.05f, seconds); }

void Tooltip(const char* text) {
    if (!text || !text[0]) return;
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) return;
    // Abort if user is clicking/holding
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Right))
        return;

    ImGuiID id = ImGui::GetItemID();
    float dt = DeltaTime();
    if (id != s_HoverId) {
        s_HoverId = id;
        s_HoverTime = 0.f;
        s_ShowAnim = 0.f;
    }
    s_HoverTime += dt;
    std::snprintf(s_Text, sizeof(s_Text), "%s", text);
    s_WantShow = (s_HoverTime >= s_Delay);
}

void TooltipEndFrame() {
    auto& T = Tokens();
    float dt = DeltaTime();
    if (s_WantShow && s_Text[0]) {
        s_ShowAnim = Clampf(s_ShowAnim + dt / T.durMed, 0.f, 1.f);
    } else {
        s_ShowAnim = Clampf(s_ShowAnim - dt / T.durFast, 0.f, 1.f);
        if (s_ShowAnim <= 0.f) {
            s_HoverId = 0;
            s_HoverTime = 0.f;
            s_Text[0] = 0;
            return;
        }
    }
    if (s_ShowAnim <= 0.001f || !s_Text[0]) return;

    float e = Ease(EaseKind::EaseOutBack, s_ShowAnim);
    // squash: start slightly tall/narrow → settle
    float scaleX = 0.92f + 0.08f * e;
    float scaleY = 1.08f - 0.08f * e;
    float alpha = Ease(EaseKind::EaseOutCubic, s_ShowAnim);

    ImVec2 mouse = ImGui::GetIO().MousePos;
    ImVec2 textSize = ImGui::CalcTextSize(s_Text);
    ImVec2 pad(10.f, 8.f);
    ImVec2 box(textSize.x * scaleX + pad.x * 2, textSize.y * scaleY + pad.y * 2);
    ImVec2 pos(mouse.x + 14.f, mouse.y + 18.f);

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImU32 bg = ImGui::ColorConvertFloat4ToU32(ImVec4(T.bgElevated.x, T.bgElevated.y, T.bgElevated.z, 0.94f * alpha));
    ImU32 stroke = ImGui::ColorConvertFloat4ToU32(ImVec4(T.strokeHairline.x, T.strokeHairline.y, T.strokeHairline.z, alpha));
    ImU32 tc = ImGui::ColorConvertFloat4ToU32(ImVec4(T.textPrimary.x, T.textPrimary.y, T.textPrimary.z, alpha));
    dl->AddRectFilled(pos, ImVec2(pos.x + box.x, pos.y + box.y), bg, T.rMd);
    dl->AddRect(pos, ImVec2(pos.x + box.x, pos.y + box.y), stroke, T.rMd, 0, 1.0f);
    dl->AddText(ImVec2(pos.x + pad.x, pos.y + pad.y), tc, s_Text);

    s_WantShow = false; // must re-request each frame
}

} // namespace Ui
