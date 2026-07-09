#include "UiVisualSlider.h"
#include "../style/UiTokens.h"
#include "../style/UiMotion.h"
#include <algorithm>
#include <cmath>

namespace Ui {

bool VisualSlider(const char* id, float* value, ImVec2 size,
                  VisualSliderSkin skin, const float rgb[3], const char* tooltip) {
    if (!value) return false;
    ImGui::PushID(id);
    auto& T = Tokens();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##vsl", size);
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    bool changed = false;

    if (active || (hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
        float t = (ImGui::GetIO().MousePos.x - pos.x) / std::max(1.f, size.x);
        t = Clampf(t, 0.f, 1.f);
        if (std::fabs(t - *value) > 1e-5f) {
            *value = t;
            changed = true;
        }
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p1 = pos, p2(pos.x + size.x, pos.y + size.y);

    if (skin == VisualSliderSkin::HueStrip) {
        const int segs = 32;
        for (int i = 0; i < segs; ++i) {
            float h0 = (float)i / segs;
            float r, g, b;
            ImGui::ColorConvertHSVtoRGB(h0, 1.f, 1.f, r, g, b);
            ImU32 c = IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), 255);
            float x0 = pos.x + size.x * i / segs;
            float x1 = pos.x + size.x * (i + 1) / segs;
            dl->AddRectFilled(ImVec2(x0, pos.y), ImVec2(x1, pos.y + size.y), c, 0);
        }
        dl->AddRect(p1, p2, T.ColU32(T.strokeHairline), T.rSm);
    } else if (skin == VisualSliderSkin::OpacityChecker) {
        // checker
        float cs = 6.f;
        for (float y = pos.y; y < p2.y; y += cs) {
            for (float x = pos.x; x < p2.x; x += cs) {
                bool dark = (int((x - pos.x) / cs) + int((y - pos.y) / cs)) & 1;
                dl->AddRectFilled(ImVec2(x, y), ImVec2(std::min(x + cs, p2.x), std::min(y + cs, p2.y)),
                    dark ? IM_COL32(90, 90, 90, 255) : IM_COL32(160, 160, 160, 255));
            }
        }
        float r = rgb ? rgb[0] : 1.f, g = rgb ? rgb[1] : 1.f, b = rgb ? rgb[2] : 1.f;
        const int segs = 24;
        for (int i = 0; i < segs; ++i) {
            float a1 = (float)(i + 1) / segs;
            float x0 = pos.x + size.x * (float)i / segs;
            float x1 = pos.x + size.x * (float)(i + 1) / segs;
            dl->AddRectFilled(ImVec2(x0, pos.y), ImVec2(x1, p2.y),
                IM_COL32((int)(r*255),(int)(g*255),(int)(b*255), (int)(a1 * 255)));
        }
        dl->AddRect(p1, p2, T.ColU32(T.strokeHairline), T.rSm);
    } else {
        dl->AddRectFilled(p1, p2, T.ColU32(ImVec4(0.2f, 0.2f, 0.22f, 1.f)), T.rSm);
        float fx = pos.x + size.x * (*value);
        dl->AddRectFilled(p1, ImVec2(fx, p2.y), T.ColU32(ImVec4(T.accent.x, T.accent.y, T.accent.z, 0.7f)), T.rSm);
        dl->AddRect(p1, p2, T.ColU32(T.strokeHairline), T.rSm);
    }

    // Thumb
    float tx = pos.x + size.x * (*value);
    float th = size.y * 0.5f;
    dl->AddCircleFilled(ImVec2(tx, pos.y + size.y * 0.5f), th * 0.65f, IM_COL32(255, 255, 255, 240));
    dl->AddCircle(ImVec2(tx, pos.y + size.y * 0.5f), th * 0.65f, T.ColU32(T.strokeActive), 16, 1.5f);

    if (tooltip && hovered)
        ImGui::SetTooltip("%s: %.2f", tooltip, *value);

    ImGui::PopID();
    return changed;
}

} // namespace Ui
