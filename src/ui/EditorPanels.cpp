#include "EditorPanels.h"
#include "../core/ConfigManager.h"
#include "../core/Logger.h"
#include "../core/KeymapManager.h"
#include "../scripting/ScriptingEngine.h"
#include "../core/ThreadPool.h"
#include <imgui_internal.h>
#include <filesystem>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <unordered_map>

extern void ApplyTheme(const std::string& themeName);
extern bool g_IsLayersHovered;
extern bool g_IsViewportHovered;

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")

static std::wstring UTF8ToWString(const std::string& str) {
    if (str.empty()) return L"";
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

static std::string WStringToUTF8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

static std::wstring ConvertFilterToWString(const char* filter) {
    if (!filter) return L"";
    std::vector<char> filterBuffer;
    const char* p = filter;
    while (true) {
        if (*p == '\0' && *(p + 1) == '\0') {
            filterBuffer.push_back('\0');
            filterBuffer.push_back('\0');
            break;
        }
        filterBuffer.push_back(*p);
        p++;
    }
    int size = static_cast<int>(filterBuffer.size());
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, filterBuffer.data(), size, NULL, 0);
    std::wstring wfilter(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, filterBuffer.data(), size, &wfilter[0], size_needed);
    return wfilter;
}

static bool ShowOpenFileWin32(char* outPath, size_t maxLen, const char* filter = "All Files (*.*)\0*.*\0") {
    OPENFILENAMEW ofn;
    wchar_t szFile[512] = { 0 };
    if (outPath && strlen(outPath) > 0) {
        std::wstring wpath = UTF8ToWString(outPath);
        std::wcsncpy(szFile, wpath.c_str(), sizeof(szFile)/sizeof(wchar_t) - 1);
    }
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile)/sizeof(wchar_t);
    std::wstring wfilter = ConvertFilterToWString(filter);
    ofn.lpstrFilter = wfilter.c_str();
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn) == TRUE) {
        std::string utf8Path = WStringToUTF8(ofn.lpstrFile);
        std::strncpy(outPath, utf8Path.c_str(), maxLen - 1);
        outPath[maxLen - 1] = '\0';
        return true;
    }
    return false;
}

static bool ShowSaveFileWin32(char* outPath, size_t maxLen, const char* filter = "All Files (*.*)\0*.*\0") {
    OPENFILENAMEW ofn;
    wchar_t szFile[512] = { 0 };
    if (outPath && strlen(outPath) > 0) {
        std::wstring wpath = UTF8ToWString(outPath);
        std::wcsncpy(szFile, wpath.c_str(), sizeof(szFile)/sizeof(wchar_t) - 1);
    }
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile)/sizeof(wchar_t);
    std::wstring wfilter = ConvertFilterToWString(filter);
    ofn.lpstrFilter = wfilter.c_str();
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

    if (GetSaveFileNameW(&ofn) == TRUE) {
        std::string utf8Path = WStringToUTF8(ofn.lpstrFile);
        std::strncpy(outPath, utf8Path.c_str(), maxLen - 1);
        outPath[maxLen - 1] = '\0';
        return true;
    }
    return false;
}
#else
static bool ShowOpenFileWin32(char* outPath, size_t maxLen, const char* filter = "All Files (*.*)\0*.*\0") { return false; }
static bool ShowSaveFileWin32(char* outPath, size_t maxLen, const char* filter = "All Files (*.*)\0*.*\0") { return false; }
#endif

namespace UI {

    bool IsSelectTool(ActiveTool tool) {
        return tool == ActiveTool::RectSelect || tool == ActiveTool::EllipseSelect || tool == ActiveTool::LassoSelect;
    }

    bool IsWandTool(ActiveTool tool) {
        return tool == ActiveTool::MagicWand || tool == ActiveTool::SmartSelect;
    }

    ActiveTool CycleSelectTool(ActiveTool current) {
        if (current == ActiveTool::RectSelect) return ActiveTool::EllipseSelect;
        if (current == ActiveTool::EllipseSelect) return ActiveTool::LassoSelect;
        return ActiveTool::RectSelect;
    }

    ActiveTool CycleWandTool(ActiveTool current) {
        if (current == ActiveTool::MagicWand) return ActiveTool::SmartSelect;
        return ActiveTool::MagicWand;
    }

    void SampleCanvasColor(Canvas& canvas, float canvasX, float canvasY, float outColor[4]) {
        static std::vector<float> s_CachedComposite;
        static int s_CachedFrame = -1;
        static int s_CachedW = 0;
        static int s_CachedH = 0;

        int frame = ImGui::GetFrameCount();
        int w = canvas.GetWidth();
        int h = canvas.GetHeight();
        if (frame != s_CachedFrame || w != s_CachedW || h != s_CachedH) {
            s_CachedComposite = canvas.GetCompositePixels();
            s_CachedFrame = frame;
            s_CachedW = w;
            s_CachedH = h;
        }

        int cx = std::clamp((int)canvasX, 0, w - 1);
        int cy = std::clamp((int)canvasY, 0, h - 1);
        size_t idx = ((size_t)cy * w + cx) * 4;
        outColor[0] = s_CachedComposite[idx + 0];
        outColor[1] = s_CachedComposite[idx + 1];
        outColor[2] = s_CachedComposite[idx + 2];
        outColor[3] = s_CachedComposite[idx + 3];
    }

    struct ToolVariant {
        const char* actionName;
        const char* displayName;
        ActiveTool tool;
    };

    static void DrawToolIcon(const char* actionName, ImVec2 min, ImVec2 max, ImU32 color) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        float w = max.x - min.x;
        float h = max.y - min.y;
        float cx = min.x + w * 0.5f;
        float cy = min.y + h * 0.5f;
        float pad = w * 0.25f;

        if (strcmp(actionName, "BrushTool") == 0) {
            ImVec2 tip = ImVec2(min.x + pad + 2.0f, max.y - pad - 2.0f);
            ImVec2 end = ImVec2(max.x - pad - 2.0f, min.y + pad + 2.0f);
            drawList->AddLine(end, tip, color, 4.0f);
            drawList->AddCircleFilled(tip, 5.0f, color);
        }
        else if (strcmp(actionName, "EraserTool") == 0) {
            ImVec2 p1 = ImVec2(min.x + pad, cy + pad * 0.5f);
            ImVec2 p2 = ImVec2(cx - pad * 0.5f, min.y + pad);
            ImVec2 p3 = ImVec2(max.x - pad, cy - pad * 0.5f);
            ImVec2 p4 = ImVec2(cx + pad * 0.5f, max.y - pad);
            ImVec2 pts[4] = { p1, p2, p3, p4 };
            drawList->AddConvexPolyFilled(pts, 4, color);
        }
        else if (strcmp(actionName, "BucketFillTool") == 0) {
            // Наклонное ведерко
            ImVec2 p1 = ImVec2(cx - w * 0.15f, cy - h * 0.1f);
            ImVec2 p2 = ImVec2(cx + w * 0.15f, cy - h * 0.25f);
            ImVec2 p3 = ImVec2(cx + w * 0.25f, cy + h * 0.1f);
            ImVec2 p4 = ImVec2(cx - w * 0.05f, cy + h * 0.25f);
            drawList->AddLine(p1, p2, color, 1.5f);
            drawList->AddLine(p2, p3, color, 1.5f);
            drawList->AddLine(p3, p4, color, 1.5f);
            drawList->AddLine(p4, p1, color, 1.5f);
            // Изливаемая капля
            drawList->AddTriangleFilled(ImVec2(cx - w * 0.1f, cy + h * 0.25f), ImVec2(cx - w * 0.2f, cy + h * 0.4f), ImVec2(cx, cy + h * 0.4f), color);
        }
        else if (strcmp(actionName, "GradientTool") == 0) {
            // Линия перехода градиента с узлами на концах
            ImVec2 pStart = ImVec2(min.x + pad, max.y - pad);
            ImVec2 pEnd = ImVec2(max.x - pad, min.y + pad);
            drawList->AddLine(pStart, pEnd, color, 2.0f);
            drawList->AddCircle(pStart, 3.5f, color, 12, 1.5f);
            drawList->AddCircleFilled(pEnd, 4.0f, color);
        }
        else if (strcmp(actionName, "PipetteTool") == 0) {
            // Пипетка под наклоном
            ImVec2 tip = ImVec2(min.x + pad + 1.0f, max.y - pad - 1.0f);
            ImVec2 end = ImVec2(max.x - pad - 1.0f, min.y + pad + 1.0f);
            drawList->AddLine(tip, end, color, 3.0f);
            drawList->AddCircleFilled(end, 4.0f, color);
            drawList->AddLine(tip, ImVec2(tip.x - 2.0f, tip.y + 2.0f), color, 1.5f);
        }
        else if (strcmp(actionName, "RectSelectTool") == 0) {
            // Прямоугольная рамка выделения (штрихи по углам)
            float rLeft = min.x + pad;
            float rRight = max.x - pad;
            float rTop = min.y + pad;
            float rBottom = max.y - pad;
            drawList->AddLine(ImVec2(rLeft, rTop), ImVec2(rLeft + 4.0f, rTop), color, 1.0f);
            drawList->AddLine(ImVec2(rLeft, rTop), ImVec2(rLeft, rTop + 4.0f), color, 1.0f);
            drawList->AddLine(ImVec2(rRight, rTop), ImVec2(rRight - 4.0f, rTop), color, 1.0f);
            drawList->AddLine(ImVec2(rRight, rTop), ImVec2(rRight, rTop + 4.0f), color, 1.0f);
            drawList->AddLine(ImVec2(rLeft, rBottom), ImVec2(rLeft + 4.0f, rBottom), color, 1.0f);
            drawList->AddLine(ImVec2(rLeft, rBottom), ImVec2(rLeft, rBottom - 4.0f), color, 1.0f);
            drawList->AddLine(ImVec2(rRight, rBottom), ImVec2(rRight - 4.0f, rBottom), color, 1.0f);
            drawList->AddLine(ImVec2(rRight, rBottom), ImVec2(rRight, rBottom - 4.0f), color, 1.0f);
        }
        else if (strcmp(actionName, "EllipseSelectTool") == 0) {
            // Штриховой овал/круг
            float radius = (w - pad * 2.0f) * 0.5f;
            const int segments = 16;
            for (int i = 0; i < segments; i += 2) {
                float a1 = (i) * 2.0f * 3.14159f / segments;
                float a2 = (i + 1) * 2.0f * 3.14159f / segments;
                drawList->PathArcTo(ImVec2(cx, cy), radius, a1, a2, 4);
                drawList->PathStroke(color, false, 1.0f);
            }
        }
        else if (strcmp(actionName, "LassoSelectTool") == 0) {
            // Схематичное лассо произвольной формы штрихами
            ImVec2 pts[] = {
                ImVec2(cx - 5.0f, cy - 5.0f),
                ImVec2(cx + 5.0f, cy - 7.0f),
                ImVec2(cx + 7.0f, cy),
                ImVec2(cx + 3.0f, cy + 6.0f),
                ImVec2(cx - 6.0f, cy + 4.0f),
                ImVec2(cx - 7.0f, cy - 1.0f)
            };
            for (int i = 0; i < 6; ++i) {
                if (i % 2 == 0) {
                    drawList->AddLine(pts[i], pts[(i + 1) % 6], color, 1.0f);
                }
            }
        }
        else if (strcmp(actionName, "MagicWandTool") == 0) {
            // Волшебная палочка (палочка и звезды-точки на конце)
            ImVec2 wStart(min.x + pad, max.y - pad);
            ImVec2 wEnd(cx + 2.0f, cy - 2.0f);
            drawList->AddLine(wStart, wEnd, color, 2.0f);
            drawList->AddLine(ImVec2(cx - 1.0f, cy - 5.0f), ImVec2(cx + 5.0f, cy - 5.0f), color, 1.0f);
            drawList->AddLine(ImVec2(cx + 2.0f, cy - 8.0f), ImVec2(cx + 2.0f, cy - 2.0f), color, 1.0f);
        }
        else if (strcmp(actionName, "SmartSelectTool") == 0) {
            // Умное выделение (уголки ограничительной рамки и круглый маркер по центру)
            float rL = min.x + pad;
            float rR = max.x - pad;
            float rT = min.y + pad;
            float rB = max.y - pad;
            drawList->AddLine(ImVec2(rL, rT), ImVec2(rL + 4.0f, rT), color, 1.0f);
            drawList->AddLine(ImVec2(rL, rT), ImVec2(rL, rT + 4.0f), color, 1.0f);
            drawList->AddLine(ImVec2(rR, rB), ImVec2(rR - 4.0f, rB), color, 1.0f);
            drawList->AddLine(ImVec2(rR, rB), ImVec2(rR, rB - 4.0f), color, 1.0f);
            drawList->AddCircleFilled(ImVec2(cx, cy), 2.5f, color);
            drawList->AddLine(ImVec2(cx, cy - 4.0f), ImVec2(cx, cy + 4.0f), color, 1.0f);
            drawList->AddLine(ImVec2(cx - 4.0f, cy), ImVec2(cx + 4.0f, cy), color, 1.0f);
        }
        else if (strcmp(actionName, "PanTool") == 0) {
            float r = w * 0.25f;
            drawList->AddLine(ImVec2(cx - r, cy), ImVec2(cx + r, cy), color, 2.0f);
            drawList->AddLine(ImVec2(cx, cy - r), ImVec2(cx, cy + r), color, 2.0f);
            drawList->AddTriangleFilled(ImVec2(cx - r, cy - 3.0f), ImVec2(cx - r, cy + 3.0f), ImVec2(cx - r - 4.0f, cy), color);
            drawList->AddTriangleFilled(ImVec2(cx + r, cy - 3.0f), ImVec2(cx + r, cy + 3.0f), ImVec2(cx + r + 4.0f, cy), color);
            drawList->AddTriangleFilled(ImVec2(cx - 3.0f, cy - r), ImVec2(cx + 3.0f, cy - r), ImVec2(cx, cy - r - 4.0f), color);
            drawList->AddTriangleFilled(ImVec2(cx - 3.0f, cy + r), ImVec2(cx + 3.0f, cy + r), ImVec2(cx, cy + r + 4.0f), color);
        }
        else if (strcmp(actionName, "RotateTool") == 0) {
            float r = w * 0.22f;
            drawList->AddCircle(ImVec2(cx, cy), r, color, 16, 2.0f);
            drawList->AddTriangleFilled(ImVec2(cx + r - 3.0f, cy - 3.0f), ImVec2(cx + r + 3.0f, cy - 3.0f), ImVec2(cx + r, cy + 2.0f), color);
        }
        else {
            drawList->AddRect(min, max, color, 1.0f);
        }
    }

    static void DrawKeybindBadge(ImVec2 btnMax, const std::string& keybindString, float btnSize) {
        if (keybindString.empty()) return;

        std::string badgeText;
        size_t plusPos = keybindString.find_last_of('+');
        if (plusPos != std::string::npos) {
            badgeText = keybindString.substr(plusPos + 1);
        } else {
            badgeText = keybindString;
        }
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
        if (hasSeparator) {
            separatorExtra = gap * 2.0f + 1.0f;
        }

        float usableMain = isVertical ? avail.y : avail.x;
        usableMain -= separatorExtra + gap * (float)(buttonCount - 1);
        float sizeFromMain = usableMain / (float)buttonCount;
        float sizeFromCross = isVertical ? avail.x : avail.y;

        return std::clamp(std::min(sizeFromMain, sizeFromCross), kMin, kMax);
    }

    static void ToolbarAdvance(bool isVertical, float gap) {
        if (isVertical) {
            ImGui::Dummy(ImVec2(0.0f, gap));
        } else {
            ImGui::SameLine(0.0f, gap);
        }
    }

    static void ToolbarBeginLayout(ImVec2 avail, bool isVertical, int buttonCount, float btnSize, float gap, bool hasSeparator) {
        float separatorExtra = hasSeparator ? (gap * 2.0f + 1.0f) : 0.0f;
        float totalMain = btnSize * (float)buttonCount + gap * (float)(buttonCount - 1) + separatorExtra;
        float totalCross = btnSize;

        float padMain = std::max(0.0f, (isVertical ? avail.y : avail.x) - totalMain);
        float padCross = std::max(0.0f, (isVertical ? avail.x : avail.y) - totalCross);

        if (isVertical) {
            if (padMain > 0.0f) ImGui::Dummy(ImVec2(0.0f, padMain * 0.5f));
            if (padCross > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padCross * 0.5f);
        } else {
            if (padCross > 0.0f) ImGui::Dummy(ImVec2(0.0f, padCross * 0.5f));
            if (padMain > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padMain * 0.5f);
        }
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
        static std::unordered_map<ImGuiID, double> s_PressStart;
        static std::unordered_map<ImGuiID, bool> s_LongPressOpened;

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

        ImGui::PushID(groupId);
        if (isActive) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_Button]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered]);
        }

        ImGui::BeginGroup();
        ImGui::Button("##groupBtn", ImVec2(size, size));
        ImGuiID itemId = ImGui::GetItemID();
        double now = ImGui::GetTime();

        if (ImGui::IsItemActive()) {
            if (s_PressStart.find(itemId) == s_PressStart.end()) {
                s_PressStart[itemId] = now;
                s_LongPressOpened[itemId] = false;
            }
            if (!s_LongPressOpened[itemId] && (now - s_PressStart[itemId]) > 0.15) {
                ImGui::OpenPopup("##variantPopup");
                s_LongPressOpened[itemId] = true;
            }
        }

        if (ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            if (!s_LongPressOpened[itemId]) {
                activeTool = display.tool;
                s_LastVariantIndex[groupId] = displayIdx;
            }
            s_PressStart.erase(itemId);
            s_LongPressOpened.erase(itemId);
        }

        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            rebindAction = rebindActionName;
            ImGui::OpenPopup("RebindToolPopup");
        }

        if (ImGui::BeginPopup("##variantPopup")) {
            for (int i = 0; i < variantCount; ++i) {
                if (ImGui::Selectable(variants[i].displayName, displayIdx == i)) {
                    activeTool = variants[i].tool;
                    s_LastVariantIndex[groupId] = i;
                }
            }
            ImGui::EndPopup();
        }

        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s (%s)\nClick: activate  |  Hold: pick variant\nRight-click: rebind",
                groupTooltip, keybindString.c_str());
        }

        ImVec2 btnMin = ImGui::GetItemRectMin();
        ImVec2 btnMax = ImGui::GetItemRectMax();
        DrawToolIcon(display.actionName, btnMin, btnMax,
            ImGui::GetColorU32(isActive ? ImGuiCol_Text : ImGuiCol_TextDisabled));
        DrawKeybindBadge(btnMax, keybindString, size);

        ImGui::EndGroup();
        ImGui::PopStyleColor(2);
        ImGui::PopID();
    }

    static void RenderToolButton(const char* actionName, const char* displayName, ActiveTool targetTool, bool isEraseTool, std::string keybindString, float size, std::string& rebindAction, ActiveTool& activeTool, BrushSettings& brush, Canvas& canvas) {
        bool isActive = (activeTool == targetTool && (targetTool != ActiveTool::Brush || isEraseTool == brush.erase));
        if (strcmp(actionName, "Reset") == 0) isActive = false;

        ImGui::PushID(actionName);
        
        if (isActive) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_Button]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered]);
        }
        
        ImGui::BeginGroup();
        ImGui::Button("##toolBtn", ImVec2(size, size));
        
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            if (strcmp(actionName, "Reset") == 0) {
                canvas.ResetView();
            } else {
                activeTool = targetTool;
                if (targetTool == ActiveTool::Brush) {
                    brush.erase = isEraseTool;
                }
            }
        }
        if (strcmp(actionName, "Reset") != 0 && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            rebindAction = actionName;
            ImGui::OpenPopup("RebindToolPopup");
        }
        if (ImGui::IsItemHovered()) {
            if (strcmp(actionName, "Reset") == 0) {
                ImGui::SetTooltip("Reset View");
            } else {
                ImGui::SetTooltip("%s (%s)\nRight-click to rebind", displayName, keybindString.c_str());
            }
        }
        
        ImVec2 btnMin = ImGui::GetItemRectMin();
        ImVec2 btnMax = ImGui::GetItemRectMax();
        DrawToolIcon(actionName, btnMin, btnMax, ImGui::GetColorU32(isActive ? ImGuiCol_Text : ImGuiCol_TextDisabled));

        if (strcmp(actionName, "Reset") != 0) {
            DrawKeybindBadge(btnMax, keybindString, size);
        }

        ImGui::EndGroup();
        ImGui::PopStyleColor(2);
        ImGui::PopID();
    }

    void RenderAll(UIState& state, Canvas& canvas, BrushSettings& brush, ActiveTool& activeTool, ID3D11Device* device, ID3D11DeviceContext* context, GLFWwindow* window) {
        ImGuiViewport* mainViewport = ImGui::GetMainViewport();

        // 1. Persistent Header (Main Menu Bar)
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Open Project (.rayp)", KeymapManager::Get().GetActionShortcutString("OpenProject").c_str())) {
                    state.openLoadRaypModal = true;
                }
                if (ImGui::MenuItem("Save Project (.rayp)", KeymapManager::Get().GetActionShortcutString("SaveProject").c_str())) {
                    state.openSaveRaypModal = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Import Image...", "Ctrl+I")) {
                    state.openImportModal = true;
                }
                if (ImGui::MenuItem("Quick Export", KeymapManager::Get().GetActionShortcutString("QuickExport").c_str())) {
                    state.openQuickExportTrigger = true;
                }
                if (ImGui::MenuItem("Advanced Export...", KeymapManager::Get().GetActionShortcutString("AdvancedExport").c_str())) {
                    state.openExportAdvancedModal = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Load Config...")) {
                    state.openLoadConfigModal = true;
                }
                if (ImGui::MenuItem("Save Config...")) {
                    state.openSaveConfigModal = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Settings / Preferences...")) {
                    state.openSettingsModal = true;
                }
                if (ImGui::MenuItem("Save Settings")) {
                    ConfigManager::Get().Save();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Exit", "Alt+F4")) {
                    glfwSetWindowShouldClose(window, true);
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Edit")) {
                std::string undoLabel = "Undo";
                if (canvas.CanUndo()) {
                    undoLabel += " (" + canvas.GetUndoName() + ")";
                }
                if (ImGui::MenuItem(undoLabel.c_str(), KeymapManager::Get().GetActionShortcutString("Undo").c_str(), false, canvas.CanUndo())) {
                    canvas.Undo();
                }

                std::string redoLabel = "Redo";
                if (canvas.CanRedo()) {
                    redoLabel += " (" + canvas.GetRedoName() + ")";
                }
                if (ImGui::MenuItem(redoLabel.c_str(), KeymapManager::Get().GetActionShortcutString("Redo").c_str(), false, canvas.CanRedo())) {
                    canvas.Redo();
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Canvas")) {
                if (ImGui::MenuItem("Canvas Size...")) {
                    state.openCanvasSizeModal = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Rotate Canvas 90 CW")) {
                    canvas.RotateCanvas90(device, true);
                }
                if (ImGui::MenuItem("Rotate Canvas 90 CCW")) {
                    canvas.RotateCanvas90(device, false);
                }
                if (ImGui::MenuItem("Flip Canvas Horizontally")) {
                    canvas.FlipCanvasHorizontal(device);
                }
                if (ImGui::MenuItem("Flip Canvas Vertically")) {
                    canvas.FlipCanvasVertical(device);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Flip Active Layer Horizontally", nullptr, false, canvas.GetActiveLayerIndex() != -1)) {
                    canvas.FlipActiveLayerHorizontal(device);
                }
                if (ImGui::MenuItem("Flip Active Layer Vertically", nullptr, false, canvas.GetActiveLayerIndex() != -1)) {
                    canvas.FlipActiveLayerVertical(device);
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                ImGui::MenuItem("Toolbar", nullptr, &state.showToolbar);
                ImGui::MenuItem("Properties", nullptr, &state.showProperties);
                ImGui::MenuItem("Layers", nullptr, &state.showLayers);
                ImGui::MenuItem("Colors Window", nullptr, &state.showColors);
                ImGui::MenuItem("Tool Settings", nullptr, &state.showToolSettings);
                ImGui::MenuItem("Console logs", nullptr, &state.showConsole);
                ImGui::Separator();
                if (ImGui::MenuItem("Reset View")) {
                    canvas.ResetView();
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Scripting")) {
                if (ImGui::MenuItem("Run test command")) {
                    ScriptingEngine::Get().RunString("import rayv; rayv.log_warn('Executing scripting check.')");
                }
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        // 2. Persistent Footer (Status Bar)
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));
        ImGui::BeginViewportSideBar("##StatusBar", mainViewport, ImGuiDir_Down, 28.0f, 
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
        
        const char* toolLabel = "Hand";
        switch (activeTool) {
            case ActiveTool::Brush: toolLabel = "Brush"; break;
            case ActiveTool::Eraser: toolLabel = "Eraser"; break;
            case ActiveTool::Pan: toolLabel = "Hand"; break;
            case ActiveTool::RectSelect: toolLabel = "Rect Select"; break;
            case ActiveTool::EllipseSelect: toolLabel = "Ellipse Select"; break;
            case ActiveTool::LassoSelect: toolLabel = "Lasso Select"; break;
            case ActiveTool::MagicWand: toolLabel = "Magic Wand"; break;
            case ActiveTool::SmartSelect: toolLabel = "Smart Select"; break;
            case ActiveTool::MovePixels: toolLabel = "Transform"; break;
            case ActiveTool::Pipette: toolLabel = "Pipette"; break;
            case ActiveTool::BucketFill: toolLabel = "Bucket Fill"; break;
            case ActiveTool::Gradient: toolLabel = "Gradient"; break;
        }
        ImGui::Text("Startup: %.1f ms | Frame: %.2f ms | FPS: %.1f | Canvas: %d x %d | Zoom: %.0f%% | Threads: %d | Tool: %s",
            state.startupTimeMs, state.frameTimeMs, state.fps, canvas.GetWidth(), canvas.GetHeight(), canvas.GetZoom() * 100.0f,
            ThreadPool::Get().GetThreadCount(), toolLabel);
        
        ImGui::End();
        ImGui::PopStyleVar();

        // 3. DockSpace Default Layout Setup
        ImGuiID dockspace_id = ImGui::DockSpaceOverViewport(0, mainViewport);
        static bool firstTimeDock = !std::filesystem::exists(ConfigManager::GetUserSubdirectory("user") + "/imgui.ini");
        if (firstTimeDock) {
            firstTimeDock = false;
            ImGui::DockBuilderRemoveNode(dockspace_id);
            ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspace_id, mainViewport->Size);

            ImGuiID dock_main_id = dockspace_id;
            ImGuiID dock_left_id = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Left, 0.08f, NULL, &dock_main_id);
            ImGuiID dock_right_id = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Right, 0.25f, NULL, &dock_main_id);
            
            ImGuiID dock_right_top_id = ImGui::DockBuilderSplitNode(dock_right_id, ImGuiDir_Up, 0.35f, NULL, &dock_right_id);
            ImGuiID dock_right_middle_id = ImGui::DockBuilderSplitNode(dock_right_id, ImGuiDir_Up, 0.50f, NULL, &dock_right_id);
            ImGuiID dock_right_bottom_id = dock_right_id;

            ImGui::DockBuilderDockWindow("Toolbar", dock_left_id);
            ImGui::DockBuilderDockWindow("Canvas Viewport", dock_main_id);
            ImGui::DockBuilderDockWindow("Properties", dock_right_top_id);
            ImGui::DockBuilderDockWindow("Layers", dock_right_middle_id);
            ImGui::DockBuilderDockWindow("Tool Settings", dock_right_bottom_id);

            ImGui::DockBuilderFinish(dockspace_id);
        }

        // 4. Modals Triggers
        if (state.openImportModal) { ImGui::OpenPopup("Import Image"); state.openImportModal = false; }
        if (state.openExportDdsModal) { ImGui::OpenPopup("Export DDS"); state.openExportDdsModal = false; }
        if (state.openExportStdModal) { ImGui::OpenPopup("Export Standard Image"); state.openExportStdModal = false; }
        if (state.openExportAdvancedModal) { ImGui::OpenPopup("Advanced Export Settings"); state.openExportAdvancedModal = false; }
        if (state.openSettingsModal) { ImGui::OpenPopup("Settings"); state.openSettingsModal = false; }
        if (state.openCanvasSizeModal) { ImGui::OpenPopup("Canvas Size"); state.openCanvasSizeModal = false; }
        if (state.openSaveRaypModal) { ImGui::OpenPopup("Save Project"); state.openSaveRaypModal = false; }
        if (state.openLoadRaypModal) { ImGui::OpenPopup("Load Project"); state.openLoadRaypModal = false; }
        if (state.openLoadConfigModal) { ImGui::OpenPopup("Load Config"); state.openLoadConfigModal = false; }
        if (state.openSaveConfigModal) { ImGui::OpenPopup("Save Config"); state.openSaveConfigModal = false; }
        if (state.showRecoveryModal) { ImGui::OpenPopup("Restore Auto-Saved Session?"); state.showRecoveryModal = false; }

        // Load Config Modal
        if (ImGui::BeginPopupModal("Load Config", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            static char loadConfigPath[512] = "config.json";
            ImGui::Text("Enter config file path (.json):");
            ImGui::InputText("##loadconfigpath", loadConfigPath, IM_ARRAYSIZE(loadConfigPath));
            ImGui::Separator();
            if (ImGui::Button("Load", ImVec2(120, 0))) {
                if (ConfigManager::Get().Load(loadConfigPath)) {
                    ApplyTheme(ConfigManager::Get().GetTheme().c_str());
                    Logger::Get().Info("Config loaded successfully: " + std::string(loadConfigPath));
                    ImGui::CloseCurrentPopup();
                } else {
                    Logger::Get().Error("Failed to load config from: " + std::string(loadConfigPath));
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Save Config Modal
        if (ImGui::BeginPopupModal("Save Config", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            static char saveConfigPath[512] = "config.json";
            ImGui::Text("Enter config file path (.json):");
            ImGui::InputText("##saveconfigpath", saveConfigPath, IM_ARRAYSIZE(saveConfigPath));
            ImGui::Separator();
            if (ImGui::Button("Save", ImVec2(120, 0))) {
                if (ConfigManager::Get().Save(saveConfigPath)) {
                    Logger::Get().Info("Config saved successfully to: " + std::string(saveConfigPath));
                    ImGui::CloseCurrentPopup();
                } else {
                    Logger::Get().Error("Failed to save config to: " + std::string(saveConfigPath));
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Import Popup Modal
        if (ImGui::BeginPopupModal("Import Image", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            static char importPath[512] = "";
            ImGui::Text("Enter absolute path to image:");
            ImGui::InputText("##importpath", importPath, IM_ARRAYSIZE(importPath));
            ImGui::SameLine();
            if (ImGui::Button("Browse...##import")) {
                ShowOpenFileWin32(importPath, IM_ARRAYSIZE(importPath), "Image Files (*.dds;*.png;*.jpg;*.jpeg;*.tga;*.bmp)\0*.dds;*.png;*.jpg;*.jpeg;*.tga;*.bmp\0All Files (*.*)\0*.*\0");
            }
            ImGui::Separator();
            if (ImGui::Button("Import", ImVec2(120, 0))) {
                if (canvas.LoadImageToLayer(device, importPath)) {
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Export DDS Popup Modal
        if (ImGui::BeginPopupModal("Export DDS", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            static char exportPath[512] = "export.dds";
            static int formatChoice = 0; 
            ImGui::Text("Enter export path:");
            ImGui::InputText("##exportpath", exportPath, IM_ARRAYSIZE(exportPath));
            ImGui::SameLine();
            if (ImGui::Button("Browse...##exportdds")) {
                ShowSaveFileWin32(exportPath, IM_ARRAYSIZE(exportPath), "DDS Files (*.dds)\0*.dds\0All Files (*.*)\0*.*\0");
            }
            ImGui::Text("DDS Format:");
            static const char* formatNames[] = {
                "BC7 (sRGB)",
                "BC7 (Linear)",
                "BC1 (sRGB)",
                "BC1 (Linear)",
                "BC2 (sRGB)",
                "BC2 (Linear)",
                "BC3 (sRGB)",
                "BC3 (Linear, DXT5)",
                "BC3 (Linear, RXGB)",
                "BC4 (Linear, Unsigned)",
                "BC5 (Linear, Unsigned)",
                "BC5 (Linear, Signed)",
                "BC6H (Linear, Unsigned)",
                "BC6H (Linear, Signed)",
                "B8G8R8A8 (Linear)",
                "B8G8R8A8 (sRGB)",
                "B8G8R8X8 (Linear)",
                "B8G8R8X8 (sRGB)",
                "R8G8B8A8 (Linear)",
                "R8G8B8A8 (sRGB)",
                "R8 (Linear, Unsigned)",
                "R8G8 (Linear, Unsigned)",
                "R8G8 (Linear, Signed)",
                "R32 (Linear, Float)"
            };
            ImGui::Combo("##ddsformat", &formatChoice, formatNames, IM_ARRAYSIZE(formatNames));
            ImGui::Separator();
            if (ImGui::Button("Export", ImVec2(120, 0))) {
                std::string chosenFormat = formatNames[formatChoice];
                if (canvas.SaveCanvasCompressed(exportPath, chosenFormat, true, "Bicubic", "Medium")) {
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Export Standard Image Popup Modal
        if (ImGui::BeginPopupModal("Export Standard Image", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            static char exportPath[512] = "export.png";
            static char iccPath[512] = "";
            ImGui::Text("Enter export path (PNG, JPG, BMP, TGA):");
            ImGui::InputText("##exportpathstd", exportPath, IM_ARRAYSIZE(exportPath));
            ImGui::SameLine();
            if (ImGui::Button("Browse...##exportstd")) {
                ShowSaveFileWin32(exportPath, IM_ARRAYSIZE(exportPath), "Image Files (*.png;*.jpg;*.jpeg;*.bmp;*.tga)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0All Files (*.*)\0*.*\0");
            }
            ImGui::Spacing();
            ImGui::Text("Optional ICC Profile (PNG only):");
            ImGui::InputText("##iccpath", iccPath, IM_ARRAYSIZE(iccPath));
            ImGui::SameLine();
            if (ImGui::Button("Browse...##icc")) {
                ShowOpenFileWin32(iccPath, IM_ARRAYSIZE(iccPath), "ICC Profiles (*.icc;*.icm)\0*.icc;*.icm\0All Files (*.*)\0*.*\0");
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear")) {
                iccPath[0] = '\0';
            }
            ImGui::TextDisabled("Leave empty for default sRGB colorspace");
            ImGui::Separator();
            if (ImGui::Button("Export", ImVec2(120, 0))) {
                if (canvas.SaveCanvasStandard(exportPath, iccPath)) {
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Advanced Export Settings Modal
        if (ImGui::BeginPopupModal("Advanced Export Settings", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            static char exportPath[512] = "";
            static bool inited = false;
            if (!inited) {
                std::strncpy(exportPath, canvas.GetExportPath().c_str(), sizeof(exportPath));
                if (strlen(exportPath) == 0) {
                    std::strncpy(exportPath, "export.png", sizeof(exportPath));
                }
                inited = true;
            }
            
            ImGui::Text("Export File Path:");
            ImGui::InputText("##advpath", exportPath, IM_ARRAYSIZE(exportPath));
            ImGui::SameLine();
            if (ImGui::Button("Browse...##adv")) {
                ShowSaveFileWin32(exportPath, IM_ARRAYSIZE(exportPath), "DDS Files (*.dds)\0*.dds\0PNG Files (*.png)\0*.png\0All Files (*.*)\0*.*\0");
            }
            
            std::string pathStr = exportPath;
            size_t dot = pathStr.find_last_of('.');
            std::string ext = "";
            if (dot != std::string::npos) {
                ext = pathStr.substr(dot + 1);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            }
            
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            
            if (ext == "dds") {
                ImGui::TextColored(ImVec4(0.2f, 0.7f, 1.0f, 1.0f), "DDS Export Settings (Subprocess Compressed)");
                
                const char* formats[] = { "BC7_UNORM_SRGB", "BC7_UNORM", "BC3_UNORM", "BC1_UNORM", "RGBA8_UNORM", "RGBA16_FLOAT" };
                static int currentFormatIdx = 0;
                std::string currentFmt = canvas.GetExportFormat();
                for (int i = 0; i < IM_ARRAYSIZE(formats); ++i) {
                    if (currentFmt == formats[i]) currentFormatIdx = i;
                }
                if (ImGui::Combo("DDS Format / Preset", &currentFormatIdx, formats, IM_ARRAYSIZE(formats))) {
                    canvas.SetExportFormat(formats[currentFormatIdx]);
                }
                
                bool mips = canvas.GetExportGenerateMipMaps();
                if (ImGui::Checkbox("Generate Mipmaps", &mips)) {
                    canvas.SetExportGenerateMipMaps(mips);
                }
                
                if (mips) {
                    const char* filters[] = { "Point", "Box", "Linear", "Cubic" };
                    static int currentFilterIdx = 3;
                    std::string currentFilter = canvas.GetExportMipFilter();
                    for (int i = 0; i < IM_ARRAYSIZE(filters); ++i) {
                        if (currentFilter == filters[i]) currentFilterIdx = i;
                    }
                    if (ImGui::Combo("Mip Filter", &currentFilterIdx, filters, IM_ARRAYSIZE(filters))) {
                        canvas.SetExportMipFilter(filters[currentFilterIdx]);
                    }
                }
                
                const char* speeds[] = { "Fast", "Medium", "Slow", "Best" };
                static int currentSpeedIdx = 1;
                std::string currentSpeed = canvas.GetExportCompressionSpeed();
                for (int i = 0; i < IM_ARRAYSIZE(speeds); ++i) {
                    if (currentSpeed == speeds[i]) currentSpeedIdx = i;
                }
                if (ImGui::Combo("Compression Quality", &currentSpeedIdx, speeds, IM_ARRAYSIZE(speeds))) {
                    canvas.SetExportCompressionSpeed(speeds[currentSpeedIdx]);
                }
            } else if (ext == "png") {
                ImGui::TextColored(ImVec4(0.2f, 0.7f, 1.0f, 1.0f), "PNG Export Settings");
                
                static char iccPath[512] = "";
                std::string currentIcc = canvas.GetExportPngColorSpace();
                if (currentIcc != "sRGB" && currentIcc != "Linear") {
                    std::strncpy(iccPath, currentIcc.c_str(), sizeof(iccPath));
                }
                
                ImGui::InputText("ICC Color Profile Path", iccPath, IM_ARRAYSIZE(iccPath));
                ImGui::SameLine();
                if (ImGui::Button("Browse...##iccpathadv")) {
                    ShowOpenFileWin32(iccPath, IM_ARRAYSIZE(iccPath), "ICC Profiles (*.icc;*.icm)\0*.icc;*.icm\0All Files (*.*)\0*.*\0");
                }
                ImGui::TextDisabled("Leave empty for standard sRGB colorspace");
                
                canvas.SetExportPngColorSpace(strlen(iccPath) > 0 ? iccPath : "sRGB");
            } else {
                ImGui::TextColored(ImVec4(0.2f, 0.7f, 1.0f, 1.0f), "Standard Format Export");
                ImGui::Text("Format: %s", ext.empty() ? "None" : ext.c_str());
            }
            
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            
            if (ImGui::Button("Export Now", ImVec2(120, 0))) {
                canvas.SetExportPath(exportPath);
                
                bool success = false;
                if (ext == "dds") {
                    success = canvas.SaveCanvasCompressed(
                        exportPath,
                        canvas.GetExportFormat(),
                        canvas.GetExportGenerateMipMaps(),
                        canvas.GetExportMipFilter(),
                        canvas.GetExportCompressionSpeed()
                    );
                } else {
                    std::string icc = canvas.GetExportPngColorSpace();
                    success = canvas.SaveCanvasStandard(exportPath, icc == "sRGB" ? "" : icc);
                }
                
                if (success) {
                    inited = false;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                inited = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Settings / Preferences Popup Modal
        if (ImGui::BeginPopupModal("Settings", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (!state.settingsInitialized) {
                state.activeTheme = ConfigManager::Get().GetTheme();
                state.backupDir = ConfigManager::Get().GetBackupDir();
                state.defW = ConfigManager::Get().GetDefaultWidth();
                state.defH = ConfigManager::Get().GetDefaultHeight();
                state.autoSaveMins = ConfigManager::Get().GetAutoSaveIntervalMinutes();
                state.maxUndo = ConfigManager::Get().GetMaxUndoSteps();
                state.maxUndoMem = ConfigManager::Get().GetMaxUndoMemoryMB();
                state.settingsInitialized = true;
            }

            if (ImGui::BeginTabBar("SettingsTabs")) {
                if (ImGui::BeginTabItem("General")) {
                    ImGui::Spacing();
                    ImGui::Text("Interface Settings");
                    ImGui::Separator();
                    
                    const char* themes[] = { "Dark", "Light", "Classic" };
                    int currentThemeIdx = 0;
                    if (state.activeTheme == "Light") currentThemeIdx = 1;
                    else if (state.activeTheme == "Classic") currentThemeIdx = 2;

                    if (ImGui::Combo("Theme", &currentThemeIdx, themes, IM_ARRAYSIZE(themes))) {
                        state.activeTheme = themes[currentThemeIdx];
                        ApplyTheme(state.activeTheme);
                    }

                    ImGui::Spacing();
                    ImGui::Text("Canvas Defaults");
                    ImGui::Separator();
                    ImGui::InputInt("Default Width", &state.defW, 128, 256);
                    ImGui::InputInt("Default Height", &state.defH, 128, 256);

                    ImGui::Spacing();
                    ImGui::Text("Autosave & Backup System");
                    ImGui::Separator();
                    
                    char tempBackupDir[256] = "";
                    std::strncpy(tempBackupDir, state.backupDir.c_str(), sizeof(tempBackupDir));
                    if (ImGui::InputText("Backups Directory", tempBackupDir, IM_ARRAYSIZE(tempBackupDir))) {
                        state.backupDir = tempBackupDir;
                    }
                    ImGui::SliderInt("Autosave (minutes)", &state.autoSaveMins, 0, 60, "%d min");
                    ImGui::TextDisabled("Set to 0 to disable periodic auto-saves");

                    ImGui::Spacing();
                    ImGui::Text("Undo / Redo Cache Limits");
                    ImGui::Separator();
                    ImGui::SliderInt("Max History Steps", &state.maxUndo, 5, 200, "%d steps");
                    ImGui::SliderInt("Max RAM Cache Size", &state.maxUndoMem, 64, 2048, "%d MB");
                    
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Keybindings")) {
                    ImGui::Spacing();
                    ImGui::Text("Click 'Rebind' next to an action to assign a new physical hotkey.");
                    ImGui::Separator();
                    ImGui::Spacing();

                    auto bindings = KeymapManager::Get().GetBindings();
                    for (const auto& pair : bindings) {
                        ImGui::PushID(pair.first.c_str());
                        ImGui::Text("%s:", pair.first.c_str());
                        ImGui::SameLine(180);

                        if (state.listeningForKey && state.rebindingAction == pair.first) {
                            ImGui::TextColored(ImVec4(0.2f, 0.7f, 1.0f, 1.0f), "[Press any key + Ctrl/Shift/Alt...]");
                            
                            ImGuiIO& io = ImGui::GetIO();
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
                                            
                                            KeymapManager::Get().BindAction(state.rebindingAction, pendingCombo);
                                            state.listeningForKey = false;
                                            state.rebindingAction = "";
                                            break;
                                        }
                                    }
                                }
                            }
                        } else {
                            ImGui::Text("%s", pair.second.ToString().c_str());
                            ImGui::SameLine(320);
                            if (ImGui::Button("Rebind")) {
                                state.rebindingAction = pair.first;
                                state.listeningForKey = true;
                            }
                        }
                        ImGui::PopID();
                    }

                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (ImGui::Button("Save & Close", ImVec2(120, 0))) {
                ConfigManager::Get().SetTheme(state.activeTheme);
                ConfigManager::Get().SetDefaultWidth(state.defW);
                ConfigManager::Get().SetDefaultHeight(state.defH);
                ConfigManager::Get().SetBackupDir(state.backupDir);
                ConfigManager::Get().SetAutoSaveIntervalMinutes(state.autoSaveMins);
                ConfigManager::Get().SetMaxUndoSteps(state.maxUndo);
                ConfigManager::Get().SetMaxUndoMemoryMB(state.maxUndoMem);
                ConfigManager::Get().Save();
                
                KeymapManager::Get().Save();
                
                state.settingsInitialized = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ApplyTheme(ConfigManager::Get().GetTheme());
                KeymapManager::Get().Load();
                state.settingsInitialized = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Canvas Size Popup Modal
        if (ImGui::BeginPopupModal("Canvas Size", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            static int targetW = 0;
            static int targetH = 0;
            static bool initSize = false;
            if (!initSize) {
                targetW = canvas.GetWidth();
                targetH = canvas.GetHeight();
                initSize = true;
            }

            ImGui::Text("Resize Canvas Dimensions:");
            ImGui::Separator();
            ImGui::InputInt("Width", &targetW, 128, 256);
            ImGui::InputInt("Height", &targetH, 128, 256);

            if (targetW < 1) targetW = 1;
            if (targetH < 1) targetH = 1;

            ImGui::Separator();
            if (ImGui::Button("Resize", ImVec2(120, 0))) {
                canvas.ResizeCanvas(device, targetW, targetH);
                initSize = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                initSize = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Save Project (.rayp) Modal
        if (ImGui::BeginPopupModal("Save Project", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            static char savePath[512] = "project.rayp";
            ImGui::Text("Enter project file path (.rayp):");
            ImGui::InputText("##savepathrayp", savePath, IM_ARRAYSIZE(savePath));
            ImGui::SameLine();
            if (ImGui::Button("Browse...##saveproject")) {
                ShowSaveFileWin32(savePath, IM_ARRAYSIZE(savePath), "RayP Projects (*.rayp)\0*.rayp\0All Files (*.*)\0*.*\0");
            }
            ImGui::Separator();
            if (ImGui::Button("Save", ImVec2(120, 0))) {
                if (canvas.SaveCanvasRayp(savePath)) {
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Load Project (.rayp) Modal
        if (ImGui::BeginPopupModal("Load Project", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            static char loadPath[512] = "project.rayp";
            ImGui::Text("Enter project file path (.rayp):");
            ImGui::InputText("##loadpathrayp", loadPath, IM_ARRAYSIZE(loadPath));
            ImGui::SameLine();
            if (ImGui::Button("Browse...##loadproject")) {
                ShowOpenFileWin32(loadPath, IM_ARRAYSIZE(loadPath), "RayP Projects (*.rayp)\0*.rayp\0All Files (*.*)\0*.*\0");
            }
            ImGui::Separator();
            if (ImGui::Button("Load", ImVec2(120, 0))) {
                if (canvas.LoadCanvasRayp(loadPath, device)) {
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Restore Backup Modal
        if (ImGui::BeginPopupModal("Restore Auto-Saved Session?", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("It looks like the application closed unexpectedly.");
            ImGui::Text("Would you like to restore your auto-saved session?");
            ImGui::Separator();
            if (ImGui::Button("Restore Session", ImVec2(140, 0))) {
                if (canvas.LoadCanvasRayp(state.backupPath, device)) {
                    Logger::Get().Info("Restored auto-saved session successfully.");
                } else {
                    Logger::Get().Error("Failed to restore auto-saved session.");
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Discard", ImVec2(140, 0))) {
                try {
                    std::filesystem::remove(state.backupPath);
                } catch (...) {}
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // 5. Draw Toolbar Panel
        if (state.showToolbar) {
            ImGui::Begin("Toolbar", &state.showToolbar, ImGuiWindowFlags_NoCollapse);

            ImVec2 avail = ImGui::GetContentRegionAvail();
            bool isVertical = (avail.y > avail.x);

            std::string brushBind = KeymapManager::Get().GetActionShortcutString("BrushTool");
            std::string eraserBind = KeymapManager::Get().GetActionShortcutString("EraserTool");
            std::string panBind = KeymapManager::Get().GetActionShortcutString("PanTool");
            std::string rotateBind = KeymapManager::Get().GetActionShortcutString("RotateTool");
            std::string fillBind = KeymapManager::Get().GetActionShortcutString("BucketFillTool");
            std::string gradientBind = KeymapManager::Get().GetActionShortcutString("GradientTool");
            std::string pipetteBind = KeymapManager::Get().GetActionShortcutString("PipetteTool");
            std::string selectBind = KeymapManager::Get().GetActionShortcutString("SelectToolGroup");
            std::string wandBind = KeymapManager::Get().GetActionShortcutString("WandToolGroup");
            std::string transformBind = KeymapManager::Get().GetActionShortcutString("TransformTool");

            static const ToolVariant s_SelectVariants[] = {
                { "RectSelectTool", "Rectangular Selection", ActiveTool::RectSelect },
                { "EllipseSelectTool", "Ellipse Selection", ActiveTool::EllipseSelect },
                { "LassoSelectTool", "Lasso Selection", ActiveTool::LassoSelect },
            };
            static const ToolVariant s_WandVariants[] = {
                { "MagicWandTool", "Magic Wand", ActiveTool::MagicWand },
                { "SmartSelectTool", "Smart Select", ActiveTool::SmartSelect },
            };

            static std::string s_RebindAction = "";
            constexpr int kToolbarButtonCount = 10;
            const bool hasSeparator = true;
            float btnSize = ComputeAdaptiveToolButtonSize(avail, isVertical, kToolbarButtonCount, hasSeparator);
            float gap = isVertical ? ImGui::GetStyle().ItemSpacing.y : ImGui::GetStyle().ItemSpacing.x;

            ToolbarBeginLayout(avail, isVertical, kToolbarButtonCount, btnSize, gap, hasSeparator);

            RenderToolButton("BrushTool", "Brush", ActiveTool::Brush, false, brushBind, btnSize, s_RebindAction, activeTool, brush, canvas);
            ToolbarAdvance(isVertical, gap);
            RenderToolButton("EraserTool", "Eraser", ActiveTool::Eraser, true, eraserBind, btnSize, s_RebindAction, activeTool, brush, canvas);
            ToolbarAdvance(isVertical, gap);
            RenderToolButton("BucketFillTool", "Fill", ActiveTool::BucketFill, false, fillBind, btnSize, s_RebindAction, activeTool, brush, canvas);
            ToolbarAdvance(isVertical, gap);
            RenderToolButton("GradientTool", "Gradient", ActiveTool::Gradient, false, gradientBind, btnSize, s_RebindAction, activeTool, brush, canvas);
            ToolbarAdvance(isVertical, gap);
            RenderToolButton("PipetteTool", "Pipette", ActiveTool::Pipette, false, pipetteBind, btnSize, s_RebindAction, activeTool, brush, canvas);
            ToolbarAdvance(isVertical, gap);
            RenderGroupedToolButton("SelectGroup", "SelectToolGroup", s_SelectVariants, IM_ARRAYSIZE(s_SelectVariants),
                "Selection Tools", selectBind, btnSize, s_RebindAction, activeTool);
            ToolbarAdvance(isVertical, gap);
            RenderGroupedToolButton("WandGroup", "WandToolGroup", s_WandVariants, IM_ARRAYSIZE(s_WandVariants),
                "Wand / Smart Select", wandBind, btnSize, s_RebindAction, activeTool);
            ToolbarAdvance(isVertical, gap);
            RenderToolButton("TransformTool", "Transform", ActiveTool::MovePixels, false, transformBind, btnSize, s_RebindAction, activeTool, brush, canvas);
            ToolbarAdvance(isVertical, gap);
            RenderToolButton("PanTool", "Hand", ActiveTool::Pan, false, panBind + " / " + rotateBind, btnSize, s_RebindAction, activeTool, brush, canvas);
            if (isVertical) {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
            } else {
                ImGui::SameLine(0.0f, gap * 2.0f);
            }
            RenderToolButton("Reset", "Reset View", activeTool, false, std::string(""), btnSize, s_RebindAction, activeTool, brush, canvas);

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
                if (bound) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            ImGui::End();
        }

        // 6. Draw Properties Panel
        if (state.showProperties) {
            ImGui::Begin("Properties", &state.showProperties, ImGuiWindowFlags_NoCollapse);
            
            ImGui::Text("Zoom: %.0f%%", canvas.GetZoom() * 100.0f);
            ImGui::Text("Pan: (%.1f, %.1f)", canvas.GetPan().x, canvas.GetPan().y);
            
            ImGui::Spacing();
            ImGui::Text("Viewport Transformations:");
            bool flipH = canvas.GetViewportFlipH();
            if (ImGui::Checkbox("Flip Horizontal", &flipH)) {
                canvas.SetViewportFlipH(flipH);
            }
            ImGui::SameLine();
            bool flipV = canvas.GetViewportFlipV();
            if (ImGui::Checkbox("Flip Vertical", &flipV)) {
                canvas.SetViewportFlipV(flipV);
            }

            float rotAngle = canvas.GetRotationAngle() * (180.0f / 3.14159265f);
            if (ImGui::SliderFloat("Rotation", &rotAngle, -180.0f, 180.0f, "%.1f deg")) {
                canvas.SetRotationAngle(rotAngle * (3.14159265f / 180.0f));
            }

            if (ImGui::Button("Reset Viewport")) {
                canvas.ResetView();
            }
            
            ImGui::Separator();
            ImGui::Text("Active Channels:");
            
            auto DrawUnifiedChannelToggle = [](const char* label, bool active, ImVec4 activeColor) -> bool {
                if (active) {
                    ImGui::PushStyleColor(ImGuiCol_Button, activeColor);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(activeColor.x * 1.1f, activeColor.y * 1.1f, activeColor.z * 1.1f, activeColor.w));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(activeColor.x * 0.9f, activeColor.y * 0.9f, activeColor.z * 0.9f, activeColor.w));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_Button]);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered]);
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
                }
                
                bool clicked = ImGui::Button(label, ImVec2(44, 30));
                
                ImGui::PopStyleColor(4);
                return clicked;
            };

            bool r = canvas.GetChannelR();
            bool g = canvas.GetChannelG();
            bool b = canvas.GetChannelB();
            bool a = canvas.GetChannelA();

            if (DrawUnifiedChannelToggle("R", r, ImVec4(0.8f, 0.2f, 0.2f, 1.0f))) canvas.SetChannelR(!r);
            ImGui::SameLine();
            if (DrawUnifiedChannelToggle("G", g, ImVec4(0.2f, 0.7f, 0.2f, 1.0f))) canvas.SetChannelG(!g);
            ImGui::SameLine();
            if (DrawUnifiedChannelToggle("B", b, ImVec4(0.2f, 0.4f, 0.8f, 1.0f))) canvas.SetChannelB(!b);
            ImGui::SameLine();
            if (DrawUnifiedChannelToggle("A", a, ImVec4(0.6f, 0.6f, 0.6f, 1.0f))) canvas.SetChannelA(!a);
            
            ImGui::NewLine();
            ImGui::Separator();
            ImGui::Text("Project Properties:");
            int pType = (canvas.GetProjectType() == Canvas::ProjectType::Simple) ? 0 : 1;
            const char* pTypeNames[] = { "Simple Project", "Advanced Project (.rayp)" };
            if (ImGui::Combo("Project Type", &pType, pTypeNames, IM_ARRAYSIZE(pTypeNames))) {
                canvas.SetProjectType((pType == 0) ? Canvas::ProjectType::Simple : Canvas::ProjectType::Advanced);
            }

            char propProjPath[512] = "";
            std::strncpy(propProjPath, canvas.GetCurrentProjectFilePath().c_str(), sizeof(propProjPath));
            ImGui::InputText("Project Path", propProjPath, IM_ARRAYSIZE(propProjPath));
            ImGui::SameLine();
            if (ImGui::Button("...##propProjPath")) {
                if (ShowOpenFileWin32(propProjPath, IM_ARRAYSIZE(propProjPath), "RayP Projects (*.rayp)\0*.rayp\0All Files (*.*)\0*.*\0")) {
                    canvas.SetCurrentProjectFilePath(propProjPath);
                }
            }

            ImGui::NewLine();
            ImGui::Separator();
            ImGui::Text("Export Settings:");
            
            char propExportPath[512] = "";
            std::strncpy(propExportPath, canvas.GetExportPath().c_str(), sizeof(propExportPath));
            ImGui::InputText("Export Path", propExportPath, IM_ARRAYSIZE(propExportPath));
            ImGui::SameLine();
            if (ImGui::Button("...##propExportPath")) {
                if (ShowSaveFileWin32(propExportPath, IM_ARRAYSIZE(propExportPath), "DDS Files (*.dds)\0*.dds\0PNG Files (*.png)\0*.png\0All Files (*.*)\0*.*\0")) {
                    canvas.SetExportPath(propExportPath);
                }
            }
            
            std::string pathStr = propExportPath;
            size_t dot = pathStr.find_last_of('.');
            std::string ext = "";
            if (dot != std::string::npos) {
                ext = pathStr.substr(dot + 1);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            }
            
            if (ext == "dds") {
                const char* formats[] = { "BC7_UNORM_SRGB", "BC7_UNORM", "BC3_UNORM", "BC1_UNORM", "RGBA8_UNORM", "RGBA16_FLOAT" };
                int currentFormatIdx = 0;
                std::string currentFmt = canvas.GetExportFormat();
                for (int i = 0; i < IM_ARRAYSIZE(formats); ++i) {
                    if (currentFmt == formats[i]) currentFormatIdx = i;
                }
                if (ImGui::Combo("DDS Preset", &currentFormatIdx, formats, IM_ARRAYSIZE(formats))) {
                    canvas.SetExportFormat(formats[currentFormatIdx]);
                }
                
                bool mips = canvas.GetExportGenerateMipMaps();
                if (ImGui::Checkbox("Mipmaps", &mips)) {
                    canvas.SetExportGenerateMipMaps(mips);
                }
                
                if (mips) {
                    const char* filters[] = { "Point", "Box", "Linear", "Cubic" };
                    int currentFilterIdx = 3;
                    std::string currentFilter = canvas.GetExportMipFilter();
                    for (int i = 0; i < IM_ARRAYSIZE(filters); ++i) {
                        if (currentFilter == filters[i]) currentFilterIdx = i;
                    }
                    if (ImGui::Combo("Filter", &currentFilterIdx, filters, IM_ARRAYSIZE(filters))) {
                        canvas.SetExportMipFilter(filters[currentFilterIdx]);
                    }
                }
            } else if (ext == "png") {
                char propIccPath[512] = "";
                std::string currentIcc = canvas.GetExportPngColorSpace();
                if (currentIcc != "sRGB" && currentIcc != "Linear") {
                    std::strncpy(propIccPath, currentIcc.c_str(), sizeof(propIccPath));
                }
                ImGui::InputText("ICC Profile", propIccPath, IM_ARRAYSIZE(propIccPath));
                ImGui::SameLine();
                if (ImGui::Button("...##propIccPath")) {
                    if (ShowOpenFileWin32(propIccPath, IM_ARRAYSIZE(propIccPath), "ICC Profiles (*.icc;*.icm)\0*.icc;*.icm\0All Files (*.*)\0*.*\0")) {
                        canvas.SetExportPngColorSpace(strlen(propIccPath) > 0 ? propIccPath : "sRGB");
                    }
                }
            }

            ImGui::End();
        }

        if (state.showLayers) {
            ImGui::Begin("Layers", &state.showLayers, ImGuiWindowFlags_NoCollapse);
            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows)) {
                g_IsLayersHovered = true;
            }
            
            if (ImGui::Button("Add Layer", ImVec2(-1, 25))) {
                std::string lName = "Layer " + std::to_string(canvas.GetLayers().size() + 1);
                canvas.CreateNewLayer(device, lName);
            }

            ImGui::BeginChild("LayersList", ImVec2(0, 0), true);
            auto& layers = canvas.GetLayers();
            for (int i = static_cast<int>(layers.size()) - 1; i >= 0; --i) {
                ImGui::PushID(i);
                
                bool isIsolated = canvas.IsLayerIsolated(i);
                if (isIsolated) {
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.45f, 0.15f, 1.0f));
                }
                
                bool vis = layers[i].visible;
                if (ImGui::Checkbox("##visible", &vis)) {
                    if (ImGui::GetIO().KeyAlt) {
                        canvas.ToggleLayerIsolation(i);
                    } else {
                        layers[i].visible = vis;
                    }
                }
                if (isIsolated) {
                    ImGui::PopStyleColor();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Alt+Click to Isolate this layer");
                }
                
                ImGui::SameLine();

                bool isSelected = (canvas.GetActiveLayerIndex() == i);
                if (ImGui::Selectable(layers[i].name.c_str(), isSelected, ImGuiSelectableFlags_None, ImVec2(ImGui::GetContentRegionAvail().x - 70, 0))) {
                    canvas.SetActiveLayerIndex(i);
                }
                
                ImGui::SameLine();
                if (layers.size() > 1) {
                    if (ImGui::Button("Del")) {
                        canvas.DeleteLayer(i);
                    }
                }

                ImGui::PushItemWidth(100);
                ImGui::SliderFloat("Opacity", &layers[i].opacity, 0.0f, 1.0f, "%.2f");
                ImGui::PopItemWidth();

                if (isSelected) {
                    ImGui::Spacing();
                    if (layers[i].hasMask) {
                        ImGui::Text("Mask: Active");
                        ImGui::SameLine();
                        if (ImGui::Button("Apply Mask")) {
                            canvas.ApplyLayerMask(i);
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Delete Mask")) {
                            canvas.DeleteLayerMask(i);
                        }
                    } else {
                        if (ImGui::Button("Create Mask")) {
                            canvas.CreateLayerMask(device, i);
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Mask from Sel")) {
                            canvas.CreateLayerMaskFromSelection(device, i);
                        }
                    }
                }

                ImGui::Separator();
                ImGui::PopID();
            }
            ImGui::EndChild();

            ImGui::End();
        }

        // 8. Draw Standalone Tool Settings Panel (adapts to active tool + layout)
        if (state.showToolSettings) {
            ImGui::Begin("Tool Settings", &state.showToolSettings, ImGuiWindowFlags_NoCollapse);

            ImVec2 tsAvail = ImGui::GetContentRegionAvail();
            bool tsHorizontal = (tsAvail.x > tsAvail.y * 1.15f);

            auto BeginToolSection = [](const char* title) {
                ImGui::TextDisabled("%s", title);
                ImGui::Separator();
            };

            bool isBrushLike = (activeTool == ActiveTool::Brush || activeTool == ActiveTool::Eraser);

            if (isBrushLike) {
                BeginToolSection(activeTool == ActiveTool::Eraser ? "Eraser" : "Brush");

                if (tsHorizontal) {
                    ImGui::Columns(2, "##brushCols", false);
                    ImGui::SliderFloat("Radius##ts", &brush.radius, 1.0f, 250.0f, "%.0f px");
                    ImGui::Checkbox("P -> Radius", &brush.pressureRadius);
                    ImGui::SliderFloat("Hardness##ts", &brush.hardness, 0.0f, 1.0f, "%.2f");
                    ImGui::Checkbox("P -> Hardness", &brush.pressureHardness);

                    ImGui::NextColumn();
                    ImGui::SliderFloat("Opacity##ts", &brush.opacity, 0.0f, 1.0f, "%.2f");
                    ImGui::Checkbox("P -> Opacity", &brush.pressureOpacity);
                    ImGui::SliderFloat("Spacing##ts", &brush.spacing, 0.01f, 5.0f, "%.2f");
                    ImGui::SliderInt("Stabilization##ts", &brush.stabilization, 1, 50, "%d");
                    ImGui::Columns(1);
                } else {
                    ImGui::SliderFloat("Radius##ts", &brush.radius, 1.0f, 250.0f, "%.0f px");
                    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
                    ImGui::Checkbox("P->R", &brush.pressureRadius);
                    ImGui::SliderFloat("Hardness##ts", &brush.hardness, 0.0f, 1.0f, "%.2f");
                    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
                    ImGui::Checkbox("P->H", &brush.pressureHardness);
                    ImGui::SliderFloat("Opacity##ts", &brush.opacity, 0.0f, 1.0f, "%.2f");
                    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
                    ImGui::Checkbox("P->O", &brush.pressureOpacity);
                    ImGui::SliderFloat("Spacing##ts", &brush.spacing, 0.01f, 5.0f, "%.2f");
                    ImGui::SliderInt("Stabilization##ts", &brush.stabilization, 1, 50, "%d");
                }

                if (activeTool == ActiveTool::Brush) {
                    ImGui::Spacing();
                    bool mirrorH = canvas.GetMirrorHorizontal();
                    bool mirrorV = canvas.GetMirrorVertical();
                    if (tsHorizontal) {
                        if (ImGui::Checkbox("Mirror H", &mirrorH)) canvas.SetMirrorHorizontal(mirrorH);
                        ImGui::SameLine();
                        if (ImGui::Checkbox("Mirror V", &mirrorV)) canvas.SetMirrorVertical(mirrorV);
                    } else {
                        if (ImGui::Checkbox("Horizontal Mirror", &mirrorH)) canvas.SetMirrorHorizontal(mirrorH);
                        if (ImGui::Checkbox("Vertical Mirror", &mirrorV)) canvas.SetMirrorVertical(mirrorV);
                    }
                    ImGui::TextDisabled("Hold Alt+LMB: sample color (eyedropper)");
                }
            }
            else if (activeTool == ActiveTool::MagicWand || activeTool == ActiveTool::SmartSelect) {
                BeginToolSection(activeTool == ActiveTool::MagicWand ? "Magic Wand" : "Smart Select");
                if (activeTool == ActiveTool::MagicWand) {
                    ImGui::SliderFloat("Tolerance##mw", &state.magicWandTolerance, 0.0f, 1.0f, "%.2f");
                    ImGui::Checkbox("Contiguous", &state.magicWandContiguous);
                } else {
                    ImGui::TextWrapped("Draw contour; GrabCut runs in background.");
                }
                ImGui::TextDisabled("Shift: add  |  Alt: subtract  |  W: cycle wand tools");
            }
            else if (activeTool == ActiveTool::BucketFill) {
                BeginToolSection("Bucket Fill");
                ImGui::SliderFloat("Tolerance##bf", &state.bucketFillTolerance, 0.0f, 1.0f, "%.2f");
            }
            else if (IsSelectTool(activeTool)) {
                const char* selName =
                    activeTool == ActiveTool::RectSelect ? "Rectangular Selection" :
                    activeTool == ActiveTool::EllipseSelect ? "Ellipse Selection" : "Lasso Selection";
                BeginToolSection(selName);
                ImGui::TextDisabled("Shift: add  |  Alt: subtract  |  S: cycle select tools");
            }
            else if (activeTool == ActiveTool::Gradient) {
                BeginToolSection("Gradient");
                ImGui::TextWrapped("Drag to set direction. Primary -> Secondary color.");
            }
            else if (activeTool == ActiveTool::Pipette) {
                BeginToolSection("Pipette");
                ImGui::TextWrapped("Hold LMB on canvas to sample color.");
            }
            else if (activeTool == ActiveTool::MovePixels) {
                BeginToolSection("Transform");
                if (tsHorizontal) {
                    ImGui::Columns(2, "##txCols", false);
                    float sx = canvas.GetFloatingScaleX();
                    if (ImGui::SliderFloat("Scale X##tx", &sx, 0.05f, 5.0f, "%.2f")) canvas.SetFloatingScaleX(sx);
                    float sy = canvas.GetFloatingScaleY();
                    if (ImGui::SliderFloat("Scale Y##tx", &sy, 0.05f, 5.0f, "%.2f")) canvas.SetFloatingScaleY(sy);
                    ImGui::NextColumn();
                    float rotDeg = canvas.GetFloatingRotation() * (180.0f / 3.14159265f);
                    if (ImGui::SliderFloat("Rotation##tx", &rotDeg, -180.0f, 180.0f, "%.1f deg")) {
                        canvas.SetFloatingRotation(rotDeg * (3.14159265f / 180.0f));
                    }
                    ImGui::Columns(1);
                } else {
                    float sx = canvas.GetFloatingScaleX();
                    if (ImGui::SliderFloat("Scale X##tx", &sx, 0.05f, 5.0f, "%.2f")) canvas.SetFloatingScaleX(sx);
                    float sy = canvas.GetFloatingScaleY();
                    if (ImGui::SliderFloat("Scale Y##tx", &sy, 0.05f, 5.0f, "%.2f")) canvas.SetFloatingScaleY(sy);
                    float rotDeg = canvas.GetFloatingRotation() * (180.0f / 3.14159265f);
                    if (ImGui::SliderFloat("Rotation##tx", &rotDeg, -180.0f, 180.0f, "%.1f deg")) {
                        canvas.SetFloatingRotation(rotDeg * (3.14159265f / 180.0f));
                    }
                }

                if (ImGui::Button("Flip H##tx")) canvas.SetFloatingScaleX(-canvas.GetFloatingScaleX());
                ImGui::SameLine();
                if (ImGui::Button("Flip V##tx")) canvas.SetFloatingScaleY(-canvas.GetFloatingScaleY());
                ImGui::SameLine();
                if (ImGui::Button("Reset##tx")) {
                    canvas.SetFloatingScaleX(1.0f);
                    canvas.SetFloatingScaleY(1.0f);
                    canvas.SetFloatingRotation(0.0f);
                }

                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.55f, 0.18f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.72f, 0.25f, 1.0f));
                if (ImGui::Button("Commit##tx", ImVec2(tsHorizontal ? 120.0f : -1.0f, 0))) state.commitTransform = true;
                ImGui::PopStyleColor(2);
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.18f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.25f, 0.25f, 1.0f));
                if (ImGui::Button("Cancel##tx", ImVec2(tsHorizontal ? 120.0f : -1.0f, 0))) state.cancelTransform = true;
                ImGui::PopStyleColor(2);
            }
            else {
                BeginToolSection("Pan / Hand");
                ImGui::TextWrapped("Middle-click drag or Hand tool to pan.");
            }

            ImGui::End();
        }

        // 9. Draw Logging Console Panel
        if (state.showConsole) {
            ImGui::Begin("Console Logs", &state.showConsole);
            if (ImGui::Button("Clear")) {
                Logger::Get().ClearRecentLogs();
            }
            ImGui::Separator();
            ImGui::BeginChild("LogScrollRegion", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
            auto logs = Logger::Get().GetRecentLogs();
            for (const auto& log : logs) {
                if (log.find("[ERROR]") != std::string::npos) {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), log.c_str());
                } else if (log.find("[WARN ]") != std::string::npos) {
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), log.c_str());
                } else if (log.find("[DEBUG]") != std::string::npos) {
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.8f, 1.0f), log.c_str());
                } else {
                    ImGui::TextUnformatted(log.c_str());
                }
            }
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
                ImGui::SetScrollHereY(1.0f);
            }
            ImGui::EndChild();
            ImGui::End();
        }

        // 10. Draw Colors Panel
        if (state.showColors) {
            ImGui::Begin("Colors", &state.showColors);
            
            ImGuiColorEditFlags flags = ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_AlphaPreview;
            ImGui::ColorPicker4("##color_picker", brush.color, flags);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Text("Quick Palette:");
            static const ImVec4 paletteColors[] = {
                ImVec4(0, 0, 0, 1), ImVec4(1, 1, 1, 1), ImVec4(0.5f, 0.5f, 0.5f, 1), ImVec4(0.75f, 0.75f, 0.75f, 1),
                ImVec4(1, 0, 0, 1), ImVec4(1, 1, 0, 1), ImVec4(0, 1, 0, 1), ImVec4(0, 1, 1, 1),
                ImVec4(0, 0, 1, 1), ImVec4(1, 0, 1, 1), ImVec4(0.5f, 0, 0, 1), ImVec4(0.5f, 0.5f, 0, 1),
                ImVec4(0, 0.5f, 0, 1), ImVec4(0, 0.5f, 0.5f, 1), ImVec4(0, 0, 0.5f, 1), ImVec4(0.5f, 0, 0.5f, 1)
            };
            for (int i = 0; i < IM_ARRAYSIZE(paletteColors); ++i) {
                ImGui::PushID(i);
                if (i > 0 && i % 8 != 0) ImGui::SameLine();
                if (ImGui::ColorButton("##palette_color", paletteColors[i], ImGuiColorEditFlags_NoTooltip, ImVec2(22, 22))) {
                    brush.color[0] = paletteColors[i].x;
                    brush.color[1] = paletteColors[i].y;
                    brush.color[2] = paletteColors[i].z;
                    brush.color[3] = paletteColors[i].w;
                }
                ImGui::PopID();
            }

            ImGui::End();
        }
    }
}
