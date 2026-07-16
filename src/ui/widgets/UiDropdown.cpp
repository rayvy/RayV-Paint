#include "UiDropdown.h"
#include "UiIconButton.h"
#include "../style/UiMotion.h"
#include "../style/UiTokens.h"
#include <cmath>
#include <algorithm>
#include <cstdio>

namespace Ui {

struct DropdownState {
    AnimBool open;
    bool holdMode = false;
    bool pressActive = false;
    float holdTimer = 0.f;
    int hoverIdx = -1;
    bool openedThisGesture = false;
    float scrollY = 0.f;
};

// Strong frosted fill clipped to the popup rect only — no scrim, no outline, no rounding.
static void DrawFrostedPanel(ImDrawList* dl, ImVec2 p1, ImVec2 p2, float alpha) {
    // Stacked translucent layers simulate strong blur / glass inside the box.
    const int layers = 10;
    for (int i = 0; i < layers; ++i) {
        float t = (float)i / (float)(layers - 1);
        float a = alpha * (0.10f + 0.10f * t);
        ImU32 c = IM_COL32(18, 18, 22, (int)(a * 255.f));
        dl->AddRectFilled(p1, p2, c, 0.f);
    }
    // Final dense plate
    ImU32 plate = IM_COL32(22, 22, 28, (int)(std::min(0.92f, alpha * 0.88f) * 255.f));
    dl->AddRectFilled(p1, p2, plate, 0.f);
    // Soft top sheen (still no outline)
    ImU32 sheen = IM_COL32(255, 255, 255, (int)(18.f * alpha));
    dl->AddRectFilledMultiColor(p1, ImVec2(p2.x, p1.y + 10.f),
        sheen, sheen, IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));
}

// Fit panel into work area: prefer below trigger, flip above, then clamp + scroll.
static void FitPanelToScreen(ImVec2 triggerMin, ImVec2 triggerMax,
                             float panelW, float& panelH, float fullContentH,
                             ImVec2& panelPos, float& scrollY) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float pad = 6.f;
    const float workMinX = vp->WorkPos.x + pad;
    const float workMinY = vp->WorkPos.y + pad;
    const float workMaxX = vp->WorkPos.x + vp->WorkSize.x - pad;
    const float workMaxY = vp->WorkPos.y + vp->WorkSize.y - pad;
    const float maxH = std::max(80.f, workMaxY - workMinY);

    panelH = std::min(fullContentH, maxH);

    // Prefer below trigger
    panelPos = ImVec2(triggerMin.x, triggerMax.y + 4.f);

    // Horizontal clamp
    if (panelPos.x + panelW > workMaxX)
        panelPos.x = workMaxX - panelW;
    if (panelPos.x < workMinX)
        panelPos.x = workMinX;

    // Vertical: flip above if not enough space below
    const float spaceBelow = workMaxY - panelPos.y;
    const float spaceAbove = triggerMin.y - 4.f - workMinY;
    if (panelH > spaceBelow && spaceAbove > spaceBelow) {
        panelPos.y = triggerMin.y - 4.f - panelH;
        if (panelPos.y < workMinY) {
            panelPos.y = workMinY;
            panelH = std::min(panelH, workMaxY - panelPos.y);
        }
    } else if (panelPos.y + panelH > workMaxY) {
        panelPos.y = std::max(workMinY, workMaxY - panelH);
        panelH = std::min(panelH, workMaxY - panelPos.y);
    }
    if (panelPos.y < workMinY)
        panelPos.y = workMinY;

    // Scroll range
    float maxScroll = std::max(0.f, fullContentH - panelH);
    scrollY = Clampf(scrollY, 0.f, maxScroll);
}

// Dual-mode dropdown: click toggle / hold scrub. Animated open. Screen-aware.
static bool DropdownCore(const char* id, bool triggerDown, bool triggerHovered,
                         ImVec2 triggerMin, ImVec2 triggerMax,
                         const char* const* items, int itemCount, int* selected,
                         DropdownFlags flags) {
    if (!items || itemCount <= 0 || !selected) return false;

    if (!(flags & (DropdownFlags_ClickOnly | DropdownFlags_HoldOnly | DropdownFlags_ClickAndHold)))
        flags |= DropdownFlags_ClickAndHold;

    const bool allowClick = (flags & DropdownFlags_ClickOnly) || (flags & DropdownFlags_ClickAndHold);
    const bool allowHold  = (flags & DropdownFlags_HoldOnly)  || (flags & DropdownFlags_ClickAndHold);

    ImGui::PushID(id);
    ImGuiID gid = ImGui::GetID("##uidd");
    auto& st = AnimState<DropdownState>(gid);
    auto& T = Tokens();
    float dt = DeltaTime();
    bool changed = false;

    const bool lmbDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool lmbReleased = ImGui::IsMouseReleased(ImGuiMouseButton_Left);

    if (triggerDown && !st.pressActive) {
        st.pressActive = true;
        st.holdTimer = 0.f;
        st.openedThisGesture = false;
        st.holdMode = false;
    }

    if (st.pressActive && lmbDown) {
        st.holdTimer += dt;
        if (allowHold && !st.openedThisGesture && st.holdTimer >= T.holdThresholdSec) {
            st.open.SetOpen(true, T.durMed, T.durFast);
            st.holdMode = true;
            st.openedThisGesture = true;
            // Jump scroll so selected is visible
            st.scrollY = 0.f;
        }
    }

    if (st.pressActive && lmbReleased) {
        if (st.holdMode && st.open.IsOpen()) {
            if (st.hoverIdx >= 0 && st.hoverIdx < itemCount) {
                *selected = st.hoverIdx;
                changed = true;
            }
            st.open.SetOpen(false, T.durMed, T.durFast);
            st.holdMode = false;
        } else if (allowClick && st.holdTimer < T.holdThresholdSec && !st.openedThisGesture) {
            if (st.open.IsOpen())
                st.open.SetOpen(false, T.durMed, T.durFast);
            else {
                st.open.SetOpen(true, T.durMed, T.durFast);
                st.holdMode = false;
                st.openedThisGesture = true;
                st.scrollY = 0.f;
            }
        }
        st.pressActive = false;
        st.holdTimer = 0.f;
    }

    st.open.Update(dt);

    if (st.open.IsOpen()) {
        float p = st.open.Value();
        float alpha = std::max(0.08f, p);

        float itemH = ImGui::GetFrameHeight() + 8.f;
        float panelW = std::max(180.f, triggerMax.x - triggerMin.x + 40.f);
        // Wide format lists (DDS): grow width from longest item
        for (int i = 0; i < itemCount; ++i) {
            if (!items[i]) continue;
            float tw = ImGui::CalcTextSize(items[i]).x + 28.f;
            if (tw > panelW) panelW = std::min(tw, 520.f);
        }
        float padY = 6.f;
        float fullContentH = itemH * itemCount + padY * 2.f;

        float panelH = fullContentH;
        ImVec2 panelPos(0, 0);
        FitPanelToScreen(triggerMin, triggerMax, panelW, panelH, fullContentH, panelPos, st.scrollY);

        // Wheel scroll when hovering panel
        ImVec2 mouse = ImGui::GetIO().MousePos;
        ImVec2 p1 = panelPos;
        ImVec2 p2(panelPos.x + panelW, panelPos.y + panelH * std::max(0.15f, p)); // anim height
        // Use full fitted height for hit area once open enough
        float hitH = panelH;
        ImVec2 hitMax(panelPos.x + panelW, panelPos.y + hitH);
        bool inPanel = mouse.x >= p1.x && mouse.x <= hitMax.x &&
                       mouse.y >= p1.y && mouse.y <= hitMax.y;
        if (inPanel && std::fabs(ImGui::GetIO().MouseWheel) > 0.f) {
            st.scrollY -= ImGui::GetIO().MouseWheel * itemH * 1.5f;
            float maxScroll = std::max(0.f, fullContentH - panelH);
            st.scrollY = Clampf(st.scrollY, 0.f, maxScroll);
        }

        // Animated visual height (content still scrollable at full fitted H once open)
        float visH = std::max(itemH, panelH * p);
        p2 = ImVec2(panelPos.x + panelW, panelPos.y + visH);

        ImDrawList* dl = ImGui::GetForegroundDrawList();
        // Clip frost + items strictly to box
        dl->PushClipRect(p1, p2, true);
        DrawFrostedPanel(dl, p1, p2, alpha);
        // NO outline, NO rounding — by design

        st.hoverIdx = -1;
        const float contentTop = panelPos.y + padY - st.scrollY;
        for (int i = 0; i < itemCount; ++i) {
            float y0 = contentTop + i * itemH;
            float y1 = y0 + itemH;
            // Cull outside visible panel
            if (y1 < p1.y || y0 > p2.y) continue;

            ImVec2 rmin(panelPos.x + 4.f, y0);
            ImVec2 rmax(panelPos.x + panelW - 4.f, y1);
            bool hov = mouse.x >= rmin.x && mouse.x < rmax.x &&
                       mouse.y >= rmin.y && mouse.y < rmax.y &&
                       mouse.y >= p1.y && mouse.y <= p2.y;
            if (hov) st.hoverIdx = i;

            bool sel = (*selected == i);
            if (hov || sel) {
                ImU32 hi = IM_COL32(
                    (int)(T.accent.x * 255), (int)(T.accent.y * 255), (int)(T.accent.z * 255),
                    (int)((hov ? 90 : 50) * p));
                dl->AddRectFilled(rmin, rmax, hi, 0.f);
            }
            ImU32 tc = IM_COL32(
                (int)(T.textPrimary.x * 255), (int)(T.textPrimary.y * 255),
                (int)(T.textPrimary.z * 255), (int)(Clampf(alpha, 0.f, 1.f) * 255.f));
            dl->AddText(ImVec2(rmin.x + 10.f, rmin.y + (itemH - ImGui::GetFontSize()) * 0.5f),
                        tc, items[i] ? items[i] : "");
        }

        // Scroll cue (thin, no chrome)
        float maxScroll = std::max(0.f, fullContentH - panelH);
        if (maxScroll > 1.f && p > 0.5f) {
            float trackH = visH - 8.f;
            float thumbH = std::max(16.f, trackH * (panelH / fullContentH));
            float thumbY = p1.y + 4.f + (trackH - thumbH) * (st.scrollY / maxScroll);
            dl->AddRectFilled(
                ImVec2(p2.x - 5.f, thumbY),
                ImVec2(p2.x - 2.f, thumbY + thumbH),
                IM_COL32(255, 255, 255, (int)(50 * p)), 0.f);
        }
        dl->PopClipRect();

        bool inTrigger = mouse.x >= triggerMin.x && mouse.x <= triggerMax.x &&
                         mouse.y >= triggerMin.y && mouse.y <= triggerMax.y;
        bool outsideStrict = !inTrigger && !inPanel;

        if (st.holdMode) {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                st.open.SetOpen(false, T.durMed, T.durFast);
                st.holdMode = false;
                st.pressActive = false;
            }
        } else {
            if (st.hoverIdx >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !st.pressActive) {
                *selected = st.hoverIdx;
                changed = true;
                st.open.SetOpen(false, T.durMed, T.durFast);
            }
            if ((outsideStrict && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !st.pressActive) ||
                ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                st.open.SetOpen(false, T.durMed, T.durFast);
            }
        }
    } else if (!st.pressActive) {
        st.holdMode = false;
        st.hoverIdx = -1;
    }

    (void)triggerHovered;
    ImGui::PopID();
    return changed;
}

bool DropdownIcon(const char* id, const char* iconLogicalName, ImVec2 triggerSize,
                  const char* const* items, int itemCount, int* selected,
                  const char* tooltip, DropdownFlags flags, bool active) {
    ImGui::PushID(id);
    ImVec2 tmin = ImGui::GetCursorScreenPos();
    auto r = IconButton("##trig", iconLogicalName, triggerSize, tooltip, true, active);
    ImVec2 tmax = ImVec2(tmin.x + triggerSize.x, tmin.y + triggerSize.y);
    bool triggerDown = r.held || (ImGui::IsItemActive());
    bool changed = DropdownCore(id, triggerDown, r.hovered, tmin, tmax, items, itemCount, selected, flags);
    ImGui::PopID();
    return changed;
}

bool DropdownChip(const char* id, const char* previewLabel,
                  const char* const* items, int itemCount, int* selected,
                  DropdownFlags flags) {
    ImGui::PushID(id);
    auto& T = Tokens();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(T.bgElevated.x, T.bgElevated.y, T.bgElevated.z, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(T.accent.x, T.accent.y, T.accent.z, 0.35f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(T.accent.x, T.accent.y, T.accent.z, 0.50f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.f); // match flat dropdown language
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s  v", previewLabel ? previewLabel : "...");
    ImGui::Button(buf);
    bool held = ImGui::IsItemActive();
    ImVec2 tmin = ImGui::GetItemRectMin();
    ImVec2 tmax = ImGui::GetItemRectMax();
    // No outline on chip either
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    bool changed = DropdownCore(id, held, ImGui::IsItemHovered(), tmin, tmax, items, itemCount, selected, flags);
    ImGui::PopID();
    return changed;
}

bool Combo(const char* id, int* idx, const char* const* items, int count, const char* label) {
    if (!idx || !items || count <= 0) return false;
    if (*idx < 0 || *idx >= count) *idx = 0;
    if (label && label[0]) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
    }
    return DropdownChip(id, items[*idx], items, count, idx, DropdownFlags_ClickAndHold);
}

} // namespace Ui
