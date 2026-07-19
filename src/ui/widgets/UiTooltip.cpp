#include "UiTooltip.h"
#include "../style/UiTokens.h"
#include "../style/UiMotion.h"
#include <imgui_internal.h>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <vector>
#include <string>

namespace Ui {

// Hover-delay tooltip state machine:
//   - Tooltip() only arms request for current item + accumulates hover time
//   - TooltipEndFrame() fades/draws; must NOT reset hover timer while still armed
static float s_Delay = 0.40f;
static ImGuiID s_HoverId = 0;
static float s_HoverTime = 0.f;
static float s_ShowAnim = 0.f;
static char s_Text[1024] = {};
static bool s_ArmedThisFrame = false;

void TooltipSetDelay(float seconds) {
    s_Delay = std::max(0.05f, seconds);
}

void Tooltip(const char* text) {
    if (!text || !text[0]) return;
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled |
                              ImGuiHoveredFlags_AllowWhenOverlapped |
                              ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
        return;
    // No tooltip while pressing (click / hold menus)
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Right))
        return;

    // Prefer item ID; fall back so TextUnformatted / non-interactive rows still get tips.
    ImGuiID id = ImGui::GetItemID();
    if (id == 0) {
        // Stable-ish id from label + rect (so multi-line tips still track one hover)
        ImVec2 mn = ImGui::GetItemRectMin();
        ImVec2 mx = ImGui::GetItemRectMax();
        id = ImHashStr(text);
        id = ImHashData(&mn, sizeof(mn), id);
        id = ImHashData(&mx, sizeof(mx), id);
        if (id == 0) id = 0x54697075u; // "Tipu" — never zero
    }

    float dt = DeltaTime();
    if (id != s_HoverId) {
        s_HoverId = id;
        s_HoverTime = 0.f;
        if (s_ShowAnim > 0.f)
            s_ShowAnim = 0.f;
    }

    s_HoverTime += dt;
    std::snprintf(s_Text, sizeof(s_Text), "%s", text);
    s_ArmedThisFrame = true;
}

static void MeasureMultiline(const char* text, float wrapW, ImVec2& outSize, std::vector<std::string>& outLines) {
    outLines.clear();
    outSize = ImVec2(0, 0);
    if (!text || !*text) return;

    // Split on \n; optionally soft-wrap long lines
    const char* p = text;
    while (*p) {
        const char* nl = std::strchr(p, '\n');
        std::string line = nl ? std::string(p, nl) : std::string(p);
        if (wrapW > 1.f && !line.empty()) {
            ImVec2 ts = ImGui::CalcTextSize(line.c_str());
            if (ts.x > wrapW) {
                // crude word wrap
                std::string rest = line;
                while (!rest.empty()) {
                    size_t fit = rest.size();
                    while (fit > 1 && ImGui::CalcTextSize(rest.substr(0, fit).c_str()).x > wrapW)
                        --fit;
                    // back up to space if possible
                    size_t sp = rest.rfind(' ', fit);
                    if (sp != std::string::npos && sp > 8 && fit < rest.size())
                        fit = sp;
                    outLines.push_back(rest.substr(0, fit));
                    while (fit < rest.size() && rest[fit] == ' ') ++fit;
                    rest = rest.substr(fit);
                }
            } else {
                outLines.push_back(std::move(line));
            }
        } else {
            outLines.push_back(std::move(line));
        }
        if (!nl) break;
        p = nl + 1;
    }
    float lineH = ImGui::GetTextLineHeightWithSpacing();
    float maxW = 0.f;
    for (const auto& L : outLines) {
        ImVec2 ts = ImGui::CalcTextSize(L.c_str());
        maxW = std::max(maxW, ts.x);
    }
    outSize.x = maxW;
    outSize.y = lineH * (float)std::max<size_t>(1, outLines.size());
}

void TooltipEndFrame() {
    auto& T = Tokens();
    float dt = DeltaTime();

    const bool wantVisible = s_ArmedThisFrame && s_Text[0] && (s_HoverTime >= s_Delay);

    if (wantVisible) {
        s_ShowAnim = Clampf(s_ShowAnim + dt / std::max(0.001f, T.durMed), 0.f, 1.f);
    } else {
        s_ShowAnim = Clampf(s_ShowAnim - dt / std::max(0.001f, T.durFast), 0.f, 1.f);
        if (!s_ArmedThisFrame && s_ShowAnim <= 0.f) {
            s_HoverId = 0;
            s_HoverTime = 0.f;
            s_Text[0] = 0;
            s_ArmedThisFrame = false;
            return;
        }
    }

    s_ArmedThisFrame = false;

    if (s_ShowAnim <= 0.001f || !s_Text[0]) return;

    float e = Ease(EaseKind::EaseOutCubic, s_ShowAnim);
    float alpha = e;

    const float wrapW = 320.f;
    std::vector<std::string> lines;
    ImVec2 textSize;
    MeasureMultiline(s_Text, wrapW, textSize, lines);

    ImVec2 pad(10.f, 8.f);
    ImVec2 box(textSize.x + pad.x * 2.f, textSize.y + pad.y * 2.f);
    ImVec2 mouse = ImGui::GetIO().MousePos;
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

    float y = pos.y + pad.y;
    float lineH = ImGui::GetTextLineHeightWithSpacing();
    for (const auto& L : lines) {
        dl->AddText(ImVec2(pos.x + pad.x, y), tc, L.c_str());
        y += lineH;
    }
}

} // namespace Ui
