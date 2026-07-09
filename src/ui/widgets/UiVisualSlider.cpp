#include "UiVisualSlider.h"
#include "../style/UiTokens.h"
#include "../style/UiMotion.h"
#include <algorithm>
#include <cmath>

namespace Ui {

struct SliderAnim {
    AnimFloat display;
    SliderAnim() { display.Snap(0.f); }
};

bool VisualSlider(const char* id, float* value, ImVec2 size,
                  VisualSliderSkin skin, const float rgb[3], const char* tooltip,
                  float defaultValue, float snapStep) {
    if (!value) return false;
    ImGui::PushID(id);
    auto& T = Tokens();
    ImGuiID gid = ImGui::GetID("##vsl_anim");
    auto& anim = AnimState<SliderAnim>(gid);
    if (!anim.display.active && std::fabs(anim.display.value - *value) > 0.001f && !ImGui::IsMouseDown(0))
        anim.display.SetTarget(*value, T.durFast, EaseKind::EaseOutCubic);
    anim.display.Update(DeltaTime());

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##vsl", size);
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    bool changed = false;

    if (hovered && !active && ImGui::IsKeyPressed(ImGuiKey_Backspace) && defaultValue >= 0.f) {
        *value = defaultValue;
        anim.display.SetTarget(*value, T.durMed, EaseKind::EaseOutCubic);
        changed = true;
    }

    if (active || (hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
        float t = (ImGui::GetIO().MousePos.x - pos.x) / std::max(1.f, size.x);
        t = Clampf(t, 0.f, 1.f);
        if (ImGui::GetIO().KeyCtrl && snapStep > 0.f) {
            t = std::round(t / snapStep) * snapStep;
            t = Clampf(t, 0.f, 1.f);
        }
        if (std::fabs(t - *value) > 1e-5f) {
            *value = t;
            anim.display.Snap(t); // follow drag tightly
            changed = true;
        }
    }

    float vis = (active ? *value : anim.display.value);
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
        float fx = pos.x + size.x * vis;
        dl->AddRectFilled(p1, ImVec2(fx, p2.y), T.ColU32(ImVec4(T.accent.x, T.accent.y, T.accent.z, 0.7f)), T.rSm);
        dl->AddRect(p1, p2, T.ColU32(T.strokeHairline), T.rSm);
    }

    float tx = pos.x + size.x * vis;
    float th = size.y * 0.5f;
    dl->AddCircleFilled(ImVec2(tx, pos.y + size.y * 0.5f), th * 0.65f, IM_COL32(255, 255, 255, 240));
    dl->AddCircle(ImVec2(tx, pos.y + size.y * 0.5f), th * 0.65f, T.ColU32(T.strokeActive), 16, 1.5f);

    if (tooltip && hovered)
        ImGui::SetTooltip("%s: %.2f\nBackspace: default  ·  Ctrl: snap", tooltip, *value);

    ImGui::PopID();
    return changed;
}

bool SmartSliderFloat(const char* label, float* v, float vMin, float vMax,
                      float defaultValue, float snapStep, const char* format) {
    if (!v) return false;
    bool changed = false;
    if (ImGui::SliderFloat(label, v, vMin, vMax, format)) {
        if (ImGui::GetIO().KeyCtrl && snapStep > 0.f) {
            *v = std::round((*v - vMin) / snapStep) * snapStep + vMin;
            *v = Clampf(*v, vMin, vMax);
        }
        changed = true;
    }
    if (ImGui::IsItemHovered()) {
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
            *v = defaultValue;
            changed = true;
        }
        ImGui::SetTooltip("Backspace: default  ·  Ctrl: snap");
    }
    return changed;
}

} // namespace Ui
