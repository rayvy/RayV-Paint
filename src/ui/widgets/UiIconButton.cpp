#include "UiIconButton.h"
#include "../style/UiMotion.h"
#include <cmath>
#include <algorithm>

namespace Ui {

struct IconBtnState {
    AnimFloat scale;
    bool wasDown = false;
    IconBtnState() { scale.Snap(1.f); }
};

static IconButtonResult IconButtonImpl(const char* id, const SvgIcon* icon,
                                       ImVec2 size, const char* tooltip,
                                       bool enabled, bool active) {
    IconButtonResult res;
    ImGui::PushID(id);
    ImGuiID gid = ImGui::GetID("##uibtn");
    auto& st = AnimState<IconBtnState>(gid);
    float dt = DeltaTime();
    auto& T = Tokens();

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##hit", size);
    res.hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    bool down = enabled && ImGui::IsItemActive();
    res.held = down;
    bool clicked = enabled && ImGui::IsItemClicked(ImGuiMouseButton_Left);
    res.clicked = clicked;

    // Motion: press / release bounce
    if (down) {
        st.scale.SetTarget(T.pressScale, T.durFast * 0.85f, EaseKind::EaseOutCubic);
        st.wasDown = true;
    } else if (st.wasDown) {
        // release bounce
        st.scale.SetTarget(T.bounceOvershoot, T.durFast, EaseKind::EaseOutBack);
        st.wasDown = false;
        // then settle to 1 via second retarget after overshoot — simple chain:
        // if currently going to overshoot and not active, we'll snap follow-up in update
    } else if (!st.scale.active && std::fabs(st.scale.value - 1.f) > 0.01f) {
        st.scale.SetTarget(1.f, T.durFast, EaseKind::EaseOutCubic);
    } else if (!st.scale.active && st.scale.value > 1.001f) {
        st.scale.SetTarget(1.f, T.durMed * 0.6f, EaseKind::EaseOutCubic);
    }
    // After EaseOutBack lands above 1, pull back
    if (!down && !st.scale.active && st.scale.value > 1.001f)
        st.scale.SetTarget(1.f, T.durFast, EaseKind::EaseOutCubic);

    st.scale.Update(dt);
    float sc = st.scale.value;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);
    float side = std::min(size.x, size.y);
    float drawSide = side * sc * 0.72f; // padding inside button

    // Background
    ImU32 bg = T.ColU32(active ? ImVec4(T.accent.x, T.accent.y, T.accent.z, 0.35f)
                               : (res.hovered && enabled ? ImVec4(1, 1, 1, T.isDark ? 0.08f : 0.12f)
                                                         : ImVec4(0, 0, 0, 0)));
    if ((bg >> 24) > 0)
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bg, T.rSm);

    if (active)
        dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), T.ColU32(T.strokeActive), T.rSm, 0, 1.5f);
    else if (res.hovered && enabled)
        dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), T.ColU32(T.strokeHairline), T.rSm, 0, 1.0f);

    ImU32 tint = T.ColU32(enabled ? (active ? T.iconTint : T.iconTint)
                                  : T.iconTintMuted);
    if (icon && icon->valid())
        SvgIconCache::Get().DrawCentered(icon, c, drawSide, tint);
    else {
        // minimal fallback diamond
        float h = drawSide * 0.35f;
        dl->AddQuadFilled(ImVec2(c.x, c.y - h), ImVec2(c.x + h, c.y),
                          ImVec2(c.x, c.y + h), ImVec2(c.x - h, c.y), tint);
    }

    if (tooltip && res.hovered)
        ImGui::SetTooltip("%s", tooltip);

    ImGui::PopID();
    return res;
}

IconButtonResult IconButton(const char* id, const char* iconLogicalName,
                            ImVec2 size, const char* tooltip, bool enabled, bool active) {
    int px = (int)std::max(32.f, std::max(size.x, size.y) * 2.f);
    const SvgIcon* icon = SvgIconCache::Get().Get(iconLogicalName, px);
    return IconButtonImpl(id, icon, size, tooltip, enabled, active);
}

IconButtonResult IconButton(const char* id, const SvgIcon* icon,
                            ImVec2 size, const char* tooltip, bool enabled, bool active) {
    return IconButtonImpl(id, icon, size, tooltip, enabled, active);
}

} // namespace Ui
