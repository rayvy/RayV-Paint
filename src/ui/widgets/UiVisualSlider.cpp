#include "UiVisualSlider.h"
#include "UiTooltip.h"
#include "../style/UiTokens.h"
#include "../style/UiMotion.h"
#include "../../core/ops/AppContext.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace Ui {

struct SliderAnim {
    AnimFloat display;
    SliderAnim() { display.Snap(0.f); }
};

// Shared text-edit state keyed by ImGuiID (one active editor at a time is fine).
struct SliderTextEdit {
    bool active = false;
    char buf[64] = {};
    ImGuiID owner = 0;
};

static SliderTextEdit& TextEditState() {
    static SliderTextEdit s;
    return s;
}

// Parse float; returns false on empty / pure garbage (no digits consumed).
static bool TryParseFloat(const char* s, float& out) {
    if (!s) return false;
    while (*s == ' ' || *s == '\t') ++s;
    if (!*s) return false;
    char* end = nullptr;
    float v = std::strtof(s, &end);
    if (end == s) return false; // no conversion
    // Trailing junk (except whitespace) → reject
    while (end && (*end == ' ' || *end == '\t')) ++end;
    if (end && *end != '\0') return false;
    if (!std::isfinite(v)) return false;
    out = v;
    return true;
}

static bool BeginExactEdit(ImGuiID id, float current, const char* format) {
    auto& te = TextEditState();
    te.active = true;
    te.owner = id;
    std::snprintf(te.buf, sizeof(te.buf), format ? format : "%.3f", current);
    // Strip format artifacts like "px" if any non-numeric suffix left by format — keep simple.
    return true;
}

static bool TickExactEditFloat(ImGuiID id, float* v, float vMin, float vMax, bool& changed) {
    auto& te = TextEditState();
    if (!te.active || te.owner != id) return false;

    core::ops::AppContext::NotifyUiKeyboardCapture();
    ImGui::SetNextItemWidth(-1.f);
    ImGui::SetKeyboardFocusHere();
    bool enter = ImGui::InputText("##ssl_edit", te.buf, sizeof(te.buf),
        ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue |
        ImGuiInputTextFlags_AutoSelectAll);
    bool deactivate = enter || ImGui::IsKeyPressed(ImGuiKey_Escape) ||
        (!ImGui::IsItemActive() && !ImGui::IsItemFocused() && ImGui::IsMouseClicked(0));

    if (enter) {
        float parsed = 0.f;
        if (TryParseFloat(te.buf, parsed)) {
            *v = std::clamp(parsed, vMin, vMax);
            changed = true;
        }
        // invalid → keep old *v
        te.active = false;
        return true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        te.active = false;
        return true;
    }
    if (deactivate && !ImGui::IsItemActive()) {
        // Click away: try commit
        float parsed = 0.f;
        if (TryParseFloat(te.buf, parsed)) {
            *v = std::clamp(parsed, vMin, vMax);
            changed = true;
        }
        te.active = false;
        return true;
    }
    return true; // still editing
}

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
    ImGuiID editId = ImGui::GetID("##vsl_edit");

    auto& te = TextEditState();
    if (te.active && te.owner == editId) {
        // Overlay text field on same rect
        ImGui::SetCursorScreenPos(pos);
        ImGui::PushItemWidth(size.x);
        TickExactEditFloat(editId, value, 0.f, 1.f, changed);
        ImGui::PopItemWidth();
        if (changed) anim.display.SetTarget(*value, T.durMed, EaseKind::EaseOutCubic);
        ImGui::PopID();
        return changed;
    }

    if (hovered && !active && ImGui::IsKeyPressed(ImGuiKey_Backspace) && defaultValue >= 0.f) {
        core::ops::AppContext::NotifyUiKeyboardCapture();
        *value = defaultValue;
        anim.display.SetTarget(*value, T.durMed, EaseKind::EaseOutCubic);
        changed = true;
    }

    // Exact edit: double-click or Ctrl+click
    if (hovered && (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ||
                    (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::GetIO().KeyCtrl))) {
        BeginExactEdit(editId, *value, "%.3f");
        ImGui::PopID();
        return false;
    }

    if (active || (hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::GetIO().KeyCtrl)) {
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
        std::snprintf(buf, sizeof(buf), "%s: %.2f\nDouble-click / Ctrl+click: exact  ·  Backspace: default  ·  Ctrl+drag: snap",
                      tooltip, *value);
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
    ImGuiID editId = ImGui::GetID("##ssl_tex");
    auto& anim = AnimState<SliderAnim>(gid);
    float range = std::max(1e-6f, vMax - vMin);
    float tVal = (*v - vMin) / range;
    if (std::fabs(anim.display.to - tVal) > 1e-4f || (!anim.display.active && std::fabs(anim.display.value - tVal) > 1e-4f))
        anim.display.SetTarget(tVal, T.durFast * 0.65f, EaseKind::EaseOutCubic);
    anim.display.Update(DeltaTime());

    // Label column: ImGui-style "Visible##id" — show text before ## only.
    // Existing call sites (Layer FX) already place labels; hide when pure ##id
    // or when label contains ## (full label was for ID only in InvisibleButton era).
    // We still PushID(label) so IDs stay unique. Do NOT draw "Opacity##sh" raw.
    const bool pureHashId = label && label[0] == '#' && label[1] == '#';
    // Prefer not drawing separate label — callers typically use SetNextItemWidth only.
    // Drawing label broke Layer FX which passes "Opacity##sh" as the control id.
    (void)pureHashId;

    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::CalcItemWidth();
    if (w < 40.f) w = ImGui::GetContentRegionAvail().x;
    float h = ImGui::GetFrameHeight();

    bool changed = false;
    auto& te = TextEditState();
    if (te.active && te.owner == editId) {
        ImGui::SetNextItemWidth(w);
        TickExactEditFloat(editId, v, vMin, vMax, changed);
        if (changed) {
            float t = (*v - vMin) / range;
            anim.display.SetTarget(t, T.durFast * 0.55f, EaseKind::EaseOutCubic);
        }
        ImGui::PopID();
        return changed;
    }

    ImGui::InvisibleButton("##ssl", ImVec2(w, h));
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();

    if (hovered && ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
        core::ops::AppContext::NotifyUiKeyboardCapture();
        *v = defaultValue;
        changed = true;
    }

    if (hovered && (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ||
                    (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::GetIO().KeyCtrl))) {
        // Format current without unit clutter for edit buffer
        char fmtBuf[64];
        std::snprintf(fmtBuf, sizeof(fmtBuf), format ? format : "%.3f", *v);
        // If format embeds non-numeric suffix, still open with numeric best-effort
        BeginExactEdit(editId, *v, "%.6g");
        ImGui::PopID();
        return false;
    }

    if (active || (hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::GetIO().KeyCtrl)) {
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

    char buf[64];
    std::snprintf(buf, sizeof(buf), format ? format : "%.2f", *v);
    ImVec2 ts = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(pos.x + (w - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f),
                T.ColU32(T.textPrimary), buf);

    if (hovered)
        Tooltip("Double-click / Ctrl+click: exact value\nBackspace: default  ·  Ctrl+drag: snap");
    ImGui::PopID();
    return changed;
}

bool SmartSliderInt(const char* label, int* v, int vMin, int vMax, int defaultValue, int snapStep) {
    if (!v) return false;
    float fv = (float)*v;
    float fmin = (float)vMin, fmax = (float)vMax;
    float def = (float)defaultValue;
    float step = (float)std::max(1, snapStep);
    // Text path via SmartSliderFloat; commit rounds to int.
    if (SmartSliderFloat(label, &fv, fmin, fmax, def, step, "%.0f")) {
        *v = std::clamp((int)std::lround(fv), vMin, vMax);
        return true;
    }
    // Keep *v in sync if float path didn't change but display drifted
    int rounded = std::clamp((int)std::lround(fv), vMin, vMax);
    if (rounded != *v) { *v = rounded; return true; }
    return false;
}

} // namespace Ui
