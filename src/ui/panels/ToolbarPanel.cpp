#include "ToolbarPanel.h"
#include "../EditorPanels.h"
#include "../widgets/UiPanel.h"
#include "../widgets/UiIconButton.h"
#include "../widgets/UiDropdown.h"
#include "../widgets/UiTooltip.h"
#include "../style/UiTokens.h"
#include "../style/UiMotion.h"
#include "../../core/KeymapManager.h"
#include "../../Canvas.h"
#include "../../core/PaintEngine.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <GLFW/glfw3.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

extern float g_SecondaryColor[4];
extern float g_ColorSwapAnim;
extern bool g_ColorSwapPending;

namespace UI {

static void DrawKeybindBadge(ImVec2 btnMax, const std::string& keybindString, float btnSize) {
    if (keybindString.empty()) return;

    std::string badgeText;
    size_t plusPos = keybindString.find_last_of('+');
    if (plusPos != std::string::npos)
        badgeText = keybindString.substr(plusPos + 1);
    else
        badgeText = keybindString;
    if (badgeText.empty()) return;

    std::string singleChar = badgeText.substr(0, 1);
    float badgeSize = std::clamp(btnSize * 0.32f, 10.0f, 18.0f);
    ImVec2 badgeMin = ImVec2(btnMax.x - badgeSize, btnMax.y - badgeSize);
    ImVec2 badgeMax = btnMax;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(badgeMin, badgeMax, ImGui::GetColorU32(ImGuiCol_FrameBgActive), 2.0f);
    drawList->AddRect(badgeMin, badgeMax, ImGui::GetColorU32(ImGuiCol_Border), 2.0f);

    ImVec2 textSize = ImGui::CalcTextSize(singleChar.c_str());
    float fontScale = std::clamp(btnSize / 44.0f, 0.55f, 1.0f);
    ImVec2 textPos = ImVec2(
        badgeMin.x + (badgeSize - textSize.x * fontScale) * 0.5f,
        badgeMin.y + (badgeSize - textSize.y * fontScale) * 0.5f - 1.0f);
    drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize() * fontScale, textPos,
        ImGui::GetColorU32(ImGuiCol_Text), singleChar.c_str());
}

static float ComputeAdaptiveToolButtonSize(ImVec2 avail, bool isVertical, int buttonCount, bool hasSeparator) {
    constexpr float kMin = 16.0f;
    constexpr float kMax = 64.0f;
    if (buttonCount <= 0) return 44.0f;

    ImGuiStyle& style = ImGui::GetStyle();
    float gap = isVertical ? style.ItemSpacing.y : style.ItemSpacing.x;
    float separatorExtra = 0.0f;
    if (hasSeparator)
        separatorExtra = gap * 2.0f + 1.0f;

    float usableMain = isVertical ? avail.y : avail.x;
    usableMain -= separatorExtra + gap * (float)(buttonCount - 1);
    float sizeFromMain = usableMain / (float)buttonCount;
    float sizeFromCross = isVertical ? avail.x : avail.y;

    return std::clamp(std::min(sizeFromMain, sizeFromCross), kMin, kMax);
}

static void ToolbarAdvance(bool isVertical, float gap) {
    if (isVertical)
        ImGui::Dummy(ImVec2(0.0f, gap));
    else
        ImGui::SameLine(0.0f, gap);
}

static const char* IconNameForAction(const char* actionName) {
    if (!actionName) return "placeholder";
    if (strcmp(actionName, "BrushTool") == 0) return "tool_brush";
    if (strcmp(actionName, "EraserTool") == 0) return "tool_eraser";
    if (strcmp(actionName, "BucketFillTool") == 0) return "tool_fill";
    if (strcmp(actionName, "GradientTool") == 0) return "tool_gradient";
    if (strcmp(actionName, "SmudgeTool") == 0) return "tool_smudge";
    if (strcmp(actionName, "BlurTool") == 0) return "tool_blur";
    if (strcmp(actionName, "StampTool") == 0) return "tool_stamp";
    if (strcmp(actionName, "PipetteTool") == 0) return "tool_pipette";
    if (strcmp(actionName, "PanTool") == 0) return "tool_pan";
    if (strcmp(actionName, "TransformTool") == 0) return "tool_transform";
    if (strcmp(actionName, "RectSelectTool") == 0) return "tool_select_rect";
    if (strcmp(actionName, "EllipseSelectTool") == 0) return "tool_select_ellipse";
    if (strcmp(actionName, "LassoSelectTool") == 0) return "tool_lasso";
    if (strcmp(actionName, "PolygonalLassoTool") == 0) return "tool_lasso_poly";
    if (strcmp(actionName, "MagicWandTool") == 0) return "tool_wand";
    if (strcmp(actionName, "QuickSelectTool") == 0) return "tool_quick_select";
    if (strcmp(actionName, "SmartSelectTool") == 0) return "tool_smart_select";
    if (strcmp(actionName, "Reset") == 0) return "tool_reset";
    if (strcmp(actionName, "VectorRectTool") == 0) return "tool_select_rect";
    if (strcmp(actionName, "VectorEllipseTool") == 0) return "tool_select_ellipse";
    if (strcmp(actionName, "VectorLineTool") == 0) return "tool_vector_line";
    if (strcmp(actionName, "VectorPenTool") == 0) return "tool_vector_pen";
    if (strcmp(actionName, "VectorFreehandTool") == 0) return "tool_lasso";
    if (strcmp(actionName, "VectorPolygonTool") == 0) return "tool_lasso_poly";
    if (strcmp(actionName, "VectorSelectTool") == 0) return "tool_transform";
    if (strcmp(actionName, "VectorEditTool") == 0) return "tool_vector_pen";
    return "placeholder";
}

static void ToolbarCenterCursor(float size) {
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    float winWidth = win->Size.x;
    float winHeight = win->Size.y;
    bool isVertical = (winHeight > winWidth);
    if (isVertical) {
        float posX = (winWidth - size) * 0.5f;
        if (posX > 0.0f) ImGui::SetCursorPosX(posX);
    } else {
        float posY = (winHeight - size) * 0.5f;
        if (posY > 0.0f) ImGui::SetCursorPosY(posY);
    }
}

struct ToolVariant {
    const char* actionName;
    const char* displayName;
    ActiveTool tool;
};

static void RenderToolButton(const char* actionName, const char* displayName, ActiveTool targetTool,
    bool isEraseTool, std::string keybindString, float size, std::string& rebindAction,
    ActiveTool& activeTool, BrushSettings& brush, Canvas& canvas)
{
    bool isActive = (activeTool == targetTool && (targetTool != ActiveTool::Brush || isEraseTool == brush.erase));
    if (strcmp(actionName, "Reset") == 0) isActive = false;

    ToolbarCenterCursor(size);
    char tip[256];
    if (strcmp(actionName, "Reset") == 0)
        std::snprintf(tip, sizeof(tip), "Reset View\nHome — fit document");
    else
        std::snprintf(tip, sizeof(tip), "%s%s%s\nRight-click: rebind hotkey",
            displayName,
            keybindString.empty() ? "" : "  (",
            keybindString.empty() ? "" : (keybindString + ")").c_str());

    auto r = Ui::IconButton(actionName, IconNameForAction(actionName), ImVec2(size, size), tip, true, isActive);
    if (r.clicked) {
        if (strcmp(actionName, "Reset") == 0) {
            canvas.ResetView();
        } else {
            activeTool = targetTool;
            if (targetTool == ActiveTool::Brush)
                brush.erase = isEraseTool;
        }
    }
    if (strcmp(actionName, "Reset") != 0 && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        rebindAction = actionName;
        ImGui::OpenPopup("RebindToolPopup");
    }
    if (strcmp(actionName, "Reset") != 0 && !keybindString.empty())
        DrawKeybindBadge(ImGui::GetItemRectMax(), keybindString, size);
}

static void RenderGroupedToolButton(
    const char* groupId,
    const char* rebindActionName,
    const ToolVariant* variants,
    int variantCount,
    const char* groupTooltip,
    const std::string& keybindString,
    float size,
    std::string& rebindAction,
    ActiveTool& activeTool)
{
    static std::unordered_map<std::string, int> s_LastVariantIndex;

    int activeIdx = -1;
    for (int i = 0; i < variantCount; ++i) {
        if (activeTool == variants[i].tool) {
            activeIdx = i;
            break;
        }
    }

    int displayIdx = (activeIdx >= 0) ? activeIdx : s_LastVariantIndex[groupId];
    if (displayIdx < 0 || displayIdx >= variantCount) displayIdx = 0;

    const ToolVariant& display = variants[displayIdx];
    bool isActive = (activeIdx >= 0);

    std::vector<const char*> labels;
    labels.reserve(variantCount);
    for (int i = 0; i < variantCount; ++i)
        labels.push_back(variants[i].displayName);

    ToolbarCenterCursor(size);
    char tip[256];
    std::snprintf(tip, sizeof(tip), "%s%s%s\nClick: activate/cycle  ·  Hold: pick variant\nRight-click: rebind",
        groupTooltip,
        keybindString.empty() ? "" : "  (",
        keybindString.empty() ? "" : (keybindString + ")").c_str());

    int sel = displayIdx;
    bool changed = Ui::DropdownIcon(groupId, IconNameForAction(display.actionName), ImVec2(size, size),
        labels.data(), variantCount, &sel, tip, Ui::DropdownFlags_ClickAndHold, isActive);

    if (changed && sel >= 0 && sel < variantCount) {
        activeTool = variants[sel].tool;
        s_LastVariantIndex[groupId] = sel;
    } else {
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !isActive && !changed) {
            activeTool = display.tool;
            s_LastVariantIndex[groupId] = displayIdx;
        }
    }

    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        rebindAction = rebindActionName;
        ImGui::OpenPopup("RebindToolPopup");
    }
    if (!keybindString.empty())
        DrawKeybindBadge(ImGui::GetItemRectMax(), keybindString, size);
}

void DrawToolbarPanel(UIState& state, Canvas& canvas, BrushSettings& brush, ActiveTool& activeTool) {
    if (!state.showToolbar) return;

    ImGuiWindow* window = ImGui::FindWindowByName("Toolbar");
    bool isVertical = true;
    if (window)
        isVertical = (window->Size.y > window->Size.x);
    if (isVertical)
        ImGui::SetNextWindowSizeConstraints(ImVec2(36.0f, 100.0f), ImVec2(64.0f, 16384.0f));
    else
        ImGui::SetNextWindowSizeConstraints(ImVec2(100.0f, 36.0f), ImVec2(16384.0f, 64.0f));
    Ui::BeginDockPanel("Toolbar", &state.showToolbar);
    Ui::ClampDockLeafCrossAxis(isVertical, 36.0f, 64.0f);

    ImVec2 avail = ImGui::GetContentRegionAvail();

    struct ToolDef {
        const char* action;
        const char* display;
        ActiveTool tool;
        bool erase = false;
    };
    static const ToolDef kAllTools[] = {
        { "BrushTool", "Brush", ActiveTool::Brush, false },
        { "EraserTool", "Eraser", ActiveTool::Eraser, true },
        { "StampTool", "Stamp", ActiveTool::Stamp, false },
        { "BucketFillTool", "Fill", ActiveTool::BucketFill, false },
        { "GradientTool", "Gradient", ActiveTool::Gradient, false },
        { "SmudgeTool", "Smudge", ActiveTool::Smudge, false },
        { "BlurTool", "Blur", ActiveTool::BlurTool, false },
        { "PipetteTool", "Pipette", ActiveTool::Pipette, false },
        { "RectSelectTool", "Rect Select", ActiveTool::RectSelect, false },
        { "EllipseSelectTool", "Ellipse Select", ActiveTool::EllipseSelect, false },
        { "LassoSelectTool", "Lasso", ActiveTool::LassoSelect, false },
        { "PolygonalLassoTool", "Poly Lasso", ActiveTool::PolygonalLasso, false },
        { "MagicWandTool", "Magic Wand", ActiveTool::MagicWand, false },
        { "QuickSelectTool", "Quick Select", ActiveTool::QuickSelect, false },
        { "SmartSelectTool", "Smart Select", ActiveTool::SmartSelect, false },
        { "TransformTool", "Move", ActiveTool::MovePixels, false },
        { "PanTool", "Hand", ActiveTool::Pan, false },
        { "VectorRectTool", "Rectangle", ActiveTool::VectorRect, false },
        { "VectorEllipseTool", "Ellipse", ActiveTool::VectorEllipse, false },
        { "VectorLineTool", "Line", ActiveTool::VectorLine, false },
        { "VectorPenTool", "Pen", ActiveTool::VectorPen, false },
        { "VectorFreehandTool", "Freehand", ActiveTool::VectorFreehand, false },
        { "VectorPolygonTool", "Polygon", ActiveTool::VectorPolygon, false },
        { "VectorSelectTool", "Select shapes", ActiveTool::VectorSelect, false },
        { "VectorEditTool", "Edit nodes", ActiveTool::VectorEdit, false },
    };

    struct KeySig {
        int key = 0;
        bool ctrl = false, shift = false, alt = false;
        bool operator==(const KeySig& o) const {
            return key == o.key && ctrl == o.ctrl && shift == o.shift && alt == o.alt;
        }
    };
    auto bindingOf = [](const char* action) -> KeySig {
        KeySig s;
        const auto& map = KeymapManager::Get().GetBindings();
        auto it = map.find(action);
        if (it == map.end() || it->second.key == 0) return s;
        s.key = it->second.key;
        s.ctrl = it->second.ctrl;
        s.shift = it->second.shift;
        s.alt = it->second.alt;
        return s;
    };

    auto effectiveKey = [&](const ToolDef& t) -> KeySig {
        KeySig s = bindingOf(t.action);
        if (s.key != 0) return s;
        if (t.tool == ActiveTool::RectSelect || t.tool == ActiveTool::EllipseSelect)
            return bindingOf("SelectToolGroup");
        if (t.tool == ActiveTool::LassoSelect || t.tool == ActiveTool::PolygonalLasso)
            return bindingOf("LassoToolGroup");
        if (t.tool == ActiveTool::MagicWand || t.tool == ActiveTool::QuickSelect ||
            t.tool == ActiveTool::SmartSelect)
            return bindingOf("WandToolGroup");
        return s;
    };

    struct Slot {
        KeySig key;
        std::vector<int> toolIdx;
    };
    std::vector<Slot> slots;
    for (int i = 0; i < (int)IM_ARRAYSIZE(kAllTools); ++i) {
        KeySig k = effectiveKey(kAllTools[i]);
        int found = -1;
        if (k.key != 0) {
            for (int s = 0; s < (int)slots.size(); ++s) {
                if (slots[s].key.key != 0 && slots[s].key == k) { found = s; break; }
            }
        }
        if (found >= 0)
            slots[found].toolIdx.push_back(i);
        else {
            Slot sl;
            sl.key = k;
            sl.toolIdx.push_back(i);
            slots.push_back(std::move(sl));
        }
    }

    static std::string s_RebindAction = "";
    const int nSlots = (int)slots.size();
    const bool hasSeparator = true;
    float btnSize = ComputeAdaptiveToolButtonSize(avail, isVertical, nSlots + 1, hasSeparator);
    float gap = isVertical ? ImGui::GetStyle().ItemSpacing.y : ImGui::GetStyle().ItemSpacing.x;

    ImVec2 accentMin(0, 0), accentMax(0, 0);
    bool accentHasTarget = false;
    auto markAccent = [&]() {
        if (ImGui::GetItemID() != 0) {
            accentMin = ImGui::GetItemRectMin();
            accentMax = ImGui::GetItemRectMax();
            accentHasTarget = true;
        }
    };

    for (int si = 0; si < nSlots; ++si) {
        if (si) ToolbarAdvance(isVertical, gap);
        const Slot& sl = slots[si];
        if (sl.toolIdx.size() == 1) {
            const ToolDef& t = kAllTools[sl.toolIdx[0]];
            std::string bind = KeymapManager::Get().GetActionShortcutString(t.action);
            if (bind == "—" || bind == "None") bind.clear();
            if (bind.empty() && sl.key.key != 0) {
                if (t.tool == ActiveTool::RectSelect || t.tool == ActiveTool::EllipseSelect)
                    bind = KeymapManager::Get().GetActionShortcutString("SelectToolGroup");
                else if (t.tool == ActiveTool::LassoSelect || t.tool == ActiveTool::PolygonalLasso)
                    bind = KeymapManager::Get().GetActionShortcutString("LassoToolGroup");
                else if (UI::IsWandTool(t.tool))
                    bind = KeymapManager::Get().GetActionShortcutString("WandToolGroup");
                if (bind == "—" || bind == "None") bind.clear();
            }
            RenderToolButton(t.action, t.display, t.tool, t.erase, bind, btnSize,
                s_RebindAction, activeTool, brush, canvas);
            bool on = (activeTool == t.tool);
            if (t.tool == ActiveTool::Brush)
                on = (activeTool == ActiveTool::Brush && brush.erase == t.erase) ||
                     (t.erase && activeTool == ActiveTool::Eraser);
            if (t.erase)
                on = (activeTool == ActiveTool::Eraser ||
                      (activeTool == ActiveTool::Brush && brush.erase));
            if (on) markAccent();
        } else {
            std::vector<ToolVariant> vars;
            vars.reserve(sl.toolIdx.size());
            for (int ti : sl.toolIdx) {
                const ToolDef& t = kAllTools[ti];
                vars.push_back({ t.action, t.display, t.tool });
            }
            std::string bind;
            if (sl.key.key != 0)
                bind = KeymapManager::Get().GetActionShortcutString(kAllTools[sl.toolIdx[0]].action);
            if (bind == "—" || bind == "None") bind.clear();
            char gid[64];
            std::snprintf(gid, sizeof(gid), "DynGroup_%d_%d", sl.key.key, (int)sl.toolIdx.size());
            RenderGroupedToolButton(gid, kAllTools[sl.toolIdx[0]].action,
                vars.data(), (int)vars.size(), "Tools (same hotkey — cycle)",
                bind, btnSize, s_RebindAction, activeTool);
            {
                ImVec2 rmin = ImGui::GetItemRectMin();
                ImVec2 rmax = ImGui::GetItemRectMax();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                auto& tok = Ui::Tokens();
                float s = std::max(6.f, btnSize * 0.22f);
                ImVec2 c(rmax.x - 2.f, rmax.y - 2.f);
                dl->AddTriangleFilled(
                    ImVec2(c.x - s, c.y), ImVec2(c.x, c.y), ImVec2(c.x, c.y - s),
                    tok.ColU32(tok.accent));
                char nb[8];
                std::snprintf(nb, sizeof(nb), "%d", (int)sl.toolIdx.size());
                ImVec2 ts = ImGui::CalcTextSize(nb);
                ImVec2 bp(rmax.x - ts.x - 3.f, rmin.y + 1.f);
                dl->AddRectFilled(ImVec2(bp.x - 2.f, bp.y), ImVec2(rmax.x - 1.f, bp.y + ts.y + 1.f),
                    IM_COL32(20, 20, 28, 200), 2.f);
                dl->AddText(bp, tok.ColU32(tok.textPrimary), nb);
            }
            for (int ti : sl.toolIdx) {
                if (activeTool == kAllTools[ti].tool) { markAccent(); break; }
            }
            if (activeTool == ActiveTool::Eraser) brush.erase = true;
            else if (activeTool == ActiveTool::Brush) brush.erase = false;
        }
    }

    {
        static ImVec2 s_accMin(0, 0), s_accMax(0, 0);
        static bool s_accInit = false;
        float dt = Ui::DeltaTime();
        if (accentHasTarget) {
            if (!s_accInit) {
                s_accMin = accentMin; s_accMax = accentMax; s_accInit = true;
            } else {
                float k = 1.f - std::exp(-dt * 14.f);
                s_accMin.x += (accentMin.x - s_accMin.x) * k;
                s_accMin.y += (accentMin.y - s_accMin.y) * k;
                s_accMax.x += (accentMax.x - s_accMax.x) * k;
                s_accMax.y += (accentMax.y - s_accMax.y) * k;
            }
            const float pad = 2.5f;
            auto& tok = Ui::Tokens();
            ImGui::GetWindowDrawList()->AddRect(
                ImVec2(s_accMin.x - pad, s_accMin.y - pad),
                ImVec2(s_accMax.x + pad, s_accMax.y + pad),
                tok.ColU32(tok.strokeActive), tok.rSm, 0, 1.75f);
        }
    }
    if (isVertical) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    } else {
        ImGui::SameLine(0.0f, gap * 2.0f);
    }

    {
        ToolbarCenterCursor(btnSize);
        ImVec2 cpos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##colorswap", ImVec2(btnSize, btnSize));
        bool chipHover = ImGui::IsItemHovered();
        bool chipClick = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        if (chipClick) {
            std::swap(brush.color[0], g_SecondaryColor[0]);
            std::swap(brush.color[1], g_SecondaryColor[1]);
            std::swap(brush.color[2], g_SecondaryColor[2]);
            std::swap(brush.color[3], g_SecondaryColor[3]);
            g_ColorSwapPending = true;
        }
        if (chipHover)
            Ui::Tooltip("Primary / Secondary\nClick or X: swap");

        static Ui::AnimFloat s_swapT;
        if (g_ColorSwapPending) {
            s_swapT.Snap(1.f);
            s_swapT.SetTarget(0.f, Ui::Tokens().durMed * 1.15f, Ui::EaseKind::EaseOutCubic);
            g_ColorSwapPending = false;
        }
        s_swapT.Update(Ui::DeltaTime());
        g_ColorSwapAnim = s_swapT.value;
        float useT = s_swapT.value;

        auto& tok = Ui::Tokens();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float sq = btnSize * 0.48f;
        ImVec2 pPri(cpos.x + btnSize * 0.12f, cpos.y + btnSize * 0.12f);
        ImVec2 pSec(cpos.x + btnSize * 0.40f, cpos.y + btnSize * 0.40f);
        ImVec2 a0 = ImVec2(pPri.x + (pSec.x - pPri.x) * useT, pPri.y + (pSec.y - pPri.y) * useT);
        ImVec2 b0 = ImVec2(pSec.x + (pPri.x - pSec.x) * useT, pSec.y + (pPri.y - pSec.y) * useT);
        dl->AddRectFilled(b0, ImVec2(b0.x + sq, b0.y + sq),
            IM_COL32((int)(g_SecondaryColor[0]*255),(int)(g_SecondaryColor[1]*255),
                     (int)(g_SecondaryColor[2]*255),(int)(g_SecondaryColor[3]*255)), tok.rSm);
        dl->AddRect(b0, ImVec2(b0.x + sq, b0.y + sq), tok.ColU32(tok.strokeHairline), tok.rSm, 0, 1.f);
        dl->AddRectFilled(a0, ImVec2(a0.x + sq, a0.y + sq),
            IM_COL32((int)(brush.color[0]*255),(int)(brush.color[1]*255),
                     (int)(brush.color[2]*255),(int)(brush.color[3]*255)), tok.rSm);
        dl->AddRect(a0, ImVec2(a0.x + sq, a0.y + sq),
            chipHover ? tok.ColU32(tok.strokeActive) : tok.ColU32(tok.strokeHairline), tok.rSm, 0, 1.25f);
    }

    if (ImGui::BeginPopup("RebindToolPopup")) {
        ImGui::Text("Rebind Action: %s", s_RebindAction.c_str());
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.2f, 0.7f, 1.0f, 1.0f), "[Press any key to rebind]");

        ImGuiIO& io = ImGui::GetIO();
        bool bound = false;
        for (int k = 0; k < ImGuiKey_NamedKey_END; ++k) {
            ImGuiKey imguiKey = (ImGuiKey)k;
            if (ImGui::IsKeyPressed(imguiKey)) {
                int glfwKey = 0;
                if (imguiKey >= ImGuiKey_A && imguiKey <= ImGuiKey_Z) glfwKey = GLFW_KEY_A + (imguiKey - ImGuiKey_A);
                else if (imguiKey >= ImGuiKey_0 && imguiKey <= ImGuiKey_9) glfwKey = GLFW_KEY_0 + (imguiKey - ImGuiKey_0);
                else if (imguiKey >= ImGuiKey_F1 && imguiKey <= ImGuiKey_F12) glfwKey = GLFW_KEY_F1 + (imguiKey - ImGuiKey_F1);
                else if (imguiKey == ImGuiKey_Space) glfwKey = GLFW_KEY_SPACE;
                else if (imguiKey == ImGuiKey_Enter || imguiKey == ImGuiKey_KeypadEnter) glfwKey = GLFW_KEY_ENTER;
                else if (imguiKey == ImGuiKey_Escape) glfwKey = GLFW_KEY_ESCAPE;
                else if (imguiKey == ImGuiKey_Tab) glfwKey = GLFW_KEY_TAB;
                else if (imguiKey == ImGuiKey_Backspace) glfwKey = GLFW_KEY_BACKSPACE;
                else if (imguiKey == ImGuiKey_Insert) glfwKey = GLFW_KEY_INSERT;
                else if (imguiKey == ImGuiKey_Delete) glfwKey = GLFW_KEY_DELETE;
                else if (imguiKey == ImGuiKey_RightArrow) glfwKey = GLFW_KEY_RIGHT;
                else if (imguiKey == ImGuiKey_LeftArrow) glfwKey = GLFW_KEY_LEFT;
                else if (imguiKey == ImGuiKey_DownArrow) glfwKey = GLFW_KEY_DOWN;
                else if (imguiKey == ImGuiKey_UpArrow) glfwKey = GLFW_KEY_UP;
                else if (imguiKey == ImGuiKey_Comma) glfwKey = GLFW_KEY_COMMA;
                else if (imguiKey == ImGuiKey_Period) glfwKey = GLFW_KEY_PERIOD;
                else if (imguiKey == ImGuiKey_Slash) glfwKey = GLFW_KEY_SLASH;
                else if (imguiKey == ImGuiKey_Semicolon) glfwKey = GLFW_KEY_SEMICOLON;
                else if (imguiKey == ImGuiKey_Equal) glfwKey = GLFW_KEY_EQUAL;
                else if (imguiKey == ImGuiKey_Minus) glfwKey = GLFW_KEY_MINUS;
                else if (imguiKey == ImGuiKey_LeftBracket) glfwKey = GLFW_KEY_LEFT_BRACKET;
                else if (imguiKey == ImGuiKey_RightBracket) glfwKey = GLFW_KEY_RIGHT_BRACKET;
                else if (imguiKey == ImGuiKey_Backslash) glfwKey = GLFW_KEY_BACKSLASH;
                else if (imguiKey == ImGuiKey_GraveAccent) glfwKey = GLFW_KEY_GRAVE_ACCENT;

                if (imguiKey != ImGuiKey_LeftCtrl && imguiKey != ImGuiKey_RightCtrl &&
                    imguiKey != ImGuiKey_LeftShift && imguiKey != ImGuiKey_RightShift &&
                    imguiKey != ImGuiKey_LeftAlt && imguiKey != ImGuiKey_RightAlt) {
                    if (glfwKey != 0) {
                        KeyCombination pendingCombo;
                        pendingCombo.key = glfwKey;
                        pendingCombo.ctrl = io.KeyCtrl;
                        pendingCombo.shift = io.KeyShift;
                        pendingCombo.alt = io.KeyAlt;
                        KeymapManager::Get().BindAction(s_RebindAction, pendingCombo);
                        KeymapManager::Get().Save();
                        bound = true;
                        break;
                    }
                }
            }
        }
        if (bound)
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    Ui::EndDockPanel();
}

} // namespace UI
