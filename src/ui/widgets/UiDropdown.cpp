#include "UiDropdown.h"
#include "UiIconButton.h"
#include "../style/UiMotion.h"
#include "../style/UiTokens.h"
#include <cmath>
#include <algorithm>

namespace Ui {

struct DropdownState {
    AnimBool open;
    bool holdMode = false;
    bool pressActive = false;   // LMB down started on trigger this gesture
    float holdTimer = 0.f;
    int hoverIdx = -1;
    bool openedThisGesture = false;
};

// Proper dual-mode:
// - Hold past threshold → holdMode list, release on item = commit, RMB/Esc = cancel
// - Short press+release → toggle click-mode list; click item = commit; outside/Esc = cancel
// Never open on mouse-down (that killed hold mode).
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

    // Start gesture on trigger press
    if (triggerDown && !st.pressActive) {
        st.pressActive = true;
        st.holdTimer = 0.f;
        st.openedThisGesture = false;
        st.holdMode = false;
    }

    // While pressing (even if cursor left trigger — scrub list)
    if (st.pressActive && lmbDown) {
        st.holdTimer += dt;
        if (allowHold && !st.openedThisGesture && st.holdTimer >= T.holdThresholdSec) {
            st.open.SetOpen(true, T.durMed, T.durFast);
            st.holdMode = true;
            st.openedThisGesture = true;
        }
    }

    // Release ends gesture
    if (st.pressActive && lmbReleased) {
        if (st.holdMode && st.open.IsOpen()) {
            if (st.hoverIdx >= 0 && st.hoverIdx < itemCount) {
                *selected = st.hoverIdx;
                changed = true;
            }
            st.open.SetOpen(false, T.durMed, T.durFast);
            st.holdMode = false;
        } else if (allowClick && st.holdTimer < T.holdThresholdSec && !st.openedThisGesture) {
            // Short click: toggle list
            if (st.open.IsOpen())
                st.open.SetOpen(false, T.durMed, T.durFast);
            else {
                st.open.SetOpen(true, T.durMed, T.durFast);
                st.holdMode = false;
                st.openedThisGesture = true;
            }
        }
        st.pressActive = false;
        st.holdTimer = 0.f;
    }

    // Cancel hold if mouse released without pressActive tracking somehow
    if (!lmbDown && st.pressActive && !triggerDown) {
        // still tracking until release handled above
    }

    st.open.Update(dt);

    if (st.open.IsOpen()) {
        float p = st.open.Value();
        float alpha = std::max(0.05f, p) * T.dropdownPanelAlpha;

        float itemH = ImGui::GetFrameHeight() + 6.f;
        float panelW = std::max(160.f, triggerMax.x - triggerMin.x + 100.f);
        float fullH = itemH * itemCount + T.s2 * 2.f;
        // Always use full height for hit-test; visual height eases
        float panelHVis = std::max(itemH, fullH * p);

        ImVec2 panelPos(triggerMin.x, triggerMax.y + 4.f);
        ImGuiViewport* vp = ImGui::GetMainViewport();
        if (panelPos.x + panelW > vp->WorkPos.x + vp->WorkSize.x)
            panelPos.x = vp->WorkPos.x + vp->WorkSize.x - panelW - 4.f;
        if (panelPos.y + fullH > vp->WorkPos.y + vp->WorkSize.y)
            panelPos.y = triggerMin.y - fullH - 4.f;

        ImDrawList* dl = ImGui::GetForegroundDrawList();
        ImVec2 p1 = panelPos;
        ImVec2 p2(panelPos.x + panelW, panelPos.y + panelHVis);

        ImU32 bg = ImGui::ColorConvertFloat4ToU32(ImVec4(T.bgElevated.x, T.bgElevated.y, T.bgElevated.z, alpha));
        ImU32 stroke = ImGui::ColorConvertFloat4ToU32(ImVec4(T.strokeHairline.x, T.strokeHairline.y, T.strokeHairline.z, alpha * 1.2f));
        // Soft scrim behind panel for readability
        if (p > 0.2f) {
            ImU32 scrim = ImGui::ColorConvertFloat4ToU32(ImVec4(T.scrim.x, T.scrim.y, T.scrim.z, T.scrim.w * 0.35f * p));
            dl->AddRectFilled(vp->WorkPos, ImVec2(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y), scrim);
        }
        dl->AddRectFilled(p1, p2, bg, T.rMd);
        dl->AddRect(p1, p2, stroke, T.rMd, 0, 1.25f);

        dl->PushClipRect(p1, p2, true);

        ImVec2 mouse = ImGui::GetIO().MousePos;
        // Hit-test uses full layout even while animating
        st.hoverIdx = -1;
        for (int i = 0; i < itemCount; ++i) {
            float y0 = panelPos.y + T.s2 + i * itemH;
            float y1 = y0 + itemH;
            ImVec2 rmin(panelPos.x + T.s1, y0);
            ImVec2 rmax(panelPos.x + panelW - T.s1, y1);
            bool hov = mouse.x >= rmin.x && mouse.x < rmax.x && mouse.y >= rmin.y && mouse.y < rmax.y;
            if (hov) st.hoverIdx = i;

            bool sel = (*selected == i);
            if (hov || sel) {
                ImU32 hi = ImGui::ColorConvertFloat4ToU32(
                    ImVec4(T.accent.x, T.accent.y, T.accent.z, (hov ? 0.40f : 0.22f) * p));
                dl->AddRectFilled(rmin, rmax, hi, T.rSm);
            }
            ImU32 tc = T.ColU32(T.textPrimary);
            tc = (tc & 0x00FFFFFF) | ((ImU32)(Clampf(alpha, 0.f, 1.f) * 255.f) << 24);
            dl->AddText(ImVec2(rmin.x + 10.f, rmin.y + (itemH - ImGui::GetFontSize()) * 0.5f), tc, items[i]);
        }
        dl->PopClipRect();

        bool inTrigger = mouse.x >= triggerMin.x && mouse.x <= triggerMax.x &&
                         mouse.y >= triggerMin.y && mouse.y <= triggerMax.y;
        bool inPanel = mouse.x >= p1.x && mouse.x <= p2.x && mouse.y >= p1.y && mouse.y <= p2.y;
        bool outsideStrict = !inTrigger && !inPanel;

        if (st.holdMode) {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                st.open.SetOpen(false, T.durMed, T.durFast);
                st.holdMode = false;
                st.pressActive = false;
            }
            // commit on release handled in pressActive block
        } else {
            // Click mode: pick item
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
                  const char* tooltip, DropdownFlags flags) {
    ImGui::PushID(id);
    ImVec2 tmin = ImGui::GetCursorScreenPos();
    // active ring drawn by caller when tool selected; trigger itself not "active" fill
    auto r = IconButton("##trig", iconLogicalName, triggerSize, tooltip, true, false);
    ImVec2 tmax = ImVec2(tmin.x + triggerSize.x, tmin.y + triggerSize.y);
    // Use held OR IsMouseDown after press on this item — r.held is IsItemActive
    bool triggerDown = r.held || (ImGui::IsItemActive());
    bool changed = DropdownCore(id, triggerDown, r.hovered, tmin, tmax, items, itemCount, selected, flags);
    ImGui::PopID();
    return changed;
}

bool DropdownChip(const char* id, const char* previewLabel,
                  const char* const* items, int itemCount, int* selected,
                  DropdownFlags flags) {
    ImGui::PushID(id);
    bool held = false;
    ImGui::Button(previewLabel ? previewLabel : "…");
    held = ImGui::IsItemActive();
    ImVec2 tmin = ImGui::GetItemRectMin();
    ImVec2 tmax = ImGui::GetItemRectMax();
    bool changed = DropdownCore(id, held, ImGui::IsItemHovered(), tmin, tmax, items, itemCount, selected, flags);
    ImGui::PopID();
    return changed;
}

} // namespace Ui
