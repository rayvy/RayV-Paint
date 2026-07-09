#include "UiVisualSlider.h"
#include "UiTooltip.h"
#include "../style/UiTokens.h"
#include "../style/UiMotion.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

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
            changed = true;
        }
    }

    // Thumb always eases toward value (including while dragging — no hard Snap)
    if (std::fabs(anim.display.to - *value) > 1e-4f || (!anim.display.active && std::fabs(anim.display.value - *value) > 1e-4f))
        anim.display.SetTarget(*value, active ? T.durFast * 0.55f : T.durFast, EaseKind::EaseOutCubic);
    anim.display.Update(DeltaTime());
    float vis = anim.display.value;
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

    if (tooltip && hovered) {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "%s: %.2f\nBackspace: default  ·  Ctrl: snap", tooltip, *value);
        Tooltip(buf);
    }

    ImGui::PopID();
    return changed;
}

bool SmartSliderFloat(const char* label, float* v, float vMin, float vMax,
                      float defaultValue, float snapStep, const char* format) {
    if (!v) return false;
    auto& T = Tokens();
    ImGui::PushID(label);
    ImGuiID gid = ImGui::GetID("##ssl_anim");
    auto& anim = AnimState<SliderAnim>(gid);
    // Normalize 0..1 for animation, map back for display
    float range = std::max(1e-6f, vMax - vMin);
    float tVal = (*v - vMin) / range;
    if (std::fabs(anim.display.to - tVal) > 1e-4f || (!anim.display.active && std::fabs(anim.display.value - tVal) > 1e-4f))
        anim.display.SetTarget(tVal, T.durFast * 0.65f, EaseKind::EaseOutCubic);
    anim.display.Update(DeltaTime());

    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::CalcItemWidth();
    if (w < 40.f) w = ImGui::GetContentRegionAvail().x;
    float h = ImGui::GetFrameHeight();
    ImGui::InvisibleButton("##ssl", ImVec2(w, h));
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    bool changed = false;

    if (hovered && ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
        *v = defaultValue;
        changed = true;
    }
    if (active || (hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
        float t = Clampf((ImGui::GetIO().MousePos.x - pos.x) / std::max(1.f, w), 0.f, 1.f);
        if (ImGui::GetIO().KeyCtrl && snapStep > 0.f) {
            float raw = vMin + t * range;
            raw = std::round((raw - vMin) / snapStep) * snapStep + vMin;
            t = Clampf((raw - vMin) / range, 0.f, 1.f);
        }
        float nv = vMin + t * range;
        if (std::fabs(nv - *v) > 1e-5f) {
            *v = nv;
            anim.display.SetTarget(t, T.durFast * 0.55f, EaseKind::EaseOutCubic);
            changed = true;
        }
    }

    float vis = anim.display.value;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p2(pos.x + w, pos.y + h);
    dl->AddRectFilled(pos, p2, T.ColU32(ImVec4(0.18f, 0.18f, 0.20f, 1.f)), T.rSm);
    float fx = pos.x + w * vis;
    dl->AddRectFilled(pos, ImVec2(fx, p2.y), T.ColU32(ImVec4(T.accent.x, T.accent.y, T.accent.z, 0.65f)), T.rSm);
    dl->AddRect(pos, p2, T.ColU32(T.strokeHairline), T.rSm);
    float th = h * 0.5f;
    dl->AddCircleFilled(ImVec2(fx, pos.y + h * 0.5f), th * 0.55f, IM_COL32(255, 255, 255, 240));
    dl->AddCircle(ImVec2(fx, pos.y + h * 0.5f), th * 0.55f, T.ColU32(T.strokeActive), 12, 1.25f);

    // Value label
    char buf[64];
    std::snprintf(buf, sizeof(buf), format ? format : "%.2f", *v);
    ImVec2 ts = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(pos.x + (w - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f),
                T.ColU32(T.textPrimary), buf);

    if (hovered)
        Tooltip("Backspace: default  ·  Ctrl: snap");
    ImGui::PopID();
    return changed;
}

} // namespace Ui
