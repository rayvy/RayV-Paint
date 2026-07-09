#include "EditorPanels.h"
#include "../core/ConfigManager.h"
#include "../core/Logger.h"
#include "../core/KeymapManager.h"
#include "../core/ImageManager.h"
#include "../scripting/ScriptingEngine.h"
#include "../core/ThreadPool.h"
#include "style/UiTokens.h"
#include "icons/SvgIconCache.h"
#include "widgets/UiIconButton.h"
#include "widgets/UiIconToggle.h"
#include "widgets/UiDropdown.h"
#include "widgets/UiVisualSlider.h"
#include "widgets/UiPanel.h"
#include <thread>
#include <imgui_internal.h>
#include <filesystem>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <cmath>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_map>

extern void ApplyTheme(const std::string& themeName);
extern bool g_IsLayersHovered;
extern bool g_IsViewportHovered;
extern std::vector<float> Canvas_BuildSplineLUT(const std::vector<std::pair<float,float>>& pts);

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

    DocumentLoadingState g_LoadingState;

    void TriggerBackgroundOpenDocument(const std::string& filepath, ID3D11Device* device, Canvas& canvas) {
        if (g_LoadingState.isLoading) return;
        
        g_LoadingState.isLoading = true;
        g_LoadingState.progress = 0.0f;
        g_LoadingState.filepath = filepath;
        g_LoadingState.completed = false;
        g_LoadingState.success = false;
        {
            std::lock_guard<std::mutex> lock(g_LoadingState.mutex);
            g_LoadingState.stage = "Initializing";
        }

        std::thread([filepath, device, &canvas]() {
            Logger::Get().Info("Starting background load of: " + filepath);
            bool ok = canvas.OpenDocument(device, filepath, [](float progress, const char* stage) {
                g_LoadingState.progress = progress;
                if (stage) {
                    std::lock_guard<std::mutex> lock(g_LoadingState.mutex);
                    g_LoadingState.stage = stage;
                }
            });
            g_LoadingState.success = ok;
            g_LoadingState.completed = true;
        }).detach();
    }

    bool IsSelectTool(ActiveTool tool) {
        return tool == ActiveTool::RectSelect || tool == ActiveTool::EllipseSelect;
    }
    bool IsLassoTool(ActiveTool tool) {
        return tool == ActiveTool::LassoSelect || tool == ActiveTool::PolygonalLasso;
    }
    bool IsWandTool(ActiveTool tool) {
        return tool == ActiveTool::MagicWand || tool == ActiveTool::SmartSelect || tool == ActiveTool::QuickSelect;
    }
    ActiveTool CycleSelectTool(ActiveTool current) {
        if (current == ActiveTool::RectSelect) return ActiveTool::EllipseSelect;
        return ActiveTool::RectSelect;
    }
    ActiveTool CycleLassoTool(ActiveTool current) {
        if (current == ActiveTool::LassoSelect) return ActiveTool::PolygonalLasso;
        return ActiveTool::LassoSelect;
    }
    ActiveTool CycleWandTool(ActiveTool current) {
        if (current == ActiveTool::MagicWand) return ActiveTool::SmartSelect;
        if (current == ActiveTool::SmartSelect) return ActiveTool::QuickSelect;
        return ActiveTool::MagicWand;
    }
    void SampleCanvasColor(Canvas& canvas, float canvasX, float canvasY, float outColor[4]) {
        int cx = std::clamp((int)canvasX, 0, canvas.GetWidth() - 1);
        int cy = std::clamp((int)canvasY, 0, canvas.GetHeight() - 1);
        canvas.SampleCompositePixel(cx, cy, outColor);
    }

    // True if `maybeAncestor` is parent/ancestor of `layerIdx` (cycle guard).
    static bool IsGroupAncestorOf(const std::vector<Layer>& layers, int maybeAncestor, int layerIdx) {
        int p = layers[layerIdx].parentGroupId;
        int guard = 0;
        while (p >= 0 && p < (int)layers.size() && guard++ < 64) {
            if (p == maybeAncestor) return true;
            p = layers[p].parentGroupId;
        }
        return false;
    }

    static void DrawLayerDropHighlight(const ImVec2& rmin, const ImVec2& rmax, bool intoGroup) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (intoGroup) {
            dl->AddRectFilled(rmin, rmax, IM_COL32(60, 140, 255, 55));
            dl->AddRect(rmin, rmax, IM_COL32(80, 160, 255, 220), 0.0f, 0, 2.0f);
        } else {
            float y = rmax.y;
            dl->AddLine(ImVec2(rmin.x, y), ImVec2(rmax.x, y), IM_COL32(80, 160, 255, 255), 2.5f);
        }
    }

    // Dual-mode animated dropdown (click list / hold-scrub-release) — use instead of ImGui::Combo
    static bool UiCombo(const char* id, int* idx, const char* const* items, int count,
                        const char* label = nullptr) {
        if (!idx || !items || count <= 0) return false;
        if (*idx < 0 || *idx >= count) *idx = 0;
        if (label && label[0]) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::SameLine();
        }
        return Ui::DropdownChip(id, items[*idx], items, count, idx, Ui::DropdownFlags_ClickAndHold);
    }

    // ICC preset combo (presets only — no free-text path). Returns true if changed.
    static bool DrawIccPresetCombo(Canvas& canvas, const char* label = "ICC Profile") {
        static const Canvas::IccPreset kPresets[] = {
            Canvas::IccPreset::None,
            Canvas::IccPreset::sRGB,
            Canvas::IccPreset::DisplayP3,
            Canvas::IccPreset::AdobeRGB
        };
        int cur = 1; // sRGB default
        Canvas::IccPreset p = canvas.GetExportIccPreset();
        for (int i = 0; i < IM_ARRAYSIZE(kPresets); ++i) {
            if (kPresets[i] == p) { cur = i; break; }
        }
        const char* names[] = {
            Canvas::IccPresetName(Canvas::IccPreset::None),
            Canvas::IccPresetName(Canvas::IccPreset::sRGB),
            Canvas::IccPresetName(Canvas::IccPreset::DisplayP3),
            Canvas::IccPresetName(Canvas::IccPreset::AdobeRGB)
        };
        if (UiCombo("##icc_preset", &cur, names, IM_ARRAYSIZE(names), label)) {
            canvas.SetExportIccPreset(kPresets[cur]);
            return true;
        }
        return false;
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
        // Anchor to top-left, no dynamic alignment padding
    }

    static const char* IconNameForAction(const char* actionName) {
        if (!actionName) return "placeholder";
        if (strcmp(actionName, "BrushTool") == 0) return "tool_brush";
        if (strcmp(actionName, "EraserTool") == 0) return "tool_eraser";
        if (strcmp(actionName, "BucketFillTool") == 0) return "tool_fill";
        if (strcmp(actionName, "GradientTool") == 0) return "tool_gradient";
        if (strcmp(actionName, "SmudgeTool") == 0) return "tool_smudge";
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

    // Stage 1 kit: SVG icon button with press/bounce motion
    static void RenderToolButton(const char* actionName, const char* displayName, ActiveTool targetTool, bool isEraseTool, std::string keybindString, float size, std::string& rebindAction, ActiveTool& activeTool, BrushSettings& brush, Canvas& canvas) {
        bool isActive = (activeTool == targetTool && (targetTool != ActiveTool::Brush || isEraseTool == brush.erase));
        if (strcmp(actionName, "Reset") == 0) isActive = false;

        ToolbarCenterCursor(size);
        char tip[192];
        if (strcmp(actionName, "Reset") == 0)
            std::snprintf(tip, sizeof(tip), "Reset View");
        else
            std::snprintf(tip, sizeof(tip), "%s%s%s\nRight-click: rebind",
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
        if (strcmp(actionName, "Reset") != 0 && !keybindString.empty()) {
            DrawKeybindBadge(ImGui::GetItemRectMax(), keybindString, size);
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

        // Build label list for dual-mode dropdown
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
        // ClickAndHold dropdown: short click opens list; hold scrub+release selects
        bool changed = Ui::DropdownIcon(groupId, IconNameForAction(display.actionName), ImVec2(size, size),
            labels.data(), variantCount, &sel, tip, Ui::DropdownFlags_ClickAndHold);

        if (changed && sel >= 0 && sel < variantCount) {
            activeTool = variants[sel].tool;
            s_LastVariantIndex[groupId] = sel;
        } else {
            // Short click on closed dropdown opens it — also treat simple IconButton click-activate:
            // DropdownIcon uses IconButton internally; if user clicked without selecting (re-activated group)
            // ensure tool is active with last variant
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

        // Active outline when tool in group selected
        if (isActive) {
            ImVec2 mn = ImGui::GetItemRectMin();
            ImVec2 mx = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRect(mn, mx, Ui::Tokens().ColU32(Ui::Tokens().strokeActive), Ui::Tokens().rSm, 0, 1.5f);
        }
    }

    void RenderAll(UIState& state, Canvas& canvas, BrushSettings& brush, ActiveTool& activeTool, ID3D11Device* device, ID3D11DeviceContext* context, GLFWwindow* window) {
        if (device)
            Ui::SvgIconCache::Get().SetDevice(device);
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
                if (ImGui::MenuItem("Canvas Edit...")) {
                    state.openCanvasSizeModal = true;
                }
                if (ImGui::MenuItem("Crop to Selection", KeymapManager::Get().GetActionShortcutString("CropToSelection").c_str(), false, canvas.HasSelection())) {
                    canvas.CropCanvasToSelection(device);
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
            // ---- Image Menu ----
            if (ImGui::BeginMenu("Image")) {
                bool hasLayer = canvas.GetActiveLayerIndex() != -1;
                if (ImGui::MenuItem("Invert Colors", KeymapManager::Get().GetActionShortcutString("InvertColors").c_str(), false, hasLayer))
                    canvas.InvertColors();
                if (ImGui::MenuItem("Invert Alpha", KeymapManager::Get().GetActionShortcutString("InvertAlpha").c_str(), false, hasLayer))
                    canvas.InvertAlpha();
                ImGui::Separator();
                if (ImGui::MenuItem("Blur...", nullptr, false, hasLayer))
                    state.showBlurModal = true;
                if (ImGui::MenuItem("HSV Adjust...", "Ctrl+U", false, hasLayer))
                    state.showHSVModal = true;
                if (ImGui::MenuItem("Curves...", nullptr, false, hasLayer)) {
                    state.showCurvesModal = true;
                }
                if (ImGui::MenuItem("Add Noise...", nullptr, false, hasLayer))
                    state.showNoiseModal = true;
                ImGui::EndMenu();
            }
            // ---- Select Menu ----
            if (ImGui::BeginMenu("Select")) {
                if (ImGui::MenuItem("Select All", "Ctrl+A")) canvas.SelectAll();
                if (ImGui::MenuItem("Deselect",   "Ctrl+D")) canvas.ClearSelection();
                if (ImGui::MenuItem("Invert Selection", "Ctrl+Shift+I")) canvas.InvertSelection();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                ImGui::MenuItem("Toolbar", nullptr, &state.showToolbar);
                ImGui::MenuItem("Properties", nullptr, &state.showProperties);
                ImGui::MenuItem("Viewport Navigation", nullptr, &state.showViewportNav);
                ImGui::MenuItem("Layers", nullptr, &state.showLayers);
                ImGui::MenuItem("Layer Effects", nullptr, &state.showLayerEffects);
                ImGui::MenuItem("Channels", nullptr, &state.showChannels);
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

        // ---- Image Adjustment Modals ----

        // Blur Modal
        if (state.showBlurModal) ImGui::OpenPopup("Blur##modal");
        if (ImGui::BeginPopupModal("Blur##modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Gaussian Blur (3-pass box)");
            ImGui::SliderFloat("Radius", &state.blurRadius, 0.5f, 80.0f, "%.1f px");
            ImGui::Spacing();
            if (ImGui::Button("Apply", ImVec2(100,0))) {
                canvas.ApplyBlur(state.blurRadius);
                state.showBlurModal = false; ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100,0))) { state.showBlurModal = false; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }

        // HSV Modal
        if (state.showHSVModal) ImGui::OpenPopup("HSV Adjust##modal");
        if (ImGui::BeginPopupModal("HSV Adjust##modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Hue / Saturation / Value");
            ImGui::SliderFloat("Hue",        &state.hsvH, -0.5f, 0.5f, "%.3f");
            ImGui::SliderFloat("Saturation", &state.hsvS, -1.0f, 1.0f, "%.3f");
            ImGui::SliderFloat("Value",      &state.hsvV, -1.0f, 1.0f, "%.3f");
            ImGui::Spacing();
            if (ImGui::Button("Apply", ImVec2(100,0))) {
                canvas.ApplyHSV(state.hsvH, state.hsvS, state.hsvV);
                state.hsvH=state.hsvS=state.hsvV=0.f;
                state.showHSVModal=false; ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel",ImVec2(100,0))){ state.hsvH=state.hsvS=state.hsvV=0.f; state.showHSVModal=false; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }

        // Noise Modal
        if (state.showNoiseModal) ImGui::OpenPopup("Add Noise##modal");
        if (ImGui::BeginPopupModal("Add Noise##modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::SliderFloat("Strength", &state.noiseStrength, 0.0f, 1.0f, "%.3f");
            ImGui::Checkbox("Color Noise", &state.noiseColor);
            ImGui::Spacing();
            if (ImGui::Button("Apply",ImVec2(100,0))){ canvas.ApplyNoise(state.noiseStrength, state.noiseColor); state.showNoiseModal=false; ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (ImGui::Button("Cancel",ImVec2(100,0))){ state.showNoiseModal=false; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }

        // Curves Modal — interactive spline editor (mouse on graph moves points, not window)
        if (state.showCurvesModal) ImGui::OpenPopup("Curves##modal");
        if (ImGui::BeginPopupModal("Curves##modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (state.curvesPointsRGB.empty()) {
                state.curvesPointsRGB = {{0.f, 0.f}, {1.f, 1.f}};
                state.curvesLUTRGB = Canvas_BuildSplineLUT(state.curvesPointsRGB);
            }
            if (state.curvesPointsAlpha.empty()) {
                state.curvesPointsAlpha = {{0.f, 0.f}, {1.f, 1.f}};
                state.curvesLUTAlpha = Canvas_BuildSplineLUT(state.curvesPointsAlpha);
            }

            static const char* chanNames[] = {"RGB","Alpha"};
            UiCombo("##curves_chan", &state.curvesChannel, chanNames, 2, "Channel");
            ImGui::SameLine(); ImGui::TextDisabled("(right-click = remove point)");

            std::vector<std::pair<float,float>>& activePoints = (state.curvesChannel == 0) ? state.curvesPointsRGB : state.curvesPointsAlpha;
            std::vector<float>& activeLUT = (state.curvesChannel == 0) ? state.curvesLUTRGB : state.curvesLUTAlpha;

            const float graphSz = 256.f;
            const float pad = 8.f;

            // Child captures mouse so parent modal is not dragged from the graph area
            ImGui::BeginChild("##curves_graph_child", ImVec2(graphSz + pad * 2.f, graphSz + pad * 2.f),
                ImGuiChildFlags_Borders, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

            ImVec2 graphPos = ImGui::GetCursorScreenPos();
            graphPos.x += pad; graphPos.y += pad;
            ImGui::SetCursorScreenPos(graphPos);
            ImGui::InvisibleButton("##graph_bounds", ImVec2(graphSz, graphSz));
            const bool graphActive = ImGui::IsItemActive();
            const bool graphHovered = ImGui::IsItemHovered();

            ImDrawList* dl = ImGui::GetWindowDrawList();

            dl->AddRectFilled(graphPos, ImVec2(graphPos.x+graphSz,graphPos.y+graphSz), IM_COL32(30,30,30,255));
            for (int gi=1;gi<4;++gi) {
                float gx=graphPos.x+graphSz*gi/4.f, gy=graphPos.y+graphSz*gi/4.f;
                dl->AddLine(ImVec2(gx,graphPos.y),ImVec2(gx,graphPos.y+graphSz),IM_COL32(60,60,60,255));
                dl->AddLine(ImVec2(graphPos.x,gy),ImVec2(graphPos.x+graphSz,gy),IM_COL32(60,60,60,255));
            }
            dl->AddLine(graphPos,ImVec2(graphPos.x+graphSz,graphPos.y+graphSz),IM_COL32(80,80,80,255));
            dl->AddRect(graphPos,ImVec2(graphPos.x+graphSz,graphPos.y+graphSz),IM_COL32(120,120,120,255));

            if (!activePoints.empty())
                activeLUT = Canvas_BuildSplineLUT(activePoints);
            for (int xi=0;xi<255;++xi) {
                float x0=graphPos.x+xi, y0=graphPos.y+graphSz*(1.f-activeLUT[xi]);
                float x1=graphPos.x+xi+1, y1=graphPos.y+graphSz*(1.f-activeLUT[xi+1]);
                dl->AddLine(ImVec2(x0,y0),ImVec2(x1,y1),IM_COL32(220,220,220,255),1.5f);
            }

            static int draggingPt = -1;
            ImVec2 mpos = ImGui::GetIO().MousePos;
            bool inGraph = graphHovered || graphActive ||
                (mpos.x>=graphPos.x && mpos.x<=graphPos.x+graphSz && mpos.y>=graphPos.y && mpos.y<=graphPos.y+graphSz);

            for (int pi=0;pi<(int)activePoints.size();++pi) {
                float cx=graphPos.x+activePoints[pi].first*graphSz;
                float cy=graphPos.y+(1.f-activePoints[pi].second)*graphSz;
                bool hovered = fabsf(mpos.x-cx)<7.f && fabsf(mpos.y-cy)<7.f;
                dl->AddCircleFilled(ImVec2(cx,cy),5.f,hovered?IM_COL32(255,200,100,255):IM_COL32(200,200,200,255));
                dl->AddCircle(ImVec2(cx,cy),5.f,IM_COL32(60,60,60,255));

                if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) draggingPt=pi;
                if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && pi!=0 && pi!=(int)activePoints.size()-1) {
                    activePoints.erase(activePoints.begin()+pi);
                    activeLUT = Canvas_BuildSplineLUT(activePoints);
                    break;
                }
            }
            if (draggingPt>=0) {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    float nx=std::clamp((mpos.x-graphPos.x)/graphSz,0.f,1.f);
                    float ny=std::clamp(1.f-(mpos.y-graphPos.y)/graphSz,0.f,1.f);
                    if (draggingPt==0) nx=0.f;
                    if (draggingPt==(int)activePoints.size()-1) nx=1.f;
                    activePoints[draggingPt]={nx,ny};
                    std::sort(activePoints.begin(),activePoints.end(),[](auto&a,auto&b){return a.first<b.first;});
                    // re-find dragged index after sort (endpoints fixed at 0/1)
                    if (draggingPt > 0 && draggingPt < (int)activePoints.size()-1) {
                        // keep draggingPt as sorted index of moved point — approximate via nearest
                    }
                } else draggingPt=-1;
            }
            // Left-click empty graph = add point (InvisibleButton is active, so don't require !IsAnyItemActive)
            if (inGraph && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && draggingPt < 0) {
                bool onPoint = false;
                for (auto& pt : activePoints) {
                    float cx=graphPos.x+pt.first*graphSz;
                    float cy=graphPos.y+(1.f-pt.second)*graphSz;
                    if (fabsf(mpos.x-cx)<7.f && fabsf(mpos.y-cy)<7.f) { onPoint = true; break; }
                }
                if (!onPoint) {
                    float nx=std::clamp((mpos.x-graphPos.x)/graphSz,0.f,1.f);
                    float ny=std::clamp(1.f-(mpos.y-graphPos.y)/graphSz,0.f,1.f);
                    activePoints.push_back({nx,ny});
                    std::sort(activePoints.begin(),activePoints.end(),[](auto&a,auto&b){return a.first<b.first;});
                }
            }

            ImGui::EndChild();

            float posX=(mpos.x-graphPos.x)/graphSz*255.f, posY=(1.f-(mpos.y-graphPos.y)/graphSz)*255.f;
            if (inGraph) ImGui::Text("(%.0f, %.0f)", posX, posY);
            else ImGui::TextDisabled("Move mouse over graph");
            ImGui::Spacing();
            if (ImGui::Button("Reset",ImVec2(80,0))){
                activePoints={{0.f,0.f},{1.f,1.f}};
                activeLUT = Canvas_BuildSplineLUT(activePoints);
            }
            ImGui::SameLine();
            if (ImGui::Button("Apply",ImVec2(80,0))){
                canvas.ApplyCurves(state.curvesLUTRGB, state.curvesLUTAlpha);
                state.showCurvesModal=false; ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel",ImVec2(80,0))){ state.showCurvesModal=false; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
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
            case ActiveTool::PolygonalLasso: toolLabel = "Polygonal Lasso"; break;
            case ActiveTool::QuickSelect: toolLabel = "Quick Select"; break;
            case ActiveTool::MagicWand: toolLabel = "Magic Wand"; break;
            case ActiveTool::SmartSelect: toolLabel = "Smart Select"; break;
            case ActiveTool::MovePixels: toolLabel = "Transform"; break;
            case ActiveTool::Pipette: toolLabel = "Pipette"; break;
            case ActiveTool::BucketFill: toolLabel = "Bucket Fill"; break;
            case ActiveTool::Gradient: toolLabel = "Gradient"; break;
            case ActiveTool::Smudge: toolLabel = "Smudge"; break;
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
            ImGui::DockBuilderDockWindow("Channels", dock_right_middle_id);
            ImGui::DockBuilderDockWindow("Tool Settings", dock_right_bottom_id);

            ImGui::DockBuilderFinish(dockspace_id);
        }

        // 4. Modals Triggers
        if (state.openImportModal) { ImGui::OpenPopup("Import Image"); state.openImportModal = false; }
        if (state.openExportDdsModal) { ImGui::OpenPopup("Export DDS"); state.openExportDdsModal = false; }
        if (state.openExportStdModal) { ImGui::OpenPopup("Export Standard Image"); state.openExportStdModal = false; }
        if (state.openExportAdvancedModal) { ImGui::OpenPopup("Advanced Export Settings"); state.openExportAdvancedModal = false; }
        if (state.openSettingsModal) { ImGui::OpenPopup("Settings"); state.openSettingsModal = false; }
        if (state.openCanvasSizeModal) { ImGui::OpenPopup("Canvas Edit"); state.openCanvasSizeModal = false; }
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
                TriggerBackgroundOpenDocument(importPath, device, canvas);
                ImGui::CloseCurrentPopup();
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
            UiCombo("##ddsformat", &formatChoice, formatNames, IM_ARRAYSIZE(formatNames));
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
            ImGui::Text("Enter export path (PNG, JPG, BMP, TGA):");
            ImGui::InputText("##exportpathstd", exportPath, IM_ARRAYSIZE(exportPath));
            ImGui::SameLine();
            if (ImGui::Button("Browse...##exportstd")) {
                ShowSaveFileWin32(exportPath, IM_ARRAYSIZE(exportPath), "Image Files (*.png;*.jpg;*.jpeg;*.bmp;*.tga)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0All Files (*.*)\0*.*\0");
            }
            ImGui::Spacing();
            DrawIccPresetCombo(canvas, "ICC Profile (PNG)");
            ImGui::TextDisabled("Presets only — no free-text ICC path");
            ImGui::Separator();
            if (ImGui::Button("Export", ImVec2(120, 0))) {
                if (canvas.SaveCanvasStandard(exportPath, canvas.GetExportIccPreset())) {
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
                if (UiCombo("##cmb_currentFormatIdx", &currentFormatIdx, formats, IM_ARRAYSIZE(formats), "DDS Format / Preset")) {
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
                    if (UiCombo("##cmb_currentFilterIdx", &currentFilterIdx, filters, IM_ARRAYSIZE(filters), "Mip Filter")) {
                        canvas.SetExportMipFilter(filters[currentFilterIdx]);
                    }
                }
                
                const char* speeds[] = { "Fast", "Medium", "Slow", "Best" };
                static int currentSpeedIdx = 1;
                std::string currentSpeed = canvas.GetExportCompressionSpeed();
                for (int i = 0; i < IM_ARRAYSIZE(speeds); ++i) {
                    if (currentSpeed == speeds[i]) currentSpeedIdx = i;
                }
                if (UiCombo("##cmb_currentSpeedIdx", &currentSpeedIdx, speeds, IM_ARRAYSIZE(speeds), "Compression Quality")) {
                    canvas.SetExportCompressionSpeed(speeds[currentSpeedIdx]);
                }
            } else if (ext == "png") {
                ImGui::TextColored(ImVec4(0.2f, 0.7f, 1.0f, 1.0f), "PNG Export Settings");
                DrawIccPresetCombo(canvas, "ICC Profile");
                ImGui::TextDisabled("Presets only — no free-text ICC path");
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
                    success = canvas.SaveCanvasStandard(exportPath, canvas.GetExportIccPreset());
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

                    if (UiCombo("##cmb_currentThemeIdx", &currentThemeIdx, themes, IM_ARRAYSIZE(themes), "Theme")) {
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

        // Canvas Edit Popup Modal (Extend | Resize + resample algorithm)
        if (ImGui::BeginPopupModal("Canvas Edit", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            static int targetW = 0;
            static int targetH = 0;
            static int editMode = 0; // 0=Extend, 1=Resize
            static int resample = 1; // 0=Nearest, 1=Bilinear, 2=Lanczos
            static bool initSize = false;
            if (!initSize) {
                targetW = canvas.GetWidth();
                targetH = canvas.GetHeight();
                initSize = true;
            }

            ImGui::Text("Canvas: %d x %d", canvas.GetWidth(), canvas.GetHeight());
            ImGui::Separator();
            ImGui::RadioButton("Extend (pad/crop, no scale)", &editMode, 0);
            ImGui::RadioButton("Resize (scale content)", &editMode, 1);
            ImGui::Separator();
            ImGui::InputInt("Width", &targetW, 128, 256);
            ImGui::InputInt("Height", &targetH, 128, 256);
            if (targetW < 1) targetW = 1;
            if (targetH < 1) targetH = 1;

            if (editMode == 1) {
                const char* filters[] = { "Nearest", "Bilinear", "Lanczos" };
                UiCombo("##cmb_resample", &resample, filters, IM_ARRAYSIZE(filters), "Algorithm");
            } else {
                ImGui::TextDisabled("Extend keeps pixels 1:1; content is centered.");
            }

            ImGui::Separator();
            if (ImGui::Button("Apply", ImVec2(120, 0))) {
                auto mode = (editMode == 0) ? Canvas::CanvasEditMode::Extend : Canvas::CanvasEditMode::Resize;
                auto filter = Canvas::ResampleFilter::Bilinear;
                if (resample == 0) filter = Canvas::ResampleFilter::Nearest;
                else if (resample == 2) filter = Canvas::ResampleFilter::Lanczos;
                canvas.EditCanvas(device, mode, targetW, targetH, filter, 0.5f, 0.5f);
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
                TriggerBackgroundOpenDocument(loadPath, device, canvas);
                ImGui::CloseCurrentPopup();
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
                TriggerBackgroundOpenDocument(state.backupPath, device, canvas);
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
            ImGuiWindow* window = ImGui::FindWindowByName("Toolbar");
            bool isVertical = true;
            if (window) {
                isVertical = (window->Size.y > window->Size.x);
            }
            // Size constraints apply when floating; when docked, also clamp dock node cross-axis
            if (isVertical) {
                ImGui::SetNextWindowSizeConstraints(ImVec2(16.0f, 100.0f), ImVec2(64.0f, 16384.0f));
            } else {
                ImGui::SetNextWindowSizeConstraints(ImVec2(100.0f, 16.0f), ImVec2(16384.0f, 64.0f));
            }
            Ui::BeginDockPanel("Toolbar", &state.showToolbar);

            // Docked toolbar: Dear ImGui ignores SetNextWindowSizeConstraints on docked
            // windows — size is owned by the DockNode. Clamp SizeRef every frame so the
            // strip stays thin while docked (floating still uses constraints above).
            if (ImGuiWindow* tw = ImGui::GetCurrentWindow()) {
                if (tw->DockNode && !tw->DockNode->IsFloatingNode() && tw->DockNode->HostWindow) {
                    ImGuiDockNode* node = tw->DockNode;
                    // Only constrain leaf nodes that host this toolbar (not the whole dockspace)
                    if (!node->IsSplitNode()) {
                        if (isVertical) {
                            float w = std::clamp(node->SizeRef.x > 1.f ? node->SizeRef.x : node->Size.x, 36.0f, 64.0f);
                            node->SizeRef.x = w;
                            if (std::fabs(node->Size.x - w) > 1.0f)
                                ImGui::DockBuilderSetNodeSize(node->ID, ImVec2(w, node->Size.y));
                        } else {
                            float h = std::clamp(node->SizeRef.y > 1.f ? node->SizeRef.y : node->Size.y, 36.0f, 64.0f);
                            node->SizeRef.y = h;
                            if (std::fabs(node->Size.y - h) > 1.0f)
                                ImGui::DockBuilderSetNodeSize(node->ID, ImVec2(node->Size.x, h));
                        }
                    }
                }
            }

            ImVec2 avail = ImGui::GetContentRegionAvail();

            std::string brushBind = KeymapManager::Get().GetActionShortcutString("BrushTool");
            std::string eraserBind = KeymapManager::Get().GetActionShortcutString("EraserTool");
            std::string panBind = KeymapManager::Get().GetActionShortcutString("PanTool");
            std::string rotateBind = KeymapManager::Get().GetActionShortcutString("RotateTool");
            std::string fillBind = KeymapManager::Get().GetActionShortcutString("BucketFillTool");
            std::string gradientBind = KeymapManager::Get().GetActionShortcutString("GradientTool");
            std::string pipetteBind = KeymapManager::Get().GetActionShortcutString("PipetteTool");
            std::string smudgeBind  = KeymapManager::Get().GetActionShortcutString("SmudgeTool");
            std::string selectBind = KeymapManager::Get().GetActionShortcutString("SelectToolGroup");
            std::string lassoBind = KeymapManager::Get().GetActionShortcutString("LassoToolGroup");
            std::string wandBind = KeymapManager::Get().GetActionShortcutString("WandToolGroup");
            std::string transformBind = KeymapManager::Get().GetActionShortcutString("TransformTool");

            static const ToolVariant s_SelectVariants[] = {
                { "RectSelectTool", "Rectangular Selection", ActiveTool::RectSelect },
                { "EllipseSelectTool", "Ellipse Selection", ActiveTool::EllipseSelect },
            };
            static const ToolVariant s_LassoVariants[] = {
                { "LassoSelectTool", "Lasso Selection", ActiveTool::LassoSelect },
                { "PolygonalLassoTool", "Polygonal Lasso", ActiveTool::PolygonalLasso },
            };
            static const ToolVariant s_WandVariants[] = {
                { "MagicWandTool", "Magic Wand", ActiveTool::MagicWand },
                { "QuickSelectTool", "Quick Selection", ActiveTool::QuickSelect },
                { "SmartSelectTool", "Smart Select", ActiveTool::SmartSelect },
            };

            static std::string s_RebindAction = "";
            constexpr int kToolbarButtonCount = 11;
            const bool hasSeparator = true;
            // Adaptive icon size from dock/window content region (works docked + floating)
            float btnSize = ComputeAdaptiveToolButtonSize(avail, isVertical, kToolbarButtonCount + 1 /*+Reset*/, hasSeparator);
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
            RenderToolButton("SmudgeTool", "Smudge", ActiveTool::Smudge, false, smudgeBind, btnSize, s_RebindAction, activeTool, brush, canvas);
            ToolbarAdvance(isVertical, gap);
            RenderToolButton("PipetteTool", "Pipette", ActiveTool::Pipette, false, pipetteBind, btnSize, s_RebindAction, activeTool, brush, canvas);
            ToolbarAdvance(isVertical, gap);
            RenderGroupedToolButton("SelectGroup", "SelectToolGroup", s_SelectVariants, IM_ARRAYSIZE(s_SelectVariants),
                "Selection Tools", selectBind, btnSize, s_RebindAction, activeTool);
            ToolbarAdvance(isVertical, gap);
            RenderGroupedToolButton("LassoGroup", "LassoToolGroup", s_LassoVariants, IM_ARRAYSIZE(s_LassoVariants),
                "Lasso Tools", lassoBind, btnSize, s_RebindAction, activeTool);
            ToolbarAdvance(isVertical, gap);
            RenderGroupedToolButton("WandGroup", "WandToolGroup", s_WandVariants, IM_ARRAYSIZE(s_WandVariants),
                "Wand / Selection Tools", wandBind, btnSize, s_RebindAction, activeTool);
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

            Ui::EndDockPanel();
        }

        // 6a. Viewport Navigation (Stage 2c)
        if (state.showViewportNav) {
            Ui::BeginDockPanel("Viewport Navigation", &state.showViewportNav);
            ImGui::Text("Zoom: %.0f%%", canvas.GetZoom() * 100.0f);
            ImGui::Text("Pan: (%.1f, %.1f)", canvas.GetPan().x, canvas.GetPan().y);
            ImGui::Spacing();
            bool flipH = canvas.GetViewportFlipH();
            if (ImGui::Checkbox("Flip H", &flipH)) canvas.SetViewportFlipH(flipH);
            ImGui::SameLine();
            bool flipV = canvas.GetViewportFlipV();
            if (ImGui::Checkbox("Flip V", &flipV)) canvas.SetViewportFlipV(flipV);
            float rotAngle = canvas.GetRotationAngle() * (180.0f / 3.14159265f);
            if (ImGui::SliderFloat("Rotation", &rotAngle, -180.0f, 180.0f, "%.1f°")) {
                canvas.SetRotationAngle(rotAngle * (3.14159265f / 180.0f));
            }
            if (ImGui::Button("Reset Viewport", ImVec2(-1, 0)))
                canvas.ResetView();
            Ui::EndDockPanel();
        }

        // 6b. Properties — project / export only
        if (state.showProperties) {
            Ui::BeginDockPanel("Properties", &state.showProperties);
            
            ImGui::Text("Project Properties:");
            int pType = (canvas.GetProjectType() == Canvas::ProjectType::Simple) ? 0 : 1;
            const char* pTypeNames[] = { "Simple Project", "Advanced Project (.rayp)" };
            if (UiCombo("##cmb_pType", &pType, pTypeNames, IM_ARRAYSIZE(pTypeNames), "Project Type")) {
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
            ImGui::Text("Project Output Format (DDS ↔ PNG):");
            ImGui::TextDisabled("Sets default Quick Export target for this project.");

            char propExportPath[512] = "";
            std::strncpy(propExportPath, canvas.GetExportPath().c_str(), sizeof(propExportPath));

            // Derive current format family from path extension (default PNG)
            std::string pathStr = propExportPath;
            size_t dot = pathStr.find_last_of('.');
            std::string ext = "";
            if (dot != std::string::npos) {
                ext = pathStr.substr(dot + 1);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            }
            int outFmt = (ext == "dds") ? 1 : 0; // 0=PNG, 1=DDS
            const char* outFmtNames[] = { "PNG / Standard image", "DDS (compressed)" };
            if (UiCombo("##cmb_outFmt", &outFmt, outFmtNames, IM_ARRAYSIZE(outFmtNames), "Output Type")) {
                // Switch extension on the export path
                std::string base = propExportPath;
                size_t d = base.find_last_of('.');
                if (d != std::string::npos) base = base.substr(0, d);
                if (base.empty()) base = "export";
                base += (outFmt == 1) ? ".dds" : ".png";
                std::strncpy(propExportPath, base.c_str(), sizeof(propExportPath) - 1);
                propExportPath[sizeof(propExportPath) - 1] = '\0';
                canvas.SetExportPath(propExportPath);
                ext = (outFmt == 1) ? "dds" : "png";
            }

            ImGui::InputText("Export Path", propExportPath, IM_ARRAYSIZE(propExportPath));
            ImGui::SameLine();
            if (ImGui::Button("...##propExportPath")) {
                if (ShowSaveFileWin32(propExportPath, IM_ARRAYSIZE(propExportPath),
                    "PNG (*.png)\0*.png\0DDS (*.dds)\0*.dds\0All Files (*.*)\0*.*\0")) {
                    canvas.SetExportPath(propExportPath);
                    pathStr = propExportPath;
                    dot = pathStr.find_last_of('.');
                    ext.clear();
                    if (dot != std::string::npos) {
                        ext = pathStr.substr(dot + 1);
                        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    }
                }
            } else if (std::string(propExportPath) != canvas.GetExportPath()) {
                canvas.SetExportPath(propExportPath);
            }

            // Re-read ext after edits
            pathStr = canvas.GetExportPath();
            dot = pathStr.find_last_of('.');
            ext.clear();
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
                if (UiCombo("##cmb_currentFormatIdx", &currentFormatIdx, formats, IM_ARRAYSIZE(formats), "DDS Preset")) {
                    canvas.SetExportFormat(formats[currentFormatIdx]);
                }
                bool mips = canvas.GetExportGenerateMipMaps();
                if (ImGui::Checkbox("Mipmaps", &mips)) canvas.SetExportGenerateMipMaps(mips);
                if (mips) {
                    const char* filters[] = { "Point", "Box", "Linear", "Cubic" };
                    int currentFilterIdx = 3;
                    std::string currentFilter = canvas.GetExportMipFilter();
                    for (int i = 0; i < IM_ARRAYSIZE(filters); ++i) {
                        if (currentFilter == filters[i]) currentFilterIdx = i;
                    }
                    if (UiCombo("##cmb_currentFilterIdx", &currentFilterIdx, filters, IM_ARRAYSIZE(filters), "Mip Filter")) {
                        canvas.SetExportMipFilter(filters[currentFilterIdx]);
                    }
                }
            } else {
                DrawIccPresetCombo(canvas, "ICC Profile");
            }

            if (ImGui::Button("Quick Export (project format)", ImVec2(-1, 0))) {
                state.openQuickExportTrigger = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Export using the path/format above (same as Ctrl+E)");

            Ui::EndDockPanel();
        }

        if (state.showLayers) {
            Ui::BeginDockPanel("Layers", &state.showLayers);
            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows)) {
                g_IsLayersHovered = true;
            }

            auto& layers = canvas.GetLayers();
            auto& sel = state.selectedLayers;
            auto pruneSel = [&]() {
                sel.erase(std::remove_if(sel.begin(), sel.end(),
                    [&](int i) { return i < 0 || i >= (int)layers.size(); }), sel.end());
            };
            pruneSel();
            auto isLayerSelected = [&](int i) {
                return std::find(sel.begin(), sel.end(), i) != sel.end();
            };
            auto setSoleSelection = [&](int i) {
                sel.clear();
                if (i >= 0) { sel.push_back(i); state.layerSelectAnchor = i; }
            };
            auto toggleSelection = [&](int i) {
                auto it = std::find(sel.begin(), sel.end(), i);
                if (it != sel.end()) sel.erase(it);
                else sel.push_back(i);
                state.layerSelectAnchor = i;
            };
            auto rangeSelect = [&](int i) {
                if (state.layerSelectAnchor < 0) { setSoleSelection(i); return; }
                // Visual list is high→low; range in index space
                int a = state.layerSelectAnchor, b = i;
                if (a > b) std::swap(a, b);
                sel.clear();
                for (int k = a; k <= b; ++k) sel.push_back(k);
            };
            auto targetsForAction = [&]() -> std::vector<int> {
                if (!sel.empty()) return sel;
                int a = canvas.GetActiveLayerIndex();
                if (a >= 0) return { a };
                return {};
            };

            // Fixed top: active layer opacity + blend (not per-row — saves list space)
            {
                int ai = canvas.GetActiveLayerIndex();
                if (ai >= 0 && ai < (int)layers.size()) {
                    auto& al = layers[ai];
                    ImGui::PushID("##active_hdr");
                    ImGui::TextDisabled("%s", al.name.c_str());
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
                    if (ImGui::SliderFloat("##op_top", &al.opacity, 0.f, 1.f, "Opacity %.2f"))
                        canvas.MarkCompositeDirty();
                    ImGui::SameLine();
                    static const char* blendNamesTop[] = {
                        "Normal","Multiply","Screen","Overlay","Add","Subtract","Darken","Lighten","HardLight","SoftLight"
                    };
                    int blendIdx = (int)al.blendMode;
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                    if (UiCombo("##bl_top", &blendIdx, blendNamesTop, IM_ARRAYSIZE(blendNamesTop))) {
                        al.blendMode = (BlendMode)blendIdx;
                        canvas.MarkCompositeDirty();
                    }
                    ImGui::PopID();
                    ImGui::Separator();
                }
            }

            // List fills remaining height minus bottom bar
            const float barH = 40.f;
            ImGui::BeginChild("LayersList", ImVec2(0, -barH), true);
            const float thumb = 28.0f;
            auto& tok = Ui::Tokens();

            for (int i = static_cast<int>(layers.size()) - 1; i >= 0; --i) {
                auto& layer = layers[i];

                bool parentCollapsed = false;
                int currParentId = layer.parentGroupId;
                while (currParentId >= 0 && currParentId < (int)layers.size()) {
                    if (!layers[currParentId].groupExpanded) { parentCollapsed = true; break; }
                    currParentId = layers[currParentId].parentGroupId;
                }
                if (parentCollapsed) continue;

                ImGui::PushID(i);

                int depth = 0;
                for (int pId = layer.parentGroupId; pId >= 0 && pId < (int)layers.size(); pId = layers[pId].parentGroupId)
                    depth++;
                if (depth > 0) ImGui::Indent(12.0f * depth);

                // Visibility
                bool isIsolated = canvas.IsLayerIsolated(i);
                if (isIsolated) ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.45f, 0.15f, 1.0f));
                bool vis = layer.visible;
                if (ImGui::Checkbox("##vis", &vis)) {
                    if (ImGui::GetIO().KeyAlt) canvas.ToggleLayerIsolation(i);
                    else { layer.visible = vis; canvas.MarkCompositeDirty(); }
                }
                if (isIsolated) ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Visibility\nAlt+Click: Isolate");
                ImGui::SameLine(0, 4);

                if (layer.isGroup) {
                    if (ImGui::ArrowButton("##exp", layer.groupExpanded ? ImGuiDir_Down : ImGuiDir_Right))
                        layer.groupExpanded = !layer.groupExpanded;
                    ImGui::SameLine(0, 2);
                }

                if (!layer.isGroup && layer.srv) {
                    bool isActiveContent = (canvas.GetActiveLayerIndex() == i && canvas.GetPaintTarget() == PaintTarget::LayerContent);
                    if (isActiveContent) {
                        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.2f, 0.6f, 1.0f, 1.0f));
                        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
                    } else {
                        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
                        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                    }
                    if (ImGui::ImageButton("##thumb", (ImTextureID)layer.srv, ImVec2(thumb, thumb), ImVec2(0,0), ImVec2(1,1))) {
                        ImGuiIO& io = ImGui::GetIO();
                        if (io.KeyAlt) {
                            // Alt+Click thumb: select opaque pixels (was Ctrl — multi-select uses Ctrl)
                            canvas.SelectOpaquePixels(i);
                            canvas.UpdateSelectionMaskTexture(device);
                        } else if (io.KeyCtrl) {
                            toggleSelection(i);
                            canvas.SetActiveLayerIndex(i);
                            canvas.SetPaintTarget(PaintTarget::LayerContent);
                        } else if (io.KeyShift) {
                            rangeSelect(i);
                            canvas.SetActiveLayerIndex(i);
                            canvas.SetPaintTarget(PaintTarget::LayerContent);
                        } else {
                            setSoleSelection(i);
                            canvas.SetActiveLayerIndex(i);
                            canvas.SetPaintTarget(PaintTarget::LayerContent);
                        }
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Content\nClick: select  ·  Ctrl: multi  ·  Shift: range\nAlt+Click: select opaque");
                    if (layer.type == Layer::Type::SmartObject || layer.type == Layer::Type::VectorSvg) {
                        ImVec2 tmin = ImGui::GetItemRectMin();
                        ImVec2 tmax = ImGui::GetItemRectMax();
                        const char* badge = (layer.type == Layer::Type::VectorSvg) ? "V" : "S";
                        ImGui::GetWindowDrawList()->AddRectFilled(
                            ImVec2(tmax.x - 10, tmin.y), ImVec2(tmax.x, tmin.y + 10), IM_COL32(40,120,220,230), 2.f);
                        ImGui::GetWindowDrawList()->AddText(ImVec2(tmax.x - 8, tmin.y - 1), IM_COL32(255,255,255,255), badge);
                    }
                    ImGui::PopStyleVar();
                    ImGui::PopStyleColor();
                    ImGui::SameLine(0, 2);
                } else if (layer.isGroup) {
                    ImGui::Button("G##g", ImVec2(thumb, thumb));
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                                "LAYER_INDEX",
                                ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                            int draggedIdx = *(const int*)payload->Data;
                            bool ok = draggedIdx >= 0 && draggedIdx < (int)layers.size() &&
                                      draggedIdx != i && !layers[draggedIdx].isGroup &&
                                      !IsGroupAncestorOf(layers, draggedIdx, i);
                            if (ok) {
                                DrawLayerDropHighlight(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), true);
                                if (payload->IsDelivery()) {
                                    canvas.SetActiveLayerIndex(canvas.MoveLayerIntoGroup(draggedIdx, i));
                                    canvas.MarkCompositeDirty();
                                }
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                    ImGui::SameLine(0, 2);
                }

                // Mask: smart +M (selection -> from selection, else blank)
                if (!layer.isGroup) {
                    if (layer.hasMask && layer.maskSRV) {
                        bool isActiveMask = (canvas.GetActiveLayerIndex() == i && canvas.GetPaintTarget() == PaintTarget::LayerMask);
                        if (isActiveMask) {
                            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.2f, 0.6f, 1.0f, 1.0f));
                            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
                        } else {
                            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
                            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                        }
                        if (ImGui::ImageButton("##mask", (ImTextureID)layer.maskSRV, ImVec2(thumb, thumb), ImVec2(0,0), ImVec2(1,1))) {
                            canvas.SetActiveLayerIndex(i);
                            canvas.SetPaintTarget(PaintTarget::LayerMask);
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Layer Mask\nRight-click: Apply / Delete");
                        if (ImGui::BeginPopupContextItem("##maskctx")) {
                            if (ImGui::MenuItem("Apply Mask")) canvas.ApplyLayerMask(i);
                            if (ImGui::MenuItem("Delete Mask")) canvas.DeleteLayerMask(i);
                            ImGui::EndPopup();
                        }
                        ImGui::PopStyleVar();
                        ImGui::PopStyleColor();
                    } else {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.15f, 0.55f));
                        if (ImGui::Button("+M##addm", ImVec2(thumb, thumb))) {
                            if (canvas.HasSelection())
                                canvas.CreateLayerMaskFromSelection(device, i);
                            else
                                canvas.CreateLayerMask(device, i);
                            canvas.SetPaintTarget(PaintTarget::LayerMask);
                        }
                        ImGui::PopStyleColor();
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip(canvas.HasSelection()
                                ? "Add Layer Mask from Selection"
                                : "Add Layer Mask");
                        }
                    }
                    ImGui::SameLine(0, 4);
                }

                const bool isActive = (canvas.GetActiveLayerIndex() == i);
                const bool multiSel = isLayerSelected(i);
                char label[256];
                if (layer.isGroup) std::snprintf(label, sizeof(label), "[G] %s", layer.name.c_str());
                else if (layer.type == Layer::Type::VectorSvg) std::snprintf(label, sizeof(label), "[SVG] %s", layer.name.c_str());
                else if (layer.type == Layer::Type::SmartObject) std::snprintf(label, sizeof(label), "[SO] %s", layer.name.c_str());
                else std::snprintf(label, sizeof(label), "%s", layer.name.c_str());

                // Selection fill (multi) behind name — reserve only for FX chip
                float rightReserve = 36.0f;
                float nameW = ImGui::GetContentRegionAvail().x - rightReserve;
                if (nameW < 40.f) nameW = 40.f;

                if (multiSel && !isActive) {
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + nameW, p.y + thumb),
                        tok.ColU32(ImVec4(tok.accent.x, tok.accent.y, tok.accent.z, 0.18f)), tok.rSm);
                }

                if (ImGui::Selectable(label, isActive, ImGuiSelectableFlags_None, ImVec2(nameW, thumb))) {
                    ImGuiIO& io = ImGui::GetIO();
                    if (io.KeyShift) {
                        rangeSelect(i);
                        canvas.SetActiveLayerIndex(i);
                    } else if (io.KeyCtrl) {
                        toggleSelection(i);
                        canvas.SetActiveLayerIndex(i);
                    } else {
                        setSoleSelection(i);
                        canvas.SetActiveLayerIndex(i);
                    }
                }
                // Active = stronger outline
                if (isActive) {
                    ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
                    ImGui::GetWindowDrawList()->AddRect(mn, mx, tok.ColU32(tok.strokeActive), tok.rSm, 0, 1.5f);
                }
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover)) {
                    ImGui::SetDragDropPayload("LAYER_INDEX", &i, sizeof(int));
                    ImGui::Text("%s", layer.name.c_str());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                            "LAYER_INDEX",
                            ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                        int draggedIdx = *(const int*)payload->Data;
                        if (draggedIdx >= 0 && draggedIdx < (int)layers.size() && draggedIdx != i) {
                            const bool intoGroup = layer.isGroup && !layers[draggedIdx].isGroup &&
                                                   !IsGroupAncestorOf(layers, draggedIdx, i);
                            DrawLayerDropHighlight(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), intoGroup);
                            if (payload->IsDelivery()) {
                                if (intoGroup) {
                                    canvas.SetActiveLayerIndex(canvas.MoveLayerIntoGroup(draggedIdx, i));
                                } else {
                                    int targetParentOrig = layer.parentGroupId;
                                    int newIdx = canvas.ReorderLayer(draggedIdx, i);
                                    auto mapIdx = [](int j, int fromIdx, int toIdx) {
                                        if (fromIdx == toIdx) return j;
                                        if (fromIdx < toIdx) {
                                            if (j == fromIdx) return toIdx;
                                            if (j > fromIdx && j <= toIdx) return j - 1;
                                            return j;
                                        }
                                        if (j == fromIdx) return toIdx;
                                        if (j >= toIdx && j < fromIdx) return j + 1;
                                        return j;
                                    };
                                    int newParent = mapIdx(targetParentOrig, draggedIdx, i);
                                    auto& L = canvas.GetLayers();
                                    if (newParent == newIdx || newParent < 0 || newParent >= (int)L.size()) newParent = -1;
                                    if (newParent >= 0 && !L[newParent].isGroup) newParent = -1;
                                    L[newIdx].parentGroupId = newParent;
                                    canvas.SetActiveLayerIndex(newIdx);
                                }
                                canvas.MarkCompositeDirty();
                            }
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                if (ImGui::BeginPopupContextItem("##lyrctx")) {
                    if (ImGui::MenuItem("Remove from Group", nullptr, false, layer.parentGroupId != -1))
                        canvas.RemoveLayerFromGroup(i);
                    if (ImGui::BeginMenu("Add to Group")) {
                        for (int g = 0; g < (int)layers.size(); ++g) {
                            if (layers[g].isGroup && g != i && ImGui::MenuItem(layers[g].name.c_str()))
                                canvas.AddLayerToGroup(i, g);
                        }
                        ImGui::EndMenu();
                    }
                    if (!layer.isGroup && (layer.type == Layer::Type::SmartObject || layer.type == Layer::Type::VectorSvg)) {
                        ImGui::Separator();
                        if (ImGui::MenuItem("Rasterize Layer")) canvas.RasterizeLayer(device, i);
                    }
                    if (!layer.isGroup) {
                        ImGui::Separator();
                        if (!layer.hasMask) {
                            if (ImGui::MenuItem(canvas.HasSelection() ? "Add Mask from Selection" : "Add Mask")) {
                                if (canvas.HasSelection()) canvas.CreateLayerMaskFromSelection(device, i);
                                else canvas.CreateLayerMask(device, i);
                            }
                        } else {
                            if (ImGui::MenuItem("Apply Mask")) canvas.ApplyLayerMask(i);
                            if (ImGui::MenuItem("Delete Mask")) canvas.DeleteLayerMask(i);
                        }
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Duplicate", "Ctrl+J")) {
                        canvas.DuplicateLayer(device, i);
                        setSoleSelection(canvas.GetActiveLayerIndex());
                    }
                    if (layers.size() > 1 && ImGui::MenuItem("Delete Layer")) {
                        canvas.DeleteLayer(i);
                        pruneSel();
                    }
                    ImGui::EndPopup();
                }

                // FX opens dedicated Layer Effects dock (Stage 2c)
                ImGui::SameLine(0, 2);
                bool hasFx = !layer.filters.empty();
                if (hasFx) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.9f, 0.55f));
                if (ImGui::SmallButton("Fx")) {
                    canvas.SetActiveLayerIndex(i);
                    setSoleSelection(i);
                    state.showLayerEffects = true;
                    state.layerEffectsFocusIdx = layer.filters.empty() ? -1 : 0;
                }
                if (hasFx) ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Layer Effects…");

                if (depth > 0) ImGui::Unindent(12.0f * depth);
                ImGui::PopID();
            }
            ImGui::EndChild();

            // Bottom action bar (fixed icons) — Add / Group / Duplicate / Delete
            {
                ImGui::Separator();
                const float iconSz = 32.f;
                auto doAdd = [&]() {
                    canvas.CreateNewLayer(device, "Layer " + std::to_string(layers.size() + 1));
                    setSoleSelection(canvas.GetActiveLayerIndex());
                };
                auto doGroup = [&]() {
                    canvas.CreateLayerGroup(device, "Group " + std::to_string(layers.size() + 1));
                    setSoleSelection(canvas.GetActiveLayerIndex());
                };
                auto doDup = [&]() {
                    auto t = targetsForAction();
                    if (t.empty()) return;
                    canvas.DuplicateLayers(device, t);
                    setSoleSelection(canvas.GetActiveLayerIndex());
                };
                auto doDel = [&]() {
                    auto t = targetsForAction();
                    if (t.empty()) return;
                    canvas.DeleteLayers(t);
                    sel.clear();
                    if (canvas.GetActiveLayerIndex() >= 0)
                        setSoleSelection(canvas.GetActiveLayerIndex());
                };

                float gap = 8.f;
                float total = iconSz * 4 + gap * 3;
                float startX = std::max(0.f, (ImGui::GetContentRegionAvail().x - total) * 0.5f);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + startX);

                if (Ui::IconButton("##addL", "layer_add", ImVec2(iconSz, iconSz), "Add Layer").clicked)
                    doAdd();
                ImGui::SameLine(0, gap);
                if (Ui::IconButton("##addG", "layer_group_add", ImVec2(iconSz, iconSz), "Add Group").clicked)
                    doGroup();
                ImGui::SameLine(0, gap);
                if (Ui::IconButton("##dup", "layer_duplicate", ImVec2(iconSz, iconSz), "Duplicate (Ctrl+J)").clicked)
                    doDup();
                ImGui::SameLine(0, gap);
                if (Ui::IconButton("##del", "layer_delete", ImVec2(iconSz, iconSz), "Delete selection / active").clicked)
                    doDel();
            }

            Ui::EndDockPanel();
        }

        // Layer Effects dock (Photoshop-like FX stack for active layer)
        if (state.showLayerEffects) {
            Ui::BeginDockPanel("Layer Effects", &state.showLayerEffects);
            int ai = canvas.GetActiveLayerIndex();
            auto& L = canvas.GetLayers();
            if (ai < 0 || ai >= (int)L.size()) {
                ImGui::TextDisabled("No active layer");
            } else if (L[ai].isGroup) {
                ImGui::TextDisabled("Groups have no pixel filters");
            } else {
                Layer& layer = L[ai];
                ImGui::Text("%s", layer.name.c_str());
                ImGui::Separator();

                static const char* filterTypeNames[] = {"Blur","HSV","Curves","Alpha Invert","Noise"};
                static const FilterType ftypes[] = {
                    FilterType::Blur, FilterType::HSV, FilterType::Curves,
                    FilterType::AlphaInvert, FilterType::Noise
                };

                // Left: stack list with reorder
                float listW = ImGui::GetContentRegionAvail().x * 0.42f;
                ImGui::BeginChild("##fxlist", ImVec2(listW, 0), true);
                if (layer.filters.empty())
                    ImGui::TextDisabled("No effects — Add below");

                int removeIdx = -1;
                int moveFrom = -1, moveTo = -1;
                for (int fi = 0; fi < (int)layer.filters.size(); ++fi) {
                    ImGui::PushID(fi);
                    LayerFilter& flt = layer.filters[fi];
                    bool en = flt.enabled;
                    if (ImGui::Checkbox("##en", &en)) {
                        flt.enabled = en;
                        layer.filtersDirty = true;
                        canvas.MarkCompositeDirty();
                    }
                    ImGui::SameLine();
                    bool sel = (state.layerEffectsFocusIdx == fi);
                    char row[64];
                    std::snprintf(row, sizeof(row), "%s##fxrow", filterTypeNames[(int)flt.type]);
                    if (ImGui::Selectable(row, sel, 0, ImVec2(0, 0)))
                        state.layerEffectsFocusIdx = fi;

                    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                        ImGui::SetDragDropPayload("FX_INDEX", &fi, sizeof(int));
                        ImGui::Text("%s", filterTypeNames[(int)flt.type]);
                        ImGui::EndDragDropSource();
                    }
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("FX_INDEX")) {
                            int from = *(const int*)p->Data;
                            if (from != fi) { moveFrom = from; moveTo = fi; }
                        }
                        ImGui::EndDragDropTarget();
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("✕")) removeIdx = fi;
                    ImGui::PopID();
                }
                if (moveFrom >= 0 && moveTo >= 0) {
                    LayerFilter tmp = layer.filters[moveFrom];
                    layer.filters.erase(layer.filters.begin() + moveFrom);
                    if (moveTo > moveFrom) moveTo--;
                    layer.filters.insert(layer.filters.begin() + moveTo, std::move(tmp));
                    layer.filtersDirty = true;
                    canvas.MarkCompositeDirty();
                    state.layerEffectsFocusIdx = moveTo;
                }
                if (removeIdx >= 0) {
                    layer.filters.erase(layer.filters.begin() + removeIdx);
                    layer.filtersDirty = true;
                    canvas.MarkCompositeDirty();
                    if (state.layerEffectsFocusIdx >= (int)layer.filters.size())
                        state.layerEffectsFocusIdx = (int)layer.filters.size() - 1;
                }

                ImGui::Spacing();
                if (ImGui::BeginMenu("Add Effect")) {
                    for (int ti = 0; ti < 5; ++ti) {
                        if (ImGui::MenuItem(filterTypeNames[ti])) {
                            LayerFilter nf;
                            nf.type = ftypes[ti];
                            nf.enabled = true;
                            if (ftypes[ti] == FilterType::Blur) nf.p[0] = 5.f;
                            if (ftypes[ti] == FilterType::Curves) {
                                nf.lut.resize(256);
                                for (int li = 0; li < 256; ++li) nf.lut[li] = (float)li / 255.f;
                            }
                            layer.filters.push_back(nf);
                            layer.filtersDirty = true;
                            canvas.MarkCompositeDirty();
                            state.layerEffectsFocusIdx = (int)layer.filters.size() - 1;
                        }
                    }
                    ImGui::EndMenu();
                }
                ImGui::EndChild();

                ImGui::SameLine();
                // Right: params for focused effect
                ImGui::BeginChild("##fxparams", ImVec2(0, 0), true);
                int fi = state.layerEffectsFocusIdx;
                if (fi < 0 || fi >= (int)layer.filters.size()) {
                    ImGui::TextDisabled("Select an effect");
                } else {
                    LayerFilter& flt = layer.filters[fi];
                    ImGui::Text("%s", filterTypeNames[(int)flt.type]);
                    ImGui::Separator();
                    bool dirty = false;
                    switch (flt.type) {
                    case FilterType::Blur:
                        dirty |= ImGui::SliderFloat("Radius", &flt.p[0], 0.5f, 80.f, "%.1f");
                        break;
                    case FilterType::HSV:
                        dirty |= ImGui::SliderFloat("Hue", &flt.p[0], -0.5f, 0.5f);
                        dirty |= ImGui::SliderFloat("Sat", &flt.p[1], -1.f, 1.f);
                        dirty |= ImGui::SliderFloat("Val", &flt.p[2], -1.f, 1.f);
                        break;
                    case FilterType::Noise:
                        dirty |= ImGui::SliderFloat("Strength", &flt.p[0], 0.f, 1.f);
                        { bool col = flt.p[1] > 0.5f;
                          if (ImGui::Checkbox("Color noise", &col)) { flt.p[1] = col ? 1.f : 0.f; dirty = true; } }
                        break;
                    case FilterType::AlphaInvert:
                        ImGui::TextDisabled("No parameters");
                        break;
                    case FilterType::Curves:
                        ImGui::TextDisabled("Edit via Image → Curves, or tune LUT later");
                        break;
                    }
                    if (dirty) {
                        layer.filtersDirty = true;
                        canvas.MarkCompositeDirty();
                    }
                    ImGui::Spacing();
                    if (ImGui::Button("Move Up") && fi > 0) {
                        std::swap(layer.filters[fi], layer.filters[fi - 1]);
                        state.layerEffectsFocusIdx = fi - 1;
                        layer.filtersDirty = true;
                        canvas.MarkCompositeDirty();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Move Down") && fi + 1 < (int)layer.filters.size()) {
                        std::swap(layer.filters[fi], layer.filters[fi + 1]);
                        state.layerEffectsFocusIdx = fi + 1;
                        layer.filtersDirty = true;
                        canvas.MarkCompositeDirty();
                    }
                }
                ImGui::EndChild();
            }
            Ui::EndDockPanel();
        }

        if (state.showChannels) {
            Ui::BeginDockPanel("Channels", &state.showChannels);

            bool r = canvas.GetChannelR();
            bool g = canvas.GetChannelG();
            bool b = canvas.GetChannelB();
            bool a = canvas.GetChannelA();

            ImVec2 avail = ImGui::GetContentRegionAvail();
            bool listMode = (avail.y >= avail.x);
            auto& tok = Ui::Tokens();

            struct Ch { const char* name; bool* flag; Canvas::ChannelPreview preview; };
            Ch chans[] = {
                { "Red",   &r, Canvas::ChannelPreview::R },
                { "Green", &g, Canvas::ChannelPreview::G },
                { "Blue",  &b, Canvas::ChannelPreview::B },
                { "Alpha", &a, Canvas::ChannelPreview::A },
            };

            float thumb = listMode ? 40.f : std::clamp(std::min(avail.x / 4.5f, avail.y - 8.f), 32.f, 72.f);
            // Channel color tints (grayscale preview × tint → red/green/blue-black, A stays B&W)
            const ImVec4 chTint[] = {
                ImVec4(1.f, 0.15f, 0.15f, 1.f),
                ImVec4(0.15f, 1.f, 0.15f, 1.f),
                ImVec4(0.25f, 0.45f, 1.f, 1.f),
                ImVec4(1.f, 1.f, 1.f, 1.f),
            };

            for (int i = 0; i < 4; ++i) {
                ImGui::PushID(i);
                if (!listMode && i > 0) ImGui::SameLine(0, 8);

                ImGui::BeginGroup();
                ImVec2 t0 = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton("##ch", ImVec2(thumb, thumb));
                bool hovered = ImGui::IsItemHovered();
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) *chans[i].flag = !*chans[i].flag;

                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 t1(t0.x + thumb, t0.y + thumb);
                const bool on = *chans[i].flag;
                const float pad = 2.f;

                dl->AddRectFilled(t0, t1, tok.ColU32(ImVec4(0.04f, 0.04f, 0.05f, 1.f)), tok.rSm);
                if (on || hovered)
                    dl->AddRect(t0, t1, tok.ColU32(on ? tok.strokeActive : tok.strokeHairline), tok.rSm, 0, on ? 1.75f : 1.0f);

                // Prefer core channel grayscale proxy; fallback to composite × tint
                ID3D11ShaderResourceView* prev = canvas.GetChannelPreviewSRV(device, chans[i].preview);
                ID3D11ShaderResourceView* comp = canvas.GetCompositeSRV();
                ImVec4 tint = chTint[i];
                if (!on) tint.w = 0.45f;

                if (prev) {
                    ImU32 col = ImGui::ColorConvertFloat4ToU32(tint);
                    dl->AddImage((ImTextureID)prev,
                        ImVec2(t0.x + pad, t0.y + pad), ImVec2(t1.x - pad, t1.y - pad),
                        ImVec2(0, 0), ImVec2(1, 1), col);
                } else if (comp) {
                    // Fallback: full composite with channel tint (legacy look)
                    ImGui::SetCursorScreenPos(ImVec2(t0.x + pad, t0.y + pad));
                    ImGui::ImageWithBg((ImTextureID)comp, ImVec2(thumb - pad * 2, thumb - pad * 2),
                        ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 1), tint);
                }

                if (!on) {
                    ImU32 xcol = IM_COL32(255, 255, 255, (int)(0.25f * 255));
                    float m = thumb * 0.22f;
                    dl->AddLine(ImVec2(t0.x + m, t0.y + m), ImVec2(t1.x - m, t1.y - m), xcol, 2.0f);
                    dl->AddLine(ImVec2(t1.x - m, t0.y + m), ImVec2(t0.x + m, t1.y - m), xcol, 2.0f);
                }

                if (listMode) {
                    ImGui::SameLine(0, 10);
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (thumb - ImGui::GetTextLineHeight()) * 0.5f);
                    ImGui::TextUnformatted(chans[i].name);
                } else if (hovered) {
                    ImGui::SetTooltip("%s — click to toggle", chans[i].name);
                }
                ImGui::EndGroup();
                ImGui::PopID();
            }

            canvas.SetChannelR(r);
            canvas.SetChannelG(g);
            canvas.SetChannelB(b);
            canvas.SetChannelA(a);

            Ui::EndDockPanel();
        }

        // 8. Draw Standalone Tool Settings Panel (horizontal-first, icon toggles)
        if (state.showToolSettings) {
            Ui::BeginDockPanel("Tool Settings", &state.showToolSettings);

            ImVec2 tsAvail = ImGui::GetContentRegionAvail();
            bool tsHorizontal = (tsAvail.x >= tsAvail.y * 0.85f);

            auto MiniSlider = [&](const char* id, float* v, float mn, float mx, const char* tip, float width = 110.f) {
                ImGui::SetNextItemWidth(width);
                ImGui::SliderFloat(id, v, mn, mx, "%.2f");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            };

            bool isBrushLike = (activeTool == ActiveTool::Brush || activeTool == ActiveTool::Eraser);

            if (isBrushLike) {
                // Brush tips: ids persisted on Canvas (.rayp brush_tip_id / custom pixels)
                static BrushTip s_CustomTip;
                static bool s_CustomLoaded = false;
                static std::string s_LastSyncedTipId;

                auto ApplyTipId = [&](const std::string& id) {
                    if (id == "hard_round") { brush.tip = &BrushPresets::HardRound(); state.brushTipPreset = 1; }
                    else if (id == "pencil") { brush.tip = &BrushPresets::Pencil(); state.brushTipPreset = 2; }
                    else if (id == "airbrush") { brush.tip = &BrushPresets::Airbrush(); state.brushTipPreset = 3; }
                    else if (id == "custom") {
                        int sz = 0; std::vector<uint8_t> px;
                        if (canvas.GetCustomBrushTip(sz, px) && sz > 0) {
                            s_CustomTip.size = sz;
                            s_CustomTip.pixels = std::move(px);
                            s_CustomTip.name = "Custom";
                            s_CustomTip.spacingMul = 1.0f;
                            s_CustomLoaded = true;
                            state.hasCustomBrushTip = true;
                            brush.tip = &s_CustomTip;
                        } else {
                            brush.tip = s_CustomLoaded ? &s_CustomTip : nullptr;
                        }
                        state.brushTipPreset = 4;
                    }
                    else if (id == "procedural") { brush.tip = nullptr; state.brushTipPreset = 0; }
                    else { // soft_round default
                        brush.tip = &BrushPresets::SoftRound();
                        state.brushTipPreset = 0;
                    }
                };

                // Pull tip from project after load / when canvas id changes
                if (canvas.GetBrushTipId() != s_LastSyncedTipId) {
                    s_LastSyncedTipId = canvas.GetBrushTipId();
                    ApplyTipId(s_LastSyncedTipId.empty() ? "soft_round" : s_LastSyncedTipId);
                }

                const char* tipNames[] = { "Soft", "Hard", "Pencil", "Air", "Custom" };
                const char* tipIds[] = { "soft_round", "hard_round", "pencil", "airbrush", "custom" };
                int tipIdx = state.brushTipPreset;
                ImGui::SetNextItemWidth(72.f);
                if (UiCombo("##tip", &tipIdx, tipNames, IM_ARRAYSIZE(tipNames))) {
                    state.brushTipPreset = tipIdx;
                    if (tipIdx >= 0 && tipIdx < 5) {
                        canvas.SetBrushTipId(tipIds[tipIdx]);
                        s_LastSyncedTipId = tipIds[tipIdx];
                        ApplyTipId(tipIds[tipIdx]);
                    }
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Brush tip (saved in project)");
                ImGui::SameLine();
                if (ImGui::SmallButton("Load Tip...")) {
                    char path[512] = "";
                    if (ShowOpenFileWin32(path, sizeof(path), "Images (*.png;*.jpg;*.bmp;*.tga)\0*.png;*.jpg;*.bmp;*.tga\0All\0*.*\0")) {
                        std::vector<uint8_t> px; int tw=0, th=0;
                        if (ImageManager::LoadImageFromFile(path, px, tw, th) && tw > 0 && th > 0) {
                            int side = std::min(std::min(tw, th), 128);
                            s_CustomTip.size = side;
                            s_CustomTip.pixels.assign((size_t)side * side, 0);
                            s_CustomTip.name = "Custom";
                            s_CustomTip.spacingMul = 1.0f;
                            for (int y = 0; y < side; ++y) {
                                for (int x = 0; x < side; ++x) {
                                    int sx = x * tw / side;
                                    int sy = y * th / side;
                                    size_t si = ((size_t)sy * tw + sx) * 4;
                                    uint8_t r8 = px[si], g8 = px[si+1], b8 = px[si+2], a8 = px[si+3];
                                    float lum = (0.2126f*r8 + 0.7152f*g8 + 0.0722f*b8) * (a8 / 255.f);
                                    s_CustomTip.pixels[(size_t)y * side + x] = (uint8_t)std::clamp(lum, 0.f, 255.f);
                                }
                            }
                            s_CustomLoaded = true;
                            state.hasCustomBrushTip = true;
                            state.customBrushTipName = path;
                            state.brushTipPreset = 4;
                            brush.tip = &s_CustomTip;
                            canvas.SetCustomBrushTip(side, s_CustomTip.pixels); // also sets id=custom
                            s_LastSyncedTipId = "custom";
                        }
                    }
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Load grayscale stamp (persisted in .rayp)");
                ImGui::SameLine();

                MiniSlider("##rad", &brush.radius, 1.f, 250.f, "Radius (px)", 100.f);
                ImGui::SameLine();
                Ui::IconToggle("##pr", "ts_pressure_radius", &brush.pressureRadius, ImVec2(28, 28),
                    "Pressure → Radius (on)", "Pressure → Radius (off)");
                ImGui::SameLine();
                MiniSlider("##hrd", &brush.hardness, 0.f, 1.f, "Hardness", 80.f);
                ImGui::SameLine();
                Ui::IconToggle("##ph", "ts_pressure_hardness", &brush.pressureHardness, ImVec2(28, 28),
                    "Pressure → Hardness (on)", "Pressure → Hardness (off)");
                ImGui::SameLine();
                // Visual opacity (checker + color) — Stage 1 pilot of VisualSlider
                {
                    float op = brush.opacity;
                    if (Ui::VisualSlider("##opcvis", &op, ImVec2(88, 22), Ui::VisualSliderSkin::OpacityChecker, brush.color, "Opacity"))
                        brush.opacity = op;
                }
                ImGui::SameLine();
                Ui::IconToggle("##po", "ts_pressure_opacity", &brush.pressureOpacity, ImVec2(28, 28),
                    "Pressure → Opacity (on)", "Pressure → Opacity (off)");
                ImGui::SameLine();
                MiniSlider("##spc", &brush.spacing, 0.01f, 5.f, "Spacing", 70.f);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(60.f);
                ImGui::SliderInt("##stb", &brush.stabilization, 1, 50);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stabilization");

                if (activeTool == ActiveTool::Brush) {
                    ImGui::SameLine();
                    bool mirrorH = canvas.GetMirrorHorizontal();
                    bool mirrorV = canvas.GetMirrorVertical();
                    if (Ui::IconToggle("##mh", "ts_mirror_h", &mirrorH, ImVec2(28, 28), "Mirror H on", "Mirror H off"))
                        canvas.SetMirrorHorizontal(mirrorH);
                    ImGui::SameLine();
                    if (Ui::IconToggle("##mv", "ts_mirror_v", &mirrorV, ImVec2(28, 28), "Mirror V on", "Mirror V off"))
                        canvas.SetMirrorVertical(mirrorV);
                }
            }
            else if (activeTool == ActiveTool::MagicWand || activeTool == ActiveTool::SmartSelect || activeTool == ActiveTool::QuickSelect) {
                if (activeTool == ActiveTool::MagicWand) {
                    bool changed = false;
                    MiniSlider("##tol", &state.magicWandTolerance, 0.f, 1.f, "Tolerance", 140.f);
                    if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
                    // also live while dragging:
                    if (ImGui::IsItemActive()) changed = true;
                    ImGui::SameLine();
                    if (ImGui::Checkbox("##cont", &state.magicWandContiguous)) changed = true;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Contiguous");
                    if (changed && canvas.HasWandSeed()) {
                        bool add = ImGui::GetIO().KeyShift;
                        bool subtract = ImGui::GetIO().KeyAlt;
                        canvas.PreviewWandFromSeed(device, state.magicWandTolerance, add, subtract, state.magicWandContiguous);
                    }
                } else if (activeTool == ActiveTool::QuickSelect) {
                    MiniSlider("##qsr", &brush.radius, 1.f, 200.f, "Quick Select brush size", 140.f);
                } else {
                    ImGui::TextDisabled("Smart Select: draw contour");
                }
            }
            else if (activeTool == ActiveTool::BucketFill) {
                MiniSlider("##bft", &state.bucketFillTolerance, 0.f, 1.f, "Fill Tolerance", 140.f);
            }
            else if (IsSelectTool(activeTool) || IsLassoTool(activeTool)) {
                if (activeTool == ActiveTool::PolygonalLasso)
                    ImGui::TextDisabled("Click vertices · Enter/Dbl close · Esc cancel");
                else
                    ImGui::TextDisabled("Shift: add  ·  Alt: subtract");
            }
            else if (activeTool == ActiveTool::Gradient) {
                ImGui::TextDisabled("Drag: Primary → Secondary");
            }
            else if (activeTool == ActiveTool::Pipette) {
                ImGui::TextDisabled("Click canvas to sample");
            }
            else if (activeTool == ActiveTool::Smudge) {
                // No color controls — smudge only radius / strength / spacing
                MiniSlider("##smr", &state.smudge.radius, 1.f, 150.f, "Smudge Radius", 110.f);
                ImGui::SameLine();
                MiniSlider("##sms", &state.smudge.strength, 0.f, 1.f, "Strength", 100.f);
                ImGui::SameLine();
                MiniSlider("##smp", &state.smudge.spacing, 0.01f, 1.f, "Spacing", 90.f);
            }
            else if (activeTool == ActiveTool::MovePixels) {
                float sx = canvas.GetFloatingScaleX();
                float sy = canvas.GetFloatingScaleY();
                float rotDeg = canvas.GetFloatingRotation() * (180.0f / 3.14159265f);
                ImGui::SetNextItemWidth(80.f);
                if (ImGui::SliderFloat("##sx", &sx, 0.05f, 5.f, "X:%.2f")) canvas.SetFloatingScaleX(sx);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.f);
                if (ImGui::SliderFloat("##sy", &sy, 0.05f, 5.f, "Y:%.2f")) canvas.SetFloatingScaleY(sy);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(90.f);
                if (ImGui::SliderFloat("##rot", &rotDeg, -180.f, 180.f, "%.0f°"))
                    canvas.SetFloatingRotation(rotDeg * (3.14159265f / 180.0f));
                ImGui::SameLine();
                if (ImGui::Button("⇄")) canvas.SetFloatingScaleX(-canvas.GetFloatingScaleX());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Flip H");
                ImGui::SameLine();
                if (ImGui::Button("⇅")) canvas.SetFloatingScaleY(-canvas.GetFloatingScaleY());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Flip V");
                ImGui::SameLine();
                if (ImGui::Button("Reset")) {
                    canvas.SetFloatingScaleX(1.f); canvas.SetFloatingScaleY(1.f); canvas.SetFloatingRotation(0.f);
                }
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.55f, 0.18f, 1.0f));
                if (ImGui::Button("OK")) state.commitTransform = true;
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.18f, 1.0f));
                if (ImGui::Button("✕")) state.cancelTransform = true;
                ImGui::PopStyleColor();
            }
            else {
                ImGui::TextDisabled("Hand: pan · RMB/Shift: rotate");
            }

            Ui::EndDockPanel();
        }

        // 9. Draw Logging Console Panel
        if (state.showConsole) {
            Ui::BeginDockPanel("Console Logs", &state.showConsole);
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
            Ui::EndDockPanel();
        }

        // 10. Colors — adaptive SV rect + visual Hue / Alpha (checker)
        if (state.showColors) {
            Ui::BeginDockPanel("Colors", &state.showColors);

            ImVec2 avail = ImGui::GetContentRegionAvail();
            float h, s, v;
            ImGui::ColorConvertRGBtoHSV(brush.color[0], brush.color[1], brush.color[2], h, s, v);

            // Fill available width; height proportional but may be rectangle
            float stripH = 22.f;
            float pad = 8.f;
            float svW = std::max(80.f, avail.x - 4.f);
            float svH = std::clamp(avail.y - stripH * 2.f - 80.f, 100.f, std::max(120.f, avail.y * 0.55f));
            // Prefer using width: allow wide rectangles
            if (avail.x > avail.y)
                svH = std::clamp(svW * 0.65f, 100.f, avail.y - stripH * 2.f - 70.f);

            ImVec2 sqPos = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##svsq", ImVec2(svW, svH));
            bool svActive = ImGui::IsItemActive();
            bool svHovered = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            auto& tok = Ui::Tokens();

            const int steps = 28;
            for (int yi = 0; yi < steps; ++yi) {
                for (int xi = 0; xi < steps; ++xi) {
                    float ss = (xi + 0.5f) / steps;
                    float vv = 1.f - (yi + 0.5f) / steps;
                    float rr, gg, bb;
                    ImGui::ColorConvertHSVtoRGB(h, ss, vv, rr, gg, bb);
                    ImU32 col = IM_COL32((int)(rr*255),(int)(gg*255),(int)(bb*255),255);
                    float x0 = sqPos.x + xi * (svW / steps);
                    float y0 = sqPos.y + yi * (svH / steps);
                    float x1 = sqPos.x + (xi + 1) * (svW / steps);
                    float y1 = sqPos.y + (yi + 1) * (svH / steps);
                    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col);
                }
            }
            dl->AddRect(sqPos, ImVec2(sqPos.x + svW, sqPos.y + svH), tok.ColU32(tok.strokeHairline), tok.rSm);
            ImVec2 cur(sqPos.x + s * svW, sqPos.y + (1.f - v) * svH);
            dl->AddCircle(cur, 6.f, IM_COL32(0,0,0,200), 16, 2.f);
            dl->AddCircle(cur, 6.f, IM_COL32(255,255,255,230), 16, 1.5f);

            if (svActive || (svHovered && ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
                ImVec2 mp = ImGui::GetIO().MousePos;
                s = std::clamp((mp.x - sqPos.x) / svW, 0.f, 1.f);
                v = std::clamp(1.f - (mp.y - sqPos.y) / svH, 0.f, 1.f);
                ImGui::ColorConvertHSVtoRGB(h, s, v, brush.color[0], brush.color[1], brush.color[2]);
            }

            ImGui::Spacing();
            // Visual Hue strip
            if (Ui::VisualSlider("##huevis", &h, ImVec2(svW, stripH), Ui::VisualSliderSkin::HueStrip, nullptr, "Hue")) {
                ImGui::ColorConvertHSVtoRGB(h, s, v, brush.color[0], brush.color[1], brush.color[2]);
            }
            ImGui::Spacing();
            // Visual Alpha on checkerboard
            if (Ui::VisualSlider("##alphavis", &brush.color[3], ImVec2(svW, stripH),
                    Ui::VisualSliderSkin::OpacityChecker, brush.color, "Opacity / Alpha")) {
                // value written in-place
            }

            ImGui::Spacing();
            ImGui::ColorButton("##pri", ImVec4(brush.color[0], brush.color[1], brush.color[2], brush.color[3]),
                ImGuiColorEditFlags_AlphaPreview, ImVec2(36, 36));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Primary color");

            ImGui::Spacing();
            // Compact palette
            static const ImVec4 paletteColors[] = {
                ImVec4(0,0,0,1), ImVec4(1,1,1,1), ImVec4(0.5f,0.5f,0.5f,1), ImVec4(0.75f,0.75f,0.75f,1),
                ImVec4(1,0,0,1), ImVec4(1,1,0,1), ImVec4(0,1,0,1), ImVec4(0,1,1,1),
                ImVec4(0,0,1,1), ImVec4(1,0,1,1), ImVec4(0.5f,0,0,1), ImVec4(0.5f,0.5f,0,1),
                ImVec4(0,0.5f,0,1), ImVec4(0,0.5f,0.5f,1), ImVec4(0,0,0.5f,1), ImVec4(0.5f,0,0.5f,1)
            };
            float cell = 18.f;
            int perRow = std::max(4, (int)(ImGui::GetContentRegionAvail().x / (cell + 4.f)));
            for (int i = 0; i < IM_ARRAYSIZE(paletteColors); ++i) {
                ImGui::PushID(i);
                if (i > 0 && (i % perRow) != 0) ImGui::SameLine(0, 3);
                if (ImGui::ColorButton("##pal", paletteColors[i], ImGuiColorEditFlags_NoTooltip, ImVec2(cell, cell))) {
                    brush.color[0] = paletteColors[i].x;
                    brush.color[1] = paletteColors[i].y;
                    brush.color[2] = paletteColors[i].z;
                    brush.color[3] = paletteColors[i].w;
                }
                ImGui::PopID();
            }

            Ui::EndDockPanel();
        }

        // Draw loading progress modal
        if (g_LoadingState.isLoading) {
            ImGui::OpenPopup("Loading Document...");
            ImGuiViewport* mainViewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(mainViewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            
            if (ImGui::BeginPopupModal("Loading Document...", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
                ImGui::Text("Loading: %s", g_LoadingState.filepath.substr(g_LoadingState.filepath.find_last_of("\\/") + 1).c_str());
                
                float progress = g_LoadingState.progress;
                std::string stage;
                {
                    std::lock_guard<std::mutex> lock(g_LoadingState.mutex);
                    stage = g_LoadingState.stage;
                }
                
                ImGui::ProgressBar(progress, ImVec2(300, 20), stage.c_str());
                ImGui::EndPopup();
            }
        }
    }
}
