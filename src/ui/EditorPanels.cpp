#include "EditorPanels.h"
#include "FileExplorer.h"
#include "../core/ops/AppContext.h"
#include "../core/ops/ActionCatalog.h"
#include "../core/ops/OperatorRegistry.h"
#include "panels/LayersPanel.h"
#include "panels/AssetBrowserPanel.h"
#include "panels/ChannelsPanel.h"
#include "panels/ToolSettingsPanel.h"
#include "panels/LayerEffectsPanel.h"
#include "panels/ProjectSetupPanel.h"
#include "panels/ColorsPanel.h"
#include "widgets/UiAssetPicker.h"
#include "../core/ConfigManager.h"
#include "../core/Logger.h"
#include "../core/JobManager.h"
#include "../core/Notifications.h"
#include "../core/AutoSaveManager.h"
#include "../core/KeymapManager.h"
#include "../assets/AssetManager.h"
#include "../core/ImageManager.h"
#include "../core/ImageManager.h"
#include "../scripting/ScriptingEngine.h"
#include "../scripting/ScriptPluginHost.h"
#include "../scripting/ScriptDockRegistry.h"
#include "../scripting/ScriptMainThread.h"
#include "../vector/VectorToolSession.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif
#include "../core/ThreadPool.h"
#include "style/UiTokens.h"
#include "style/UiMotion.h"
#include "icons/SvgIconCache.h"
#include "widgets/UiIconButton.h"
#include "widgets/UiIconToggle.h"
#include "widgets/UiDropdown.h"
#include "widgets/UiVisualSlider.h"
#include "widgets/UiPanel.h"
#include "widgets/UiPathField.h"
#include "widgets/UiTooltip.h"
#include "../core/BrushLibrary.h"
#include "../core/ProjectManager.h"
#include "../shell/DdsThumbRegister.h"
#include "shell/HelperShellRegister.h"
#include "../preview3d/PreviewRenderer.h"
#include "../preview3d/MaterialConfig.h"
#include <stb_image.h>
#include <thread>
#include <imgui_internal.h>
#include <filesystem>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <ctime>

extern void ApplyTheme(const std::string& themeName);
extern bool g_IsLayersHovered;
extern bool g_IsViewportHovered;
extern float g_SecondaryColor[4];
extern float g_ColorSwapAnim;
extern bool g_ColorSwapPending;
extern std::vector<float> Canvas_BuildSplineLUT(const std::vector<std::pair<float,float>>& pts);

#include "dialogs/Win32FileDialogs.h"
#include "../core/PathUtil.h"

// Thin wrappers so PathField / call sites keep the old names during migration
static bool ShowOpenFileWin32(char* outPath, size_t maxLen, const char* filter = "All Files (*.*)\0*.*\0") {
    return Ui::ShowOpenFile(outPath, maxLen, filter);
}
static bool ShowSaveFileWin32(char* outPath, size_t maxLen, const char* filter = "All Files (*.*)\0*.*\0") {
    return Ui::ShowSaveFile(outPath, maxLen, filter);
}

namespace UI {

    VectorToolStyle g_VectorToolStyle{};
    DocumentLoadingState g_LoadingState;
    ProjectTabCloseRequest g_ProjectTabCloseRequest;

    // Fill Layer popup pipette (arm → click canvas once)
    static bool s_FillPipetteArmed = false;
    static int s_FillPipetteLayer = -1;
    static int s_FillPipetteMap = -1;

    void TriggerBackgroundOpenDocument(const std::string& filepath, ID3D11Device* device, Canvas& canvas) {
        if (g_LoadingState.isLoading) return;

        // Always store/load UTF-8 paths (dialogs/drops may arrive as ACP).
        const std::string pathUtf8 = PathUtil::NormalizeToUtf8Path(filepath);
        
        g_LoadingState.isLoading = true;
        g_LoadingState.progress = 0.0f;
        g_LoadingState.filepath = pathUtf8;
        g_LoadingState.completed = false;
        g_LoadingState.success = false;
        {
            std::lock_guard<std::mutex> lock(g_LoadingState.mutex);
            g_LoadingState.stage = "Initializing";
        }

        // Document-locking job: UI chrome stays free, edits blocked via AppContext.
        const uint64_t jobId = core::JobManager::Get().Begin(
            "Open document", /*locksDocument=*/true, /*cancellable=*/false);
        core::JobManager::Get().SetProgress(jobId, 0.f, pathUtf8);

        std::thread([pathUtf8, device, &canvas, jobId]() {
            Logger::Get().Info("Starting background load of: " + pathUtf8);
            bool ok = canvas.OpenDocument(device, pathUtf8, [jobId](float progress, const char* stage) {
                g_LoadingState.progress = progress;
                if (stage) {
                    std::lock_guard<std::mutex> lock(g_LoadingState.mutex);
                    g_LoadingState.stage = stage;
                }
                core::JobManager::Get().SetProgress(jobId, progress, stage ? stage : "");
            });
            g_LoadingState.success = ok;
            g_LoadingState.completed = true;
            core::JobManager::Get().Complete(jobId, ok,
                ok ? ("Opened " + pathUtf8) : ("Open failed: " + pathUtf8));
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
        // Always sample the active Channels view composite (map-filtered stack).
        // Matches what the viewport shows — not the unfiltered all-maps blend.
        canvas.SampleCompositePixel(cx, cy, outColor);
    }

    bool IsFillPipetteArmed() { return s_FillPipetteArmed; }

    void ArmFillPipette(int layerIdx, int mapIdx) {
        s_FillPipetteArmed = true;
        s_FillPipetteLayer = layerIdx;
        s_FillPipetteMap = mapIdx;
    }

    bool FillPipetteArmedFor(int layerIdx, int mapIdx) {
        if (!s_FillPipetteArmed || s_FillPipetteLayer != layerIdx) return false;
        if (mapIdx < 0) return true;
        return s_FillPipetteMap == mapIdx;
    }

    bool TryApplyFillPipette(Canvas& canvas, float canvasX, float canvasY) {
        if (!s_FillPipetteArmed) return false;
        auto& layers = canvas.GetLayers();
        if (s_FillPipetteLayer < 0 || s_FillPipetteLayer >= (int)layers.size()) {
            s_FillPipetteArmed = false;
            return false;
        }
        Layer& L = layers[s_FillPipetteLayer];
        if (!L.IsFill()) {
            s_FillPipetteArmed = false;
            return false;
        }
        int mi = s_FillPipetteMap;
        if (mi < 0 || mi >= (int)texset::MapKind::Count) {
            s_FillPipetteArmed = false;
            return false;
        }
        float c[4] = {0, 0, 0, 1};
        SampleCanvasColor(canvas, canvasX, canvasY, c);
        auto& mc = L.fill.mapColor[mi];
        mc.enabled = true;
        mc.rgba[0] = c[0]; mc.rgba[1] = c[1]; mc.rgba[2] = c[2]; mc.rgba[3] = c[3];
        L.needsUpload = true;
        L.SyncWorkSpaceFromFillTarget(nullptr);
        canvas.MarkCompositeDirty();
        canvas.SetDocumentModified(true);
        s_FillPipetteArmed = false;
        s_FillPipetteLayer = -1;
        s_FillPipetteMap = -1;
        return true;
    }

    // Stroke dab preview (rotation/spacing-aware visual; scatter/dynamics drawn as ghost only)
    static void DrawBrushStrokePreview(ImDrawList* dl, ImVec2 rmin, ImVec2 rmax,
                                       const BrushSettings& b, ImU32 baseCol) {
        if (!dl) return;
        float boxW = rmax.x - rmin.x;
        float boxH = rmax.y - rmin.y;
        if (boxW < 4.f || boxH < 4.f) return;
        float rPx = std::clamp(b.radius * 0.32f, 2.5f, boxH * 0.40f);
        float y = rmin.y + boxH * 0.5f;
        float x0 = rmin.x + rPx + 2.f;
        float x1 = rmax.x - rPx - 2.f;
        if (x1 <= x0) x1 = x0 + 1.f;
        float stepPx = std::max(2.f, rPx * 2.f * std::max(0.05f, b.spacing));
        int steps = std::max(3, (int)((x1 - x0) / stepPx));
        float rot = b.rotationDeg * (3.14159265f / 180.f);
        int aBase = (int)((baseCol >> 24) & 0xFF);
        for (int s = 0; s <= steps; ++s) {
            float t = (float)s / (float)steps;
            float x = x0 + (x1 - x0) * t;
            float yy = y + std::sin(t * 3.14159265f) * (boxH * 0.07f);
            // scatter placeholder: slight visual noise (not applied in engine yet)
            if (b.scatter > 0.001f) {
                x += std::sin(t * 17.f) * rPx * b.scatter * 0.35f;
                yy += std::cos(t * 13.f) * rPx * b.scatter * 0.35f;
            }
            float aMul = b.opacity;
            int aOuter = (int)(aBase * aMul * 0.22f);
            int aInner = (int)(aBase * aMul * 0.88f);
            ImU32 cOuter = (baseCol & 0x00FFFFFF) | ((aOuter & 0xFF) << 24);
            ImU32 cInner = (baseCol & 0x00FFFFFF) | ((aInner & 0xFF) << 24);
            // Ellipse rotated for rotation preview
            float rx = rPx, ry = rPx * (0.55f + 0.45f * b.hardness);
            if (std::fabs(rot) < 0.01f) {
                dl->AddCircleFilled(ImVec2(x, yy), rx, cOuter, 18);
                dl->AddCircleFilled(ImVec2(x, yy), rx * (0.35f + 0.55f * b.hardness), cInner, 14);
            } else {
                // approximate rotated ellipse with a few points
                const int N = 16;
                ImVec2 pts[N];
                for (int i = 0; i < N; ++i) {
                    float a = (float)i / N * 6.2831853f;
                    float lx = std::cos(a) * rx;
                    float ly = std::sin(a) * ry;
                    float c = std::cos(rot), s2 = std::sin(rot);
                    pts[i] = ImVec2(x + lx * c - ly * s2, yy + lx * s2 + ly * c);
                }
                dl->AddConvexPolyFilled(pts, N, cOuter);
            }
        }
    }

    static void DrawTipTextureThumb(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const BrushTip* tip) {
        dl->AddRectFilled(p0, p1, IM_COL32(24, 24, 26, 255), 4.f);
        dl->AddRect(p0, p1, IM_COL32(80, 80, 90, 255), 4.f);
        if (!tip || tip->size <= 0 || tip->pixels.empty()) {
            // procedural soft circle
            ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
            float r = std::min(p1.x - p0.x, p1.y - p0.y) * 0.35f;
            dl->AddCircleFilled(c, r, IM_COL32(200, 200, 210, 180), 24);
            return;
        }
        // Downsample tip into a small grid
        const int g = 24;
        float cw = (p1.x - p0.x) / g, ch = (p1.y - p0.y) / g;
        int ts = tip->size;
        for (int y = 0; y < g; ++y) {
            for (int x = 0; x < g; ++x) {
                int sx = x * ts / g, sy = y * ts / g;
                uint8_t v = tip->pixels[(size_t)sy * ts + sx];
                if (v < 8) continue;
                dl->AddRectFilled(
                    ImVec2(p0.x + x * cw, p0.y + y * ch),
                    ImVec2(p0.x + (x + 1) * cw, p0.y + (y + 1) * ch),
                    IM_COL32(v, v, v, 255));
            }
        }
    }

    void DrawBrushPickerPopup(bool& openFlag, ImVec2 popupPos, BrushSettings& brush) {
        auto& lib = BrushLibrary::Get();
        auto& T = Ui::Tokens();
        static bool s_popupOpen = false;

        if (openFlag) {
            ImGui::OpenPopup("##BrushPicker");
            openFlag = false;
            s_popupOpen = true;
        }

        ImGui::SetNextWindowPos(popupPos, ImGuiCond_Appearing);
        // Wide enough for long stroke previews + inspector
        ImGui::SetNextWindowSize(ImVec2(780, 480), ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints(ImVec2(640, 400), ImVec2(1400, 900));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, T.rMd);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 12));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(T.bgElevated.x, T.bgElevated.y, T.bgElevated.z, 0.97f));

        if (ImGui::BeginPopup("##BrushPicker")) {
            s_popupOpen = true;
            ImGui::Text("Brush presets");
            ImGui::SameLine();
            ImGui::TextDisabled("RMB · Save stores size/opacity/tablet/tip · drag edges to resize");
            ImGui::Separator();

            // --- Left: list (wider for stroke dab visibility) ---
            float listW = std::clamp(ImGui::GetContentRegionAvail().x * 0.42f, 300.f, 420.f);
            ImGui::BeginChild("##brushlist", ImVec2(listW, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
            if (ImGui::Button("Create brush", ImVec2(-1, 0))) {
                std::string id = lib.CreateFromCurrent(brush, "New Brush");
                if (!id.empty()) {
                    lib.SetActiveId(id);
                    const bool er = brush.erase;
                    lib.ApplyTo(id, brush);
                    brush.erase = er;
                }
            }
            ImGui::Spacing();

            const std::string active = lib.GetActiveId();
            const float rowH = 64.f; // taller rows = longer visible stroke
            for (const auto& m : lib.List()) {
                ImGui::PushID(m.id.c_str());
                bool sel = (m.id == active);
                BrushPresetParams params;
                lib.Get(m.id, params);
                BrushSettings prev;
                lib.ApplyTo(m.id, prev);

                ImVec4 chip = m.isBuiltin
                    ? ImVec4(0.25f, 0.55f, 0.95f, 1.f)
                    : (m.isDirty ? ImVec4(1.0f, 0.55f, 0.15f, 1.f) : ImVec4(0.95f, 0.50f, 0.12f, 1.f));

                ImVec2 row0 = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##row", sel, 0, ImVec2(0, rowH))) {
                    const bool er = brush.erase;
                    lib.ApplyTo(m.id, brush);
                    brush.erase = er;
                    lib.SetActiveId(m.id);
                }
                ImVec2 row1 = ImGui::GetItemRectMax();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(ImVec2(row0.x, row0.y + 4), ImVec2(row0.x + 5, row1.y - 4),
                    ImGui::ColorConvertFloat4ToU32(chip), 2.f);
                char label[160];
                std::snprintf(label, sizeof(label), "%s%s", m.displayName.c_str(), m.isDirty ? " *" : "");
                dl->AddText(ImVec2(row0.x + 12, row0.y + 4), T.ColU32(T.textPrimary), label);
                // Meta line
                char metaLine[96];
                std::snprintf(metaLine, sizeof(metaLine), "r=%.0f  h=%.0f%%  op=%.0f%%  sp=%.2f",
                    params.radius, params.hardness * 100.f, params.opacity * 100.f, params.spacing);
                dl->AddText(ImVec2(row0.x + 12, row0.y + 20), T.ColU32(T.textSecondary), metaLine);
                ImVec2 pmin(row0.x + 12, row0.y + 36);
                ImVec2 pmax(row1.x - 8, row1.y - 4);
                dl->AddRectFilled(pmin, pmax, IM_COL32(18, 18, 20, 255), 3.f);
                DrawBrushStrokePreview(dl, pmin, pmax, prev, IM_COL32(230, 230, 235, 255));
                ImGui::PopID();
            }
            ImGui::EndChild();

            ImGui::SameLine(0, 12);

            // --- Right: inspector / params ---
            ImGui::BeginChild("##brushside", ImVec2(0, 0), true);
            {
                BrushPresetMeta meta;
                bool has = lib.GetMeta(lib.GetActiveId(), meta);
                ImGui::TextUnformatted(has ? meta.displayName.c_str() : "(no preset)");
                if (has) {
                    ImGui::SameLine();
                    if (meta.isBuiltin) ImGui::TextColored(ImVec4(0.4f, 0.65f, 1.f, 1.f), "builtin");
                    else if (meta.isDirty) ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f), "unsaved");
                    else ImGui::TextColored(ImVec4(1.f, 0.55f, 0.15f, 1.f), "custom");
                }

                // Large stroke preview
                ImVec2 prevPos = ImGui::GetCursorScreenPos();
                float prevW = ImGui::GetContentRegionAvail().x;
                float prevH = 96.f;
                ImGui::Dummy(ImVec2(prevW, prevH));
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 p0 = prevPos, p1(prevPos.x + prevW, prevPos.y + prevH);
                dl->AddRectFilled(p0, p1, IM_COL32(16, 16, 18, 255), 6.f);
                DrawBrushStrokePreview(dl, ImVec2(p0.x + 10, p0.y + 10), ImVec2(p1.x - 10, p1.y - 10),
                    brush, IM_COL32(240, 240, 245, 255));

                ImGui::Spacing();
                ImGui::TextDisabled("Tip texture");
                ImVec2 t0 = ImGui::GetCursorScreenPos();
                float th = 80.f;
                ImGui::Dummy(ImVec2(th, th));
                DrawTipTextureThumb(dl, t0, ImVec2(t0.x + th, t0.y + th), brush.tip);
                ImGui::SameLine();
                ImGui::BeginGroup();
                if (brush.tip && brush.tip->size > 0)
                    ImGui::Text("%s  %dx%d", brush.tip->name ? brush.tip->name : "tip",
                        brush.tip->size, brush.tip->size);
                else
                    ImGui::TextDisabled("Procedural soft circle");
                if (!brush.tipSourcePath.empty())
                    ImGui::TextWrapped("%s", brush.tipSourcePath.c_str());
                else if (has && !meta.sourcePath.empty())
                    ImGui::TextWrapped("%s", meta.sourcePath.c_str());
                ImGui::EndGroup();

                ImGui::Separator();
                ImGui::Text("Parameters");
                float maxR = ConfigManager::Get().GetMaxBrushRadius();
                Ui::SmartSliderFloat("Size", &brush.radius, 1.f, maxR, 20.f, 1.f, "%.1f px");
                Ui::SmartSliderFloat("Hardness", &brush.hardness, 0.f, 1.f, 0.5f, 0.05f, "%.2f");
                Ui::SmartSliderFloat("Opacity", &brush.opacity, 0.f, 1.f, 1.f, 0.05f, "%.2f");
                Ui::SmartSliderFloat("Spacing", &brush.spacing, 0.01f, 2.f, 0.1f, 0.05f, "%.2f");
                Ui::SmartSliderInt("Stabilization", &brush.stabilization, 1, 50, 1, 1);

                ImGui::Separator();
                ImGui::Text("Tablet pressure");
                ImGui::Checkbox("→ Radius", &brush.pressureRadius); ImGui::SameLine();
                ImGui::Checkbox("→ Hardness", &brush.pressureHardness); ImGui::SameLine();
                ImGui::Checkbox("→ Opacity", &brush.pressureOpacity);

                ImGui::Separator();
                ImGui::Text("Rotation / dynamics");
                ImGui::TextDisabled("Applied in paint engine (tip rotation + dab scatter/jitter)");
                Ui::SmartSliderFloat("Rotation", &brush.rotationDeg, 0.f, 360.f, 0.f, 1.f, "%.0f°");
                ImGui::Checkbox("Pressure → Rotation", &brush.pressureRotation);
                Ui::SmartSliderFloat("Scatter", &brush.scatter, 0.f, 1.f, 0.f, 0.05f, "%.2f");
                Ui::SmartSliderFloat("Angle jitter", &brush.angleJitter, 0.f, 1.f, 0.f, 0.05f, "%.2f");

                ImGui::Separator();
                ImGui::BeginDisabled(!has || meta.isBuiltin);
                if (ImGui::Button("Save preset", ImVec2(-1, 0))) {
                    // Push live brush into staging then disk
                    if (meta.isDirty || !meta.isBuiltin) {
                        lib.UpdateStaging(lib.GetActiveId(), brush);
                        lib.SaveToDisk(lib.GetActiveId());
                    }
                }
                ImGui::EndDisabled();
                ImGui::BeginDisabled(!has || meta.isBuiltin);
                if (ImGui::Button("Delete", ImVec2(-1, 0))) {
                    lib.DeleteCustom(lib.GetActiveId());
                    lib.SetActiveId("builtin.soft_round");
                    const bool er = brush.erase;
                    lib.ApplyTo("builtin.soft_round", brush);
                    brush.erase = er;
                }
                ImGui::EndDisabled();
            }
            ImGui::EndChild();

            // Keep dirty staging in sync while popup open
            if (!lib.GetActiveId().empty()) {
                BrushPresetMeta meta;
                if (lib.GetMeta(lib.GetActiveId(), meta) && !meta.isBuiltin)
                    lib.UpdateStaging(lib.GetActiveId(), brush);
            }

            ImGui::EndPopup();
        } else {
            s_popupOpen = false;
        }

        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        (void)s_popupOpen;
    }

    // Themed combo — kit API (Ui::Combo). Local alias for call-site brevity.
    static bool UiCombo(const char* id, int* idx, const char* const* items, int count,
                        const char* label = nullptr) {
        return Ui::Combo(id, idx, items, count, label);
    }

    // ICC preset combo (presets only — no free-text path). Returns true if changed.
    static bool DrawIccPresetCombo(Canvas& canvas, const char* label = "ICC Profile") {
        static const Canvas::IccPreset kPresets[] = {
            Canvas::IccPreset::None,
            Canvas::IccPreset::sRGB,
            Canvas::IccPreset::DisplayP3,
            Canvas::IccPreset::AdobeRGB,
            Canvas::IccPreset::Linear
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
            Canvas::IccPresetName(Canvas::IccPreset::AdobeRGB),
            Canvas::IccPresetName(Canvas::IccPreset::Linear)
        };
        if (Ui::Combo("##icc_preset", &cur, names, IM_ARRAYSIZE(names), label)) {
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
        if (strcmp(actionName, "BlurTool") == 0) return "tool_smudge"; // reuse until dedicated icon
        if (strcmp(actionName, "StampTool") == 0) return "tool_brush"; // clone stamp uses brush base
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
        // Vector tools re-use closest existing icons
        if (strcmp(actionName, "VectorRectTool") == 0) return "tool_select_rect";
        if (strcmp(actionName, "VectorEllipseTool") == 0) return "tool_select_ellipse";
        if (strcmp(actionName, "VectorLineTool") == 0) return "tool_pan";
        if (strcmp(actionName, "VectorPenTool") == 0) return "tool_lasso_poly";
        if (strcmp(actionName, "VectorFreehandTool") == 0) return "tool_lasso";
        if (strcmp(actionName, "VectorPolygonTool") == 0) return "tool_lasso_poly";
        if (strcmp(actionName, "VectorSelectTool") == 0) return "tool_transform";
        if (strcmp(actionName, "VectorEditTool") == 0) return "tool_wand";
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
            labels.data(), variantCount, &sel, tip, Ui::DropdownFlags_ClickAndHold, isActive);

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
        // Active chrome: soft fill via DropdownIcon(active); outline = floating square
    }

    void RenderAll(UIState& state, Canvas& canvas, BrushSettings& brush, ActiveTool& activeTool, ID3D11Device* device, ID3D11DeviceContext* context, GLFWwindow* window) {
        if (device)
            Ui::SvgIconCache::Get().SetDevice(device);
        Ui::TooltipSetDelay(Ui::Tokens().tooltipDelaySec);
        ImGuiViewport* mainViewport = ImGui::GetMainViewport();

        // 0. Custom title bar (undecorated window): drag + project tabs + window controls
        {
            const float titleH = 32.f;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.f, 4.f));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.f, 0.f));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.10f, 1.f));
            ImGui::BeginViewportSideBar("##TitleBar", mainViewport, ImGuiDir_Up, titleH,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoDocking);

            // Drag region: full bar minus right buttons
            const float btnW = 42.f;
            const float rightCtrls = btnW * 3.f + 8.f;
            ImVec2 barMin = ImGui::GetWindowPos();
            ImVec2 barSize = ImGui::GetWindowSize();
            ImGui::InvisibleButton("##title_drag", ImVec2(std::max(40.f, barSize.x - rightCtrls - 8.f), titleH - 4.f));
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                if (GLFWwindow* gw = window) {
                    int wx = 0, wy = 0;
                    glfwGetWindowPos(gw, &wx, &wy);
                    ImVec2 d = ImGui::GetIO().MouseDelta;
                    glfwSetWindowPos(gw, wx + (int)d.x, wy + (int)d.y);
                }
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && window) {
                if (glfwGetWindowAttrib(window, GLFW_MAXIMIZED))
                    glfwRestoreWindow(window);
                else
                    glfwMaximizeWindow(window);
            }
            // Draw title + tabs over drag region
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                auto& tok = Ui::Tokens();
                dl->AddText(ImVec2(barMin.x + 10.f, barMin.y + 8.f), tok.ColU32(tok.textSecondary), "RayV");
                float tabX = barMin.x + 52.f;
                float tabY = barMin.y + 4.f;
                auto tabs = ProjectManager::Get().ListTabs();
                for (const auto& tab : tabs) {
                    std::string label = tab.title;
                    if (tab.dirty) label += " *";
                    ImVec2 ts = ImGui::CalcTextSize(label.c_str());
                    float tw = ts.x + 28.f;
                    ImVec2 a(tabX, tabY);
                    ImVec2 b(tabX + tw, tabY + titleH - 8.f);
                    ImU32 bg = tab.active ? IM_COL32(50, 70, 110, 220) : IM_COL32(35, 35, 42, 180);
                    dl->AddRectFilled(a, b, bg, 4.f);
                    dl->AddText(ImVec2(a.x + 8.f, a.y + 4.f), tok.ColU32(tok.textPrimary), label.c_str());
                    // Hit: select tab
                    if (ImGui::IsMouseHoveringRect(a, b) && ImGui::IsMouseClicked(0) &&
                        !ImGui::IsMouseDragging(0)) {
                        if (!tab.active) ProjectManager::Get().SwitchTo(tab.id);
                    }
                    // Close x
                    ImVec2 xa(b.x - 16.f, a.y + 4.f);
                    ImVec2 xb(b.x - 4.f, a.y + 16.f);
                    if (ImGui::IsMouseHoveringRect(xa, ImVec2(b.x - 2.f, b.y - 2.f))) {
                        dl->AddText(xa, IM_COL32(255, 120, 120, 255), "x");
                        if (ImGui::IsMouseClicked(0)) {
                            if (!ProjectManager::Get().CloseProject(tab.id, false)) {
                                g_ProjectTabCloseRequest.projectId = tab.id;
                                g_ProjectTabCloseRequest.pending = true;
                            }
                        }
                    } else {
                        dl->AddText(xa, tok.ColU32(tok.textSecondary), "x");
                    }
                    tabX += tw + 4.f;
                }
                // New tab
                ImVec2 na(tabX, tabY);
                ImVec2 nb(tabX + 22.f, tabY + titleH - 8.f);
                dl->AddRectFilled(na, nb, IM_COL32(40, 40, 48, 200), 4.f);
                dl->AddText(ImVec2(na.x + 6.f, na.y + 3.f), tok.ColU32(tok.textPrimary), "+");
                if (ImGui::IsMouseHoveringRect(na, nb) && ImGui::IsMouseClicked(0))
                    ProjectManager::Get().CreateEmptyProject();
            }

            // Window controls (right)
            ImGui::SetCursorScreenPos(ImVec2(barMin.x + barSize.x - rightCtrls, barMin.y + 2.f));
            if (ImGui::SmallButton("—##min")) {
                if (window) glfwIconifyWindow(window);
            }
            if (ImGui::IsItemHovered()) Ui::Tooltip("Minimize");
            ImGui::SameLine(0, 2);
            if (ImGui::SmallButton("[]##max")) {
                if (window) {
                    if (glfwGetWindowAttrib(window, GLFW_MAXIMIZED))
                        glfwRestoreWindow(window);
                    else
                        glfwMaximizeWindow(window);
                }
            }
            if (ImGui::IsItemHovered()) Ui::Tooltip("Maximize / Restore");
            ImGui::SameLine(0, 2);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.2f, 0.2f, 1.f));
            if (ImGui::SmallButton("X##close")) {
                if (window) glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) Ui::Tooltip("Close");

            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
        }

        // 1. Persistent Header (Main Menu Bar)
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                // Catalog-backed: same poll + execute as hotkeys
                core::ops::MenuAction("NewProject");
                if (ImGui::MenuItem("New Blank Tab")) {
                    ProjectManager::Get().CreateEmptyProject();
                }
                if (ImGui::MenuItem("Import Texture…")) {
                    UI::FileExplorerOpen(state.fileExplorer, UI::FileExplorerMode::ImportTexture);
                }
                core::ops::MenuAction("OpenProject");
                core::ops::MenuAction("SaveProject");
                ImGui::Separator();
                if (ImGui::MenuItem("Import Image...", "Ctrl+I")) {
                    state.openImportModal = true;
                }
                core::ops::MenuAction("QuickExport");
                {
                    // Prefill export path for Simple, then same AdvancedExport operator
                    const bool advanced = canvas.GetProjectType() != Canvas::ProjectType::Simple;
                    const char* aeLabel = advanced ? "Batch Export…" : "Advanced Export…";
                    std::string sc = KeymapManager::Get().GetActionShortcutString("AdvancedExport");
                    if (ImGui::MenuItem(aeLabel, (sc == "—" || sc == "None") ? nullptr : sc.c_str())) {
                        if (!advanced && !canvas.GetExportPath().empty()) {
                            try {
                                auto p = std::filesystem::path(PathUtil::Utf8ToWide(canvas.GetExportPath()));
                                std::string dir = PathUtil::WideToUtf8(p.parent_path().wstring());
                                std::string fn = PathUtil::WideToUtf8(p.filename().wstring());
                                if (!dir.empty()) state.fileExplorer.currentDir = dir;
                                if (!fn.empty())
                                    std::snprintf(state.fileExplorer.saveFileName,
                                        sizeof(state.fileExplorer.saveFileName), "%s", fn.c_str());
                            } catch (...) {}
                        }
                        core::ops::Invoke("AdvancedExport");
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Load Config...")) {
                    UI::FileExplorerOpen(state.fileExplorer, UI::FileExplorerMode::LoadConfig);
                }
                if (ImGui::MenuItem("Save Config...")) {
                    UI::FileExplorerOpen(state.fileExplorer, UI::FileExplorerMode::SaveConfig);
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
                std::string undoSc = KeymapManager::Get().GetActionShortcutString("Undo");
                if (ImGui::MenuItem(undoLabel.c_str(),
                        (undoSc == "—" || undoSc == "None") ? nullptr : undoSc.c_str(),
                        false, canvas.CanUndo())) {
                    core::ops::Invoke("Undo");
                }

                std::string redoLabel = "Redo";
                if (canvas.CanRedo()) {
                    redoLabel += " (" + canvas.GetRedoName() + ")";
                }
                std::string redoSc = KeymapManager::Get().GetActionShortcutString("Redo");
                if (ImGui::MenuItem(redoLabel.c_str(),
                        (redoSc == "—" || redoSc == "None") ? nullptr : redoSc.c_str(),
                        false, canvas.CanRedo())) {
                    core::ops::Invoke("Redo");
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Canvas")) {
                if (ImGui::MenuItem("Canvas Edit...")) {
                    state.openCanvasSizeModal = true;
                }
                core::ops::MenuAction("CropToSelection", canvas.HasSelection());
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
            // ---- Image Menu (catalog operators) ----
            if (ImGui::BeginMenu("Layer")) {
                int ai = canvas.GetActiveLayerIndex();
                bool isVec = ai >= 0 && ai < (int)canvas.GetLayers().size() &&
                             canvas.GetLayers()[ai].IsVector();
                if (ImGui::MenuItem("New Vector Layer")) {
                    canvas.CreateVectorLayer(device, "Vector");
                }
                if (ImGui::MenuItem("Export Vector Layer as SVG…", nullptr, false, isVec)) {
                    vec::VectorActionExportSvg(canvas);
                }
                if (ImGui::MenuItem("Rasterize Layer", nullptr, false, ai >= 0)) {
                    canvas.RasterizeLayer(device, ai);
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Image")) {
                bool hasLayer = canvas.GetActiveLayerIndex() != -1;
                core::ops::MenuAction("FreeTransform", hasLayer);
                core::ops::MenuAction("PerspectiveWarp", hasLayer);
                core::ops::MenuAction("MeshWarp", hasLayer);
                ImGui::Separator();
                core::ops::MenuAction("RefreshCanvas");
                ImGui::Separator();
                core::ops::MenuAction("InvertColors", hasLayer);
                core::ops::MenuAction("InvertAlpha", hasLayer);
                ImGui::Separator();
                core::ops::MenuAction("AdjustBlur", hasLayer);
                core::ops::MenuAction("AdjustHSV", hasLayer);
                core::ops::MenuAction("AdjustCurves", hasLayer);
                core::ops::MenuAction("AdjustNoise", hasLayer);
                ImGui::Separator();
                {
                    bool canCaf = hasLayer && canvas.HasSelection();
                    if (core::ops::MenuAction("ContentAwareFill", canCaf)) { /* ran */ }
                    if (ImGui::IsItemHovered() && !canCaf)
                        Ui::Tooltip("Requires an active selection (region to fill) on a paint layer.");
                }
                ImGui::Separator();
                if (ImGui::BeginMenu("Document Bit Depth")) {
                    using BD = Canvas::DocumentBitDepth;
                    auto cur = canvas.GetDocumentBitDepth();
                    if (ImGui::MenuItem("8-bit / channel (U8)", nullptr, cur == BD::U8))
                        canvas.SetDocumentBitDepth(BD::U8);
                    if (ImGui::MenuItem("16-bit float (F16)", nullptr, cur == BD::F16))
                        canvas.SetDocumentBitDepth(BD::F16);
                    if (ImGui::MenuItem("32-bit float (F32)", nullptr, cur == BD::F32))
                        canvas.SetDocumentBitDepth(BD::F32);
                    ImGui::Separator();
                    ImGui::TextDisabled("Working space only — export packing free.");
                    ImGui::TextDisabled("U8 default for diffuse/BC7. F16/F32 for height/HDR.");
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }
            // ---- Select Menu ----
            if (ImGui::BeginMenu("Select")) {
                core::ops::MenuAction("SelectAll");
                core::ops::MenuAction("Deselect");
                core::ops::MenuAction("InvertSelection");
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                ImGui::MenuItem("Toolbar", nullptr, &state.showToolbar);
                ImGui::MenuItem("Properties", nullptr, &state.showProperties);
                ImGui::MenuItem("Viewport Navigation", nullptr, &state.showViewportNav);
                ImGui::MenuItem("Layers", nullptr, &state.showLayers);
                ImGui::MenuItem("Asset Browser", nullptr, &state.showAssetBrowser);
                // Layer Effects: only via Layers panel (Fx), not View menu
                ImGui::MenuItem("Channels", nullptr, &state.showChannels);
                ImGui::MenuItem("Colors Window", nullptr, &state.showColors);
                ImGui::MenuItem("Tool Settings", nullptr, &state.showToolSettings);
                ImGui::MenuItem("Console logs", nullptr, &state.showConsole);
                script::ScriptDockRegistry::Get().DrawViewMenuItems();
                ImGui::Separator();
                {
                    bool fxPrev = canvas.GetEffectsPreviewEnabled();
                    if (ImGui::MenuItem("Preview Layer Effects", nullptr, fxPrev)) {
                        canvas.SetEffectsPreviewEnabled(!fxPrev);
                    }
                    if (ImGui::IsItemHovered())
                        Ui::Tooltip(
                            "ON: bake shadows/outlines/filters for display (CPU — can lag).\n"
                            "OFF: show raw paint content so brush stays fast.\n"
                            "Effects stay on the layer; toggle ON to re-bake.\n"
                            "Export always applies full effects.");
                }
                ImGui::MenuItem("Rulers", nullptr, &state.showRulers);
                ImGui::MenuItem("Mod Setup…", nullptr, &state.showModSetup);
                ImGui::MenuItem("3D Preview", nullptr, &state.showPreview3D);
                if (ImGui::MenuItem("Reset View")) {
                    canvas.ResetView();
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Scripting")) {
                if (ImGui::MenuItem("Refresh Scripts")) {
                    std::string sum;
                    if (ScriptingEngine::Get().ReloadPlugins(&sum))
                        Logger::Get().Info("Scripts refreshed: " + sum);
                    else
                        Logger::Get().Error("Scripts refresh failed: " + sum);
                }
                if (ImGui::IsItemHovered())
                    Ui::Tooltip("Reload {exe}/scripts + Documents/RayVPaint/scripts");

                ImGui::Separator();
                // Dynamic plugin open entries
                {
                    auto& host = script::ScriptPluginHost::Get();
                    if (host.List().empty()) {
                        ImGui::TextDisabled("(no plugins — Refresh Scripts)");
                    } else {
                        for (const auto& p : host.List()) {
                            std::string label = p.title;
                            if (p.source == "user") label += "  [user]";
                            else label += "  [builtin]";
                            if (ImGui::MenuItem(label.c_str()))
                                host.RequestOpen(p.id);
                        }
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Open user scripts folder")) {
                    std::string dir = script::ScriptPluginHost::UserScriptsDir();
#ifdef _WIN32
                    ShellExecuteA(nullptr, "open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
                    (void)dir;
#endif
                }
                if (ImGui::MenuItem("Run test command")) {
                    ScriptingEngine::Get().RunString("import rayv; rayv.log_warn('Executing scripting check.')");
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Help")) {
                // ---- Explorer integration (Blender-style register) ----
                {
                    auto st = DdsThumbRegister::QueryStatus();
                    ImGui::TextUnformatted("Windows Explorer integration");
                    ImGui::PushStyleColor(ImGuiCol_Text, st.fullyOk
                        ? ImVec4(0.45f, 0.9f, 0.5f, 1.f)
                        : ImVec4(0.95f, 0.75f, 0.35f, 1.f));
                    ImGui::TextWrapped("%s", st.summary.c_str());
                    ImGui::PopStyleColor();
                    ImGui::TextDisabled("%s", st.elevLine.c_str());
                    ImGui::TextDisabled("%s", st.dllLine.c_str());
                    ImGui::TextDisabled("%s", st.thumbLine.c_str());
                    ImGui::TextDisabled("%s", st.propLine.c_str());
                    ImGui::Spacing();
                    if (ImGui::MenuItem("Register (DDS thumbs + type + fix PNG)")) {
                        if (DdsThumbRegister::EnsureRegistered({}, true))
                            Logger::Get().Info(
                                "Explorer register OK. Restart Explorer / F5 if icons linger.");
                        else
                            Logger::Get().Error(
                                "Register incomplete — accept UAC, ensure RayVPaint_DdsThumb.dll "
                                "is next to Core.");
                    }
                    if (ImGui::IsItemHovered())
                        Ui::Tooltip(
                            "Registers COM thumbnail + property handlers (HKLM needs Admin once).\n"
                            "ProgID RayVPaint.dds, KindMap Picture, PNG Photo restore.\n"
                            "Does NOT put ShellEx on Applications\\RayVPaint.");
                    if (ImGui::MenuItem("Unregister Explorer integration")) {
                        if (DdsThumbRegister::Unregister())
                            Logger::Get().Info("Explorer integration unregistered.");
                        else
                            Logger::Get().Error("Unregister failed (DLL missing or UAC denied).");
                    }
                    if (ImGui::IsItemHovered())
                        Ui::Tooltip("Removes our CLSID / ShellEx / property handler (elevates for HKLM).");
                    ImGui::Separator();
                    // ---- Helper microservices (PNG→DDS + atlas) — registry only ----
                    {
                        auto hs = HelperShellRegister::QueryStatus();
                        ImGui::TextUnformatted("PNG helper tools");
                        ImGui::PushStyleColor(ImGuiCol_Text, hs.fullyOk
                            ? ImVec4(0.45f, 0.9f, 0.5f, 1.f)
                            : ImVec4(0.95f, 0.75f, 0.35f, 1.f));
                        ImGui::TextWrapped("%s", hs.summary.c_str());
                        ImGui::PopStyleColor();
                        ImGui::TextDisabled("%s", hs.exeLine.c_str());
                        ImGui::TextDisabled("%s", hs.convertLine.c_str());
                        ImGui::TextDisabled("%s", hs.atlasLine.c_str());
                        if (ImGui::MenuItem("Register helpers (PNG → DDS + Atlas)")) {
                            if (HelperShellRegister::EnsureRegistered())
                                Logger::Get().Info(
                                    "PNG helpers registered. Right-click PNG(s) in Explorer.");
                            else
                                Logger::Get().Error(
                                    "Helpers register failed — build RayVHelpers.exe next to Core.");
                        }
                        if (ImGui::IsItemHovered())
                            Ui::Tooltip(
                                "Registers Explorer context menu for multi-select PNG:\n"
                                "• Convert to DDS (RayVPaint)\n"
                                "• Create Texture Atlas (RayVPaint)\n"
                                "Uses RayVHelpers.exe + same app icon. HKCU only (no Admin).");
                        if (ImGui::MenuItem("Unregister helpers")) {
                            if (HelperShellRegister::Unregister())
                                Logger::Get().Info("PNG helpers unregistered.");
                            else
                                Logger::Get().Error("Helpers unregister failed.");
                        }
                    }
                    ImGui::Separator();
                }
                if (ImGui::MenuItem("About RayV-Paint…"))
                    state.openAboutModal = true;
                ImGui::EndMenu();
            }

            // ---- Right header: mode · I/O · map switcher (compact, high-use) ----
            {
                Project* hp = ProjectManager::Get().ActiveProject();
                auto ptype = canvas.GetProjectType();
                const bool adv = (ptype != Canvas::ProjectType::Simple);
                if (hp) hp->textureSets.EnsureSimpleDefault();
                texset::TextureSet* hset = hp ? hp->textureSets.Active() : nullptr;

                // Estimate width of right tools
                float rightTools = 210.f; // mode + I/O
                if (adv && hset) {
                    for (const auto& m : hset->maps)
                        if (m.enabled || m.kind == texset::MapKind::Diffuse)
                            rightTools += 72.f;
                }
                float barW = ImGui::GetWindowWidth();
                float x = barW - rightTools - 8.f;
                if (x > ImGui::GetCursorPosX() + 40.f)
                    ImGui::SameLine(x);

                // Project mode
                {
                    int mi = (ptype == Canvas::ProjectType::Simple) ? 0
                           : (ptype == Canvas::ProjectType::AdvancedModMode) ? 2 : 1;
                    const char* modes[] = { "Simple", "Advanced", "Adv Mod" };
                    ImGui::SetNextItemWidth(88.f);
                    if (UiCombo("##projmode", &mi, modes, 3)) {
                        canvas.SetProjectType(
                            mi == 0 ? Canvas::ProjectType::Simple :
                            mi == 2 ? Canvas::ProjectType::AdvancedModMode :
                                      Canvas::ProjectType::Advanced);
                        canvas.SetDocumentModified(true);
                        if (mi == 0)
                            canvas.SetViewMapKind(texset::MapKind::Diffuse);
                    }
                    if (ImGui::IsItemHovered())
                        Ui::Tooltip("Project mode — workspace layout");
                }
                ImGui::SameLine(0, 6);
                if (adv) {
                    if (ImGui::SmallButton("Import"))
                        UI::FileExplorerOpen(state.fileExplorer, UI::FileExplorerMode::ImportTexture);
                    ImGui::SameLine(0, 4);
                    if (ImGui::SmallButton("Export"))
                        state.openQuickExportTrigger = true;
                    ImGui::SameLine(0, 8);
                    if (ImGui::SmallButton("Setup"))
                        state.openProjectSetup = true;
                } else {
                    if (ImGui::SmallButton("Import"))
                        state.openImportModal = true;
                    ImGui::SameLine(0, 4);
                    if (ImGui::SmallButton("Export"))
                        state.openQuickExportTrigger = true;
                }
            }

            ImGui::EndMainMenuBar();
        }

        // Dirty close confirm
        if (g_ProjectTabCloseRequest.pending) {
            ImGui::OpenPopup("Close Project?##dirty");
            g_ProjectTabCloseRequest.pending = false;
        }
        if (ImGui::BeginPopupModal("Close Project?##dirty", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("This project has unsaved changes.");
            ImGui::Text("Close anyway?");
            ImGui::Spacing();
            if (ImGui::Button("Close Without Saving", ImVec2(180, 0))) {
                ProjectManager::Get().CloseProject(g_ProjectTabCloseRequest.projectId, true);
                g_ProjectTabCloseRequest.projectId = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100, 0))) {
                g_ProjectTabCloseRequest.projectId = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // ---- Image Adjustment Modals (live layer+selection preview) ----
        // One session at a time; cancel restores base snapshot.
        static bool s_AdjPreviewBegun = false;
        static bool s_WasAnyAdjModal = false;
        const bool anyAdjModal = state.showBlurModal || state.showHSVModal ||
                                 state.showCurvesModal || state.showNoiseModal;
        auto endAdjSession = [&](bool commit, const char* name) {
            if (s_AdjPreviewBegun) {
                if (commit) canvas.CommitAdjustPreview(name ? name : "Adjust");
                else canvas.CancelAdjustPreview();
                s_AdjPreviewBegun = false;
            }
        };
        auto ensureAdjPreview = [&]() -> bool {
            if (s_AdjPreviewBegun) return canvas.IsAdjustPreviewActive();
            s_AdjPreviewBegun = canvas.BeginAdjustPreview();
            return s_AdjPreviewBegun;
        };
        // Closed via X / Esc without Apply/Cancel buttons
        if (s_WasAnyAdjModal && !anyAdjModal)
            endAdjSession(false, nullptr);
        s_WasAnyAdjModal = anyAdjModal;

        // Operator modals: NO dim/scrim — user must see live image changes (not a "cat in a bag").
        auto beginOperatorModal = [](const char* name, bool* open) -> bool {
            ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.f, 0.f, 0.f, 0.f));
            bool openOk = ImGui::BeginPopupModal(name, open, ImGuiWindowFlags_AlwaysAutoResize);
            if (!openOk) ImGui::PopStyleColor(); // balanced when not open
            return openOk;
        };
        auto endOperatorModal = []() {
            ImGui::EndPopup();
            ImGui::PopStyleColor(); // ModalWindowDimBg
        };

        // Blur Modal
        if (state.showBlurModal) ImGui::OpenPopup("Blur##modal");
        if (beginOperatorModal("Blur##modal", &state.showBlurModal)) {
            if (!s_AdjPreviewBegun && ensureAdjPreview())
                canvas.UpdateAdjustPreviewBlur(state.blurRadius);
            ImGui::Text("Blur (3-pass box) — preview on active layer");
            ImGui::TextDisabled("Respects selection; only this layer");
            if (Ui::SmartSliderFloat("Radius", &state.blurRadius, 0.5f, 80.0f, 5.f, 0.5f, "%.1f px")) {
                if (ensureAdjPreview())
                    canvas.UpdateAdjustPreviewBlur(state.blurRadius);
            }
            ImGui::Spacing();
            if (ImGui::Button("Apply", ImVec2(100, 0))) {
                endAdjSession(true, "Blur");
                state.showBlurModal = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100, 0))) {
                endAdjSession(false, nullptr);
                state.showBlurModal = false;
                ImGui::CloseCurrentPopup();
            }
            endOperatorModal();
        }

        // HSV Modal
        if (state.showHSVModal) ImGui::OpenPopup("HSV Adjust##modal");
        if (beginOperatorModal("HSV Adjust##modal", &state.showHSVModal)) {
            if (!s_AdjPreviewBegun) ensureAdjPreview();
            ImGui::Text("Hue / Saturation / Value");
            ImGui::TextDisabled("Live preview on active layer · selection mask");
            bool ch = false;
            ch |= Ui::SmartSliderFloat("Hue", &state.hsvH, -0.5f, 0.5f, 0.f, 0.01f, "%.3f");
            ch |= Ui::SmartSliderFloat("Saturation", &state.hsvS, -1.0f, 1.0f, 0.f, 0.05f, "%.3f");
            ch |= Ui::SmartSliderFloat("Value", &state.hsvV, -1.0f, 1.0f, 0.f, 0.05f, "%.3f");
            if (ch || ImGui::IsItemDeactivatedAfterEdit()) {
                if (ensureAdjPreview())
                    canvas.UpdateAdjustPreviewHSV(state.hsvH, state.hsvS, state.hsvV);
            }
            ImGui::Spacing();
            if (ImGui::Button("Apply", ImVec2(100, 0))) {
                endAdjSession(true, "HSV");
                state.hsvH = state.hsvS = state.hsvV = 0.f;
                state.showHSVModal = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100, 0))) {
                endAdjSession(false, nullptr);
                state.hsvH = state.hsvS = state.hsvV = 0.f;
                state.showHSVModal = false;
                ImGui::CloseCurrentPopup();
            }
            endOperatorModal();
        }

        // Noise Modal
        if (state.showNoiseModal) ImGui::OpenPopup("Add Noise##modal");
        if (beginOperatorModal("Add Noise##modal", &state.showNoiseModal)) {
            if (!s_AdjPreviewBegun) ensureAdjPreview();
            ImGui::TextDisabled("Live preview (stable seed until Apply)");
            bool ch = false;
            ch |= Ui::SmartSliderFloat("Strength", &state.noiseStrength, 0.0f, 1.0f, 0.1f, 0.05f, "%.3f");
            ch |= ImGui::Checkbox("Color Noise", &state.noiseColor);
            if (ch) {
                if (ensureAdjPreview())
                    canvas.UpdateAdjustPreviewNoise(state.noiseStrength, state.noiseColor);
            }
            ImGui::Spacing();
            if (ImGui::Button("Apply", ImVec2(100, 0))) {
                endAdjSession(true, "Noise");
                state.showNoiseModal = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100, 0))) {
                endAdjSession(false, nullptr);
                state.showNoiseModal = false;
                ImGui::CloseCurrentPopup();
            }
            endOperatorModal();
        }

        // Curves Modal — interactive spline editor + live canvas preview
        if (state.showCurvesModal) ImGui::OpenPopup("Curves##modal");
        if (beginOperatorModal("Curves##modal", &state.showCurvesModal)) {
            if (!s_AdjPreviewBegun) ensureAdjPreview();
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
            ImGui::SameLine(); ImGui::TextDisabled("Live layer preview · selection");

            std::vector<std::pair<float,float>>& activePoints = (state.curvesChannel == 0) ? state.curvesPointsRGB : state.curvesPointsAlpha;
            std::vector<float>& activeLUT = (state.curvesChannel == 0) ? state.curvesLUTRGB : state.curvesLUTAlpha;

            const float graphSz = 256.f;
            const float pad = 8.f;
            bool curvesChanged = false;

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
                    curvesChanged = true;
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
                    curvesChanged = true;
                } else draggingPt=-1;
            }
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
                    curvesChanged = true;
                }
            }

            ImGui::EndChild();

            if (curvesChanged) {
                state.curvesLUTRGB = Canvas_BuildSplineLUT(state.curvesPointsRGB);
                state.curvesLUTAlpha = Canvas_BuildSplineLUT(state.curvesPointsAlpha);
                if (ensureAdjPreview())
                    canvas.UpdateAdjustPreviewCurves(state.curvesLUTRGB, state.curvesLUTAlpha);
            }

            float posX=(mpos.x-graphPos.x)/graphSz*255.f, posY=(1.f-(mpos.y-graphPos.y)/graphSz)*255.f;
            if (inGraph) ImGui::Text("(%.0f, %.0f)", posX, posY);
            else ImGui::TextDisabled("Move mouse over graph");
            ImGui::Spacing();
            if (ImGui::Button("Reset",ImVec2(80,0))){
                activePoints={{0.f,0.f},{1.f,1.f}};
                activeLUT = Canvas_BuildSplineLUT(activePoints);
                state.curvesLUTRGB = Canvas_BuildSplineLUT(state.curvesPointsRGB);
                state.curvesLUTAlpha = Canvas_BuildSplineLUT(state.curvesPointsAlpha);
                if (ensureAdjPreview())
                    canvas.UpdateAdjustPreviewCurves(state.curvesLUTRGB, state.curvesLUTAlpha);
            }
            ImGui::SameLine();
            if (ImGui::Button("Apply",ImVec2(80,0))){
                endAdjSession(true, "Curves");
                state.showCurvesModal=false; ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel",ImVec2(80,0))){
                endAdjSession(false, nullptr);
                state.showCurvesModal=false; ImGui::CloseCurrentPopup();
            }
            endOperatorModal();
        }

        // 2. Persistent Footer — FIXED height (never jumps when jobs appear)
        core::JobManager::Get().PruneFinished(2500.0);
        const auto jobs = core::JobManager::Get().Snapshot();
        bool hasActiveJob = false;
        for (const auto& j : jobs) {
            if (j.state == core::JobState::Running || j.state == core::JobState::Cancelling) {
                hasActiveJob = true;
                break;
            }
        }
        const bool feBusy = UI::FileExplorerIsBusy();
        const bool assetsBusy = assets::AssetManager::Get().IsBusy();
        const bool showBusyChrome = hasActiveJob || feBusy || assetsBusy || g_LoadingState.isLoading;
        // Always 28px — job status is centered in-band (no second row)
        const float statusBarH = 28.0f;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));
        ImGui::BeginViewportSideBar("##StatusBar", mainViewport, ImGuiDir_Down, statusBarH,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoScrollbar);
        
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
            case ActiveTool::MovePixels: toolLabel = "Move"; break;
            case ActiveTool::Pipette: toolLabel = "Pipette"; break;
            case ActiveTool::BucketFill: toolLabel = "Bucket Fill"; break;
            case ActiveTool::Gradient: toolLabel = "Gradient"; break;
            case ActiveTool::Smudge: toolLabel = "Smudge"; break;
            case ActiveTool::BlurTool: toolLabel = "Blur"; break;
            case ActiveTool::Stamp: toolLabel = "Stamp"; break;
            case ActiveTool::VectorSelect: toolLabel = "Select shapes"; break;
            case ActiveTool::VectorEdit: toolLabel = "Edit nodes"; break;
            case ActiveTool::VectorPen: toolLabel = "Pen"; break;
            case ActiveTool::VectorRect: toolLabel = "Rectangle"; break;
            case ActiveTool::VectorEllipse: toolLabel = "Ellipse"; break;
            case ActiveTool::VectorLine: toolLabel = "Line"; break;
            case ActiveTool::VectorFreehand: toolLabel = "Freehand"; break;
            case ActiveTool::VectorPolygon: toolLabel = "Polygon"; break;
        }
        {
            const char* bdLabel =
                (canvas.GetDocumentBitDepth() == Canvas::DocumentBitDepth::F32) ? "F32" :
                (canvas.GetDocumentBitDepth() == Canvas::DocumentBitDepth::F16) ? "F16" : "U8";
            const int bpp = BytesPerPixel(Canvas::FormatForBitDepth(canvas.GetDocumentBitDepth()));
            const bool floatDoc = (canvas.GetDocumentBitDepth() != Canvas::DocumentBitDepth::U8);

            // Layout: LEFT metrics | CENTER job (fixed slot) | RIGHT notify + context
            // Heights fixed — job strip never changes footer height.
            const float winW = ImGui::GetWindowWidth();
            const float contextBtnW = 72.f;
            const float notifyChipW = 148.f;
            const float centerJobW = 280.f;
            const float rightReserve = contextBtnW + notifyChipW + 16.f;
            const float leftMax = std::max(80.f, winW - rightReserve - centerJobW - 24.f);

            // LEFT — compact metrics (clipped)
            {
                char left[256];
                if (floatDoc) {
                    std::snprintf(left, sizeof(left),
                        "%.0fms | %.0f FPS | %dx%d %s | Z%.0f%% | %s",
                        state.frameTimeMs, state.fps,
                        canvas.GetWidth(), canvas.GetHeight(), bdLabel,
                        canvas.GetZoom() * 100.f, toolLabel);
                } else {
                    std::snprintf(left, sizeof(left),
                        "%.0f FPS | %dx%d | Z%.0f%% | %s",
                        state.fps, canvas.GetWidth(), canvas.GetHeight(),
                        canvas.GetZoom() * 100.f, toolLabel);
                }
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + leftMax);
                ImGui::TextUnformatted(left);
                ImGui::PopTextWrapPos();
            }

            // CENTER — fixed job slot (always reserved position, content optional)
            {
                const float centerX = (winW - centerJobW) * 0.5f;
                ImGui::SameLine(std::max(leftMax + 8.f, centerX));
                ImGui::BeginChild("##footer_job", ImVec2(centerJobW, 22.f), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                if (showBusyChrome) {
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    const float r = 5.f;
                    const ImVec2 c(p.x + r + 1.f, p.y + 10.f);
                    const float t = (float)ImGui::GetTime() * 7.5f;
                    for (int i = 0; i < 8; ++i) {
                        const float a = t + (float)i * 6.2831853f / 8.f;
                        const float fade = 0.2f + 0.8f * ((float)i / 8.f);
                        dl->AddCircleFilled(
                            ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r),
                            1.8f, IM_COL32(130, 175, 255, (int)(fade * 255.f)), 6);
                    }
                    ImGui::Dummy(ImVec2(r * 2.f + 6.f, 18.f));
                    ImGui::SameLine(0, 4);

                    bool drew = false;
                    for (const auto& j : jobs) {
                        if (j.state != core::JobState::Running && j.state != core::JobState::Cancelling)
                            continue;
                        char line[160];
                        if (j.progress >= 0.f)
                            std::snprintf(line, sizeof(line), "%s %d%%", j.name.c_str(),
                                (int)std::lround(j.progress * 100.f));
                        else
                            std::snprintf(line, sizeof(line), "%s…", j.name.c_str());
                        ImGui::TextUnformatted(line);
                        if (j.cancellable) {
                            ImGui::SameLine();
                            char cid[40];
                            std::snprintf(cid, sizeof(cid), "x##j%llu", (unsigned long long)j.id);
                            if (ImGui::SmallButton(cid))
                                core::JobManager::Get().RequestCancel(j.id);
                        }
                        drew = true;
                        break;
                    }
                    if (!drew) {
                        if (g_LoadingState.isLoading)
                            ImGui::TextUnformatted("Opening…");
                        else if (state.fileExplorer.dirListingBusy)
                            ImGui::TextUnformatted("Indexing folder…");
                        else if (assets::AssetManager::Get().IsIndexScanning())
                            ImGui::TextUnformatted("Indexing assets…");
                        else if (feBusy || assetsBusy)
                            ImGui::TextUnformatted("Background…");
                    }
                }
                ImGui::EndChild();
            }

            // RIGHT — notification + context (absolute positions, stable)
            {
                std::string preview = core::Notifications::Get().LatestPreview(28);
                if (preview.empty()) preview = "—";
                ImVec4 col(0.75f, 0.78f, 0.85f, 1.f);
                switch (core::Notifications::Get().LatestLevel()) {
                case core::NotifyLevel::Warning: col = ImVec4(1.f, 0.82f, 0.4f, 1.f); break;
                case core::NotifyLevel::Error:   col = ImVec4(1.f, 0.45f, 0.45f, 1.f); break;
                default: break;
                }
                ImGui::SameLine(winW - rightReserve + 4.f);
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                if (ImGui::SmallButton((preview + "##notify").c_str()))
                    ImGui::OpenPopup("##NotifyHistory");
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    Ui::Tooltip("Notifications");

                if (ImGui::BeginPopup("##NotifyHistory")) {
                    ImGui::TextUnformatted("Notification history");
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Clear##nh"))
                        core::Notifications::Get().Clear();
                    ImGui::Separator();
                    auto hist = core::Notifications::Get().History();
                    ImGui::BeginChild("##nhscroll", ImVec2(420, 220), true);
                    for (int i = (int)hist.size() - 1; i >= 0; --i) {
                        const auto& n = hist[(size_t)i];
                        ImVec4 c(0.85f, 0.85f, 0.9f, 1.f);
                        if (n.level == core::NotifyLevel::Warning) c = ImVec4(1.f, 0.82f, 0.4f, 1.f);
                        if (n.level == core::NotifyLevel::Error)   c = ImVec4(1.f, 0.45f, 0.45f, 1.f);
                        ImGui::PushStyleColor(ImGuiCol_Text, c);
                        if (ImGui::Selectable(n.message.c_str(), false))
                            ImGui::SetClipboardText(n.message.c_str());
                        ImGui::PopStyleColor();
                    }
                    ImGui::EndChild();
                    ImGui::EndPopup();
                }

                ImGui::SameLine(winW - contextBtnW - 10.f);
                if (ImGui::SmallButton(state.showContextDebug ? "Context*" : "Context"))
                    state.showContextDebug = !state.showContextDebug;
                if (ImGui::IsItemHovered())
                    Ui::Tooltip("AppContext live dump");
            }
        }
        
        ImGui::End();
        ImGui::PopStyleVar();

        // Context Debug panel (AppContext understandability tool)
        if (state.showContextDebug) {
            ImGui::SetNextWindowSize(ImVec2(420, 360), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Context", &state.showContextDebug)) {
                const auto& ctx = core::ops::AppContext::CGet();
                std::vector<std::string> lines;
                ctx.AppendDebugLines(lines);
                for (const auto& ln : lines)
                    ImGui::TextUnformatted(ln.c_str());
                ImGui::Separator();
                ImGui::TextDisabled("Document ops blocked: %s", ctx.blocksDocumentOps ? "YES" : "no");
                ImGui::TextDisabled("Canvas interaction blocked: %s", ctx.blocksCanvasInteraction ? "YES" : "no");
                ImGui::TextDisabled("Registered executes: %d",
                    (int)core::ops::OperatorRegistry::Get().List().size());
                ImGui::Separator();
                ImGui::TextUnformatted("Poll matrix (sample)");
                static const char* kSample[] = {
                    "FillSecondary", "DeleteContent", "Undo", "BrushTool", "AdjustNoise",
                    "LassoToolGroup", "Deselect", "Paste", "SwapColors"
                };
                if (ImGui::BeginTable("##poll", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                    ImGui::TableSetupColumn("Action");
                    ImGui::TableSetupColumn("Poll");
                    ImGui::TableSetupColumn("Shortcut");
                    ImGui::TableHeadersRow();
                    for (const char* id : kSample) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        const auto* def = core::ops::ActionCatalog::Find(id);
                        ImGui::TextUnformatted(def && def->label ? def->label : id);
                        ImGui::TableNextColumn();
                        bool ok = ctx.PollAction(id);
                        ImGui::TextColored(ok ? ImVec4(0.3f, 0.9f, 0.4f, 1.f) : ImVec4(0.95f, 0.35f, 0.3f, 1.f),
                            ok ? "OK" : "BLOCKED");
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(KeymapManager::Get().GetActionShortcutString(id).c_str());
                    }
                    ImGui::EndTable();
                }
            }
            ImGui::End();
        }

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

        // 4. Modal / File Explorer triggers (FE owns project/config/import maps)
        if (state.openImportModal) {
            state.openImportModal = false;
            // Open as texture import browser (also works for single image pick)
            UI::FileExplorerOpen(state.fileExplorer, UI::FileExplorerMode::ImportTexture);
        }
        if (state.openExportDdsModal) { ImGui::OpenPopup("Export DDS"); state.openExportDdsModal = false; }
        if (state.openExportStdModal) { ImGui::OpenPopup("Export Standard Image"); state.openExportStdModal = false; }
        if (state.openExportAdvancedModal) {
            state.openExportAdvancedModal = false;
            UI::FileExplorerOpen(state.fileExplorer, UI::FileExplorerMode::AdvancedExport);
        }
        if (state.openSettingsModal) { ImGui::OpenPopup("Settings"); state.openSettingsModal = false; }
        if (state.openCanvasSizeModal) { ImGui::OpenPopup("Canvas Edit"); state.openCanvasSizeModal = false; }
        if (state.openSaveRaypModal) {
            state.openSaveRaypModal = false;
            UI::FileExplorerOpen(state.fileExplorer, UI::FileExplorerMode::SaveProject);
        }
        if (state.openLoadRaypModal) {
            state.openLoadRaypModal = false;
            UI::FileExplorerOpen(state.fileExplorer, UI::FileExplorerMode::OpenProject);
        }
        if (state.openLoadConfigModal) {
            state.openLoadConfigModal = false;
            UI::FileExplorerOpen(state.fileExplorer, UI::FileExplorerMode::LoadConfig);
        }
        if (state.openSaveConfigModal) {
            state.openSaveConfigModal = false;
            UI::FileExplorerOpen(state.fileExplorer, UI::FileExplorerMode::SaveConfig);
        }
        if (state.showRecoveryModal) { ImGui::OpenPopup("Restore Auto-Saved Session?"); state.showRecoveryModal = false; }

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

        // Advanced Export: File Explorer only (no legacy modal)

        // Settings / Preferences Popup Modal
        if (ImGui::BeginPopupModal("Settings", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (!state.settingsInitialized) {
                state.activeTheme = ConfigManager::Get().GetTheme();
                state.backupDir = ConfigManager::Get().GetBackupDir();
                state.defW = ConfigManager::Get().GetDefaultWidth();
                state.defH = ConfigManager::Get().GetDefaultHeight();
                state.autoSaveMins = ConfigManager::Get().GetAutoSaveIntervalMinutes();
                state.autosaveMaxPerProject = ConfigManager::Get().GetAutosaveMaxPerProject();
                state.maxUndo = ConfigManager::Get().GetMaxUndoSteps();
                state.maxUndoMem = ConfigManager::Get().GetMaxUndoMemoryMB();
                state.maxBrushRadius = ConfigManager::Get().GetMaxBrushRadius();
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
                    ImGui::Text("Brush");
                    ImGui::Separator();
                    Ui::SmartSliderFloat("Max brush radius (px)", &state.maxBrushRadius, 10.f, 1000.f, 250.f, 1.f, "%.0f");
                    ImGui::TextDisabled("Ctrl+Alt+RMB size range; 1 screen px = 1/zoom canvas px (WYSIWYG)");

                    ImGui::Spacing();
                    ImGui::Text("Autosave & Backup System");
                    ImGui::Separator();
                    
                    char tempBackupDir[256] = "";
                    std::strncpy(tempBackupDir, state.backupDir.c_str(), sizeof(tempBackupDir));
                    if (ImGui::InputText("Backups Directory", tempBackupDir, IM_ARRAYSIZE(tempBackupDir))) {
                        state.backupDir = tempBackupDir;
                    }
                    Ui::SmartSliderInt("Autosave (minutes)", &state.autoSaveMins, 0, 60, 3, 1);
                    ImGui::TextDisabled("Default 3 min. Set 0 to disable periodic auto-saves (quit save still runs).");
                    Ui::SmartSliderInt("Max autosaves per project", &state.autosaveMaxPerProject, 1, 30, 5, 1);
                    ImGui::TextDisabled("Keeps newest N files per project BASE (UNTITLED / stem). Includes quit saves.");

                    ImGui::Spacing();
                    ImGui::Text("Undo / Redo Cache Limits");
                    ImGui::Separator();
                    Ui::SmartSliderInt("Max History Steps", &state.maxUndo, 5, 200, 100, 1);
                    Ui::SmartSliderInt("Max RAM Cache Size", &state.maxUndoMem, 64, 2048, 512, 16);
                    
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Keybindings")) {
                    ImGui::Spacing();
                    ImGui::TextWrapped(
                        "Single source of truth: ActionCatalog (src/core/ops). "
                        "Categories group related actions. Group tools (Lasso L, Select S, Wand W) "
                        "cycle variants — members show \"via …\" when unbound.");
                    ImGui::Separator();
                    ImGui::Spacing();

                    // Rebind capture (Escape cancels)
                    if (state.listeningForKey) {
                        core::ops::AppContext::NotifyUiKeyboardCapture();
                        ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.f, 1.f),
                            "Listening for \"%s\" — press key (+ mods). Esc cancels.",
                            state.rebindingAction.c_str());
                        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                            state.listeningForKey = false;
                            state.rebindingAction.clear();
                        } else {
                            ImGuiIO& io = ImGui::GetIO();
                            for (int k = 0; k < ImGuiKey_NamedKey_END; ++k) {
                                ImGuiKey imguiKey = (ImGuiKey)k;
                                if (!ImGui::IsKeyPressed(imguiKey)) continue;
                                int glfwKey = 0;
                                if (imguiKey >= ImGuiKey_A && imguiKey <= ImGuiKey_Z) glfwKey = GLFW_KEY_A + (imguiKey - ImGuiKey_A);
                                else if (imguiKey >= ImGuiKey_0 && imguiKey <= ImGuiKey_9) glfwKey = GLFW_KEY_0 + (imguiKey - ImGuiKey_0);
                                else if (imguiKey >= ImGuiKey_F1 && imguiKey <= ImGuiKey_F12) glfwKey = GLFW_KEY_F1 + (imguiKey - ImGuiKey_F1);
                                else if (imguiKey == ImGuiKey_Space) glfwKey = GLFW_KEY_SPACE;
                                else if (imguiKey == ImGuiKey_Enter || imguiKey == ImGuiKey_KeypadEnter) glfwKey = GLFW_KEY_ENTER;
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

                                if (imguiKey == ImGuiKey_LeftCtrl || imguiKey == ImGuiKey_RightCtrl ||
                                    imguiKey == ImGuiKey_LeftShift || imguiKey == ImGuiKey_RightShift ||
                                    imguiKey == ImGuiKey_LeftAlt || imguiKey == ImGuiKey_RightAlt)
                                    continue;

                                if (glfwKey != 0) {
                                    KeyCombination pendingCombo;
                                    pendingCombo.key = glfwKey;
                                    pendingCombo.ctrl = io.KeyCtrl;
                                    pendingCombo.shift = io.KeyShift;
                                    pendingCombo.alt = io.KeyAlt;
                                    KeymapManager::Get().BindAction(state.rebindingAction, pendingCombo);
                                    state.listeningForKey = false;
                                    state.rebindingAction.clear();
                                    break;
                                }
                            }
                        }
                        ImGui::Separator();
                    }

                    auto list = core::ops::ActionCatalog::ListForKeybindUi();
                    core::ops::ActionCategory lastCat = core::ops::ActionCategory::COUNT;
                    for (const core::ops::ActionDef* def : list) {
                        if (!def || !def->id) continue;
                        if (def->category != lastCat) {
                            lastCat = def->category;
                            ImGui::Spacing();
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.75f, 1.f, 1.f));
                            ImGui::TextUnformatted(core::ops::ActionCatalog::CategoryLabel(def->category));
                            ImGui::PopStyleColor();
                            ImGui::Separator();
                        }

                        ImGui::PushID(def->id);
                        const char* indent = (def->role == core::ops::ActionRole::GroupMember) ? "    " : "";
                        ImGui::Text("%s%s", indent, def->label ? def->label : def->id);
                        if (ImGui::IsItemHovered() && def->note)
                            Ui::Tooltip(def->note);
                        if (ImGui::IsItemHovered()) {
                            ImGui::BeginTooltip();
                            ImGui::Text("id: %s", def->id);
                            if (def->role == core::ops::ActionRole::GroupMember && def->groupId)
                                ImGui::Text("group: %s", def->groupId);
                            if (def->note) ImGui::TextWrapped("%s", def->note);
                            ImGui::EndTooltip();
                        }

                        ImGui::SameLine(280);
                        std::string chord = KeymapManager::Get().GetActionShortcutString(def->id);
                        ImGui::TextDisabled("%s", chord.c_str());

                        ImGui::SameLine(400);
                        if (def->userRebindable) {
                            if (ImGui::SmallButton("Rebind")) {
                                state.rebindingAction = def->id;
                                state.listeningForKey = true;
                            }
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Clear")) {
                                KeyCombination none;
                                none.key = 0;
                                KeymapManager::Get().BindAction(def->id, none);
                            }
                        } else {
                            ImGui::TextDisabled("fixed");
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
                ConfigManager::Get().SetAutosaveMaxPerProject(state.autosaveMaxPerProject);
                ConfigManager::Get().SetMaxUndoSteps(state.maxUndo);
                ConfigManager::Get().SetMaxUndoMemoryMB(state.maxUndoMem);
                ConfigManager::Get().SetMaxBrushRadius(state.maxBrushRadius);
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

        // Save/Load Project + Config: File Explorer only (see triggers above)

        // Cold start: Recent autosaves browser (previews + open)
        if (state.showRecentAutosaves) {
            ImGui::OpenPopup("Recent Autosaves##recents");
            state.showRecentAutosaves = false;
        }
        {
            ImGuiViewport* vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(720, 480), ImGuiCond_Appearing);
            if (ImGui::BeginPopupModal("Recent Autosaves##recents", nullptr,
                    ImGuiWindowFlags_NoCollapse)) {
                ImGui::TextUnformatted("Welcome back — recent autosaves");
                ImGui::TextDisabled("UNTITLED / project · time stamp · type · quit saves included");
                ImGui::Separator();

                static std::vector<core::AutosaveEntry> s_recent;
                static bool s_loaded = false;
                static int s_sel = -1;
                static std::unordered_map<std::string, ID3D11ShaderResourceView*> s_prevSrv;
                static std::unordered_map<std::string, ID3D11Texture2D*> s_prevTex;
                if (!s_loaded) {
                    s_recent = core::AutoSaveManager::Get().ListRecent(40);
                    s_loaded = true;
                    s_sel = s_recent.empty() ? -1 : 0;
                }

                ImGui::BeginChild("##recent_list", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.4f), true);
                const float thumb = 72.f;
                for (int i = 0; i < (int)s_recent.size(); ++i) {
                    const auto& e = s_recent[i];
                    ImGui::PushID(i);
                    bool selected = (s_sel == i);

                    // Lazy-load preview PNG once
                    ID3D11ShaderResourceView* srv = nullptr;
                    if (device && !e.previewPath.empty()) {
                        auto it = s_prevSrv.find(e.previewPath);
                        if (it != s_prevSrv.end()) {
                            srv = it->second;
                        } else {
                            std::vector<uint8_t> px;
                            int w = 0, h = 0;
                            if (ImageManager::LoadImageFromFile(e.previewPath, px, w, h) && w > 0 && h > 0) {
                                D3D11_TEXTURE2D_DESC td = {};
                                td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
                                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                                td.SampleDesc.Count = 1;
                                td.Usage = D3D11_USAGE_DEFAULT;
                                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                                D3D11_SUBRESOURCE_DATA init = {};
                                init.pSysMem = px.data();
                                init.SysMemPitch = (UINT)w * 4;
                                ID3D11Texture2D* tex = nullptr;
                                if (SUCCEEDED(device->CreateTexture2D(&td, &init, &tex)) && tex) {
                                    ID3D11ShaderResourceView* nsrv = nullptr;
                                    if (SUCCEEDED(device->CreateShaderResourceView(tex, nullptr, &nsrv)) && nsrv) {
                                        s_prevTex[e.previewPath] = tex;
                                        s_prevSrv[e.previewPath] = nsrv;
                                        srv = nsrv;
                                    } else {
                                        tex->Release();
                                    }
                                }
                            }
                            if (!srv) s_prevSrv[e.previewPath] = nullptr; // mark tried
                        }
                    }

                    if (srv) {
                        ImGui::Image((ImTextureID)srv, ImVec2(thumb, thumb));
                    } else {
                        ImGui::Dummy(ImVec2(thumb, thumb));
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                        dl->AddRectFilled(a, b, IM_COL32(40, 44, 52, 255), 4.f);
                        dl->AddText(ImVec2(a.x + 8, a.y + thumb * 0.4f), IM_COL32(180, 180, 190, 255), "no preview");
                    }
                    ImGui::SameLine();
                    ImGui::BeginGroup();
                    std::string label = e.baseName + "  ·  " + e.projectType +
                        (e.isQuit ? "  ·  quit" : "");
                    if (ImGui::Selectable(label.c_str(), selected, 0, ImVec2(0, thumb * 0.45f)))
                        s_sel = i;
                    // time
                    if (e.mtime > 0) {
                        std::time_t tt = (std::time_t)e.mtime;
                        std::tm tm{};
#ifdef _WIN32
                        localtime_s(&tm, &tt);
#else
                        localtime_r(&tt, &tm);
#endif
                        char tbuf[64];
                        std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm);
                        ImGui::TextDisabled("%s", tbuf);
                    }
                    ImGui::TextDisabled("%s", e.displayName.c_str());
                    ImGui::EndGroup();
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                        s_sel = i;
                        // fall through open
                    }
                    ImGui::Separator();
                    ImGui::PopID();
                }
                if (s_recent.empty()) {
                    ImGui::TextDisabled("No autosaves yet. Paint something — saves every %d min.",
                        ConfigManager::Get().GetAutoSaveIntervalMinutes());
                }
                ImGui::EndChild();

                auto openSelected = [&]() {
                    if (s_sel < 0 || s_sel >= (int)s_recent.size()) return;
                    const std::string path = s_recent[s_sel].raypPath;
                    int id = ProjectManager::Get().ActivateOrPrepareOpen(path);
                    if (id >= 0) {
                        Project* p = ProjectManager::Get().FindProject(id);
                        if (p && p->canvas)
                            TriggerBackgroundOpenDocument(path, device, *p->canvas);
                    }
                    s_loaded = false;
                    ImGui::CloseCurrentPopup();
                };

                if (ImGui::Button("Open selected", ImVec2(140, 0)))
                    openSelected();
                ImGui::SameLine();
                if (ImGui::Button("New blank", ImVec2(120, 0))) {
                    s_loaded = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Refresh list", ImVec2(120, 0))) {
                    s_loaded = false;
                    s_recent = core::AutoSaveManager::Get().ListRecent(40);
                    s_loaded = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Close", ImVec2(100, 0))) {
                    s_loaded = false;
                    ImGui::CloseCurrentPopup();
                }

                // Double-click open
                if (s_sel >= 0 && s_sel < (int)s_recent.size() &&
                    ImGui::IsMouseDoubleClicked(0) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
                    // handled per-row above roughly; keep button primary
                }

                ImGui::EndPopup();
            }
        }

        // Legacy single-file recovery (old autosave_backup.rayp)
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
            if (isVertical) {
                ImGui::SetNextWindowSizeConstraints(ImVec2(36.0f, 100.0f), ImVec2(64.0f, 16384.0f));
            } else {
                ImGui::SetNextWindowSizeConstraints(ImVec2(100.0f, 36.0f), ImVec2(16384.0f, 64.0f));
            }
            Ui::BeginDockPanel("Toolbar", &state.showToolbar);
            // Hard clamp: no overshoot while dragging splitter
            Ui::ClampDockLeafCrossAxis(isVertical, 36.0f, 64.0f);

            ImVec2 avail = ImGui::GetContentRegionAvail();

            // ---- Hotkey-driven grouping: same key+mods → one stack; else separate ----
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
                // Vector group (clear names — workflow: draw shape → Select → Edit)
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

            // Catalog group actions also share a key — fold members onto group key if member unbound
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

            // Build ordered unique slots: first occurrence order of tools, merged by key
            struct Slot {
                KeySig key;
                std::vector<int> toolIdx; // indices into kAllTools
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
            float btnSize = ComputeAdaptiveToolButtonSize(avail, isVertical, nSlots + 1 /* color */, hasSeparator);
            float gap = isVertical ? ImGui::GetStyle().ItemSpacing.y : ImGui::GetStyle().ItemSpacing.x;

            ToolbarBeginLayout(avail, isVertical, nSlots, btnSize, gap, hasSeparator);

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
                    // Fall back to group shortcut string for display
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
                    // Multi-tool stack for shared hotkey
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
                    // Explicit multi-tool marker (stack count + corner chevron)
                    {
                        ImVec2 rmin = ImGui::GetItemRectMin();
                        ImVec2 rmax = ImGui::GetItemRectMax();
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        auto& tok = Ui::Tokens();
                        // Bottom-right fold / chevron
                        float s = std::max(6.f, btnSize * 0.22f);
                        ImVec2 c(rmax.x - 2.f, rmax.y - 2.f);
                        dl->AddTriangleFilled(
                            ImVec2(c.x - s, c.y), ImVec2(c.x, c.y), ImVec2(c.x, c.y - s),
                            tok.ColU32(tok.accent));
                        // Small count badge top-right
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
                    // Keep erase flag consistent when Eraser shares a stack with Brush
                    if (activeTool == ActiveTool::Eraser) brush.erase = true;
                    else if (activeTool == ActiveTool::Brush) brush.erase = false;
                }
            }

            // Floating square outline — jumps tool-to-tool (EaseOutCubic via exp-smooth)
            {
                static ImVec2 s_accMin(0, 0), s_accMax(0, 0);
                static bool s_accInit = false;
                float dt = Ui::DeltaTime();
                if (accentHasTarget) {
                    if (!s_accInit) {
                        s_accMin = accentMin; s_accMax = accentMax; s_accInit = true;
                    } else {
                        // Smooth exp toward target (~EaseOut feel); rate ~14 → snappy jump
                        float k = 1.f - std::exp(-dt * 14.f);
                        s_accMin.x += (accentMin.x - s_accMin.x) * k;
                        s_accMin.y += (accentMin.y - s_accMin.y) * k;
                        s_accMax.x += (accentMax.x - s_accMax.x) * k;
                        s_accMax.y += (accentMax.y - s_accMax.y) * k;
                    }
                    // Slight outward pad so outline floats around the item (not glued to edges)
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

            // Primary / Secondary color chip (replaces Reset View) — X or click swaps with animation
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

                // After color swap: start at crossed positions (t=1) → ease to rest (t=0)
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
                // Secondary behind
                dl->AddRectFilled(b0, ImVec2(b0.x + sq, b0.y + sq),
                    IM_COL32((int)(g_SecondaryColor[0]*255),(int)(g_SecondaryColor[1]*255),
                             (int)(g_SecondaryColor[2]*255),(int)(g_SecondaryColor[3]*255)), tok.rSm);
                dl->AddRect(b0, ImVec2(b0.x + sq, b0.y + sq), tok.ColU32(tok.strokeHairline), tok.rSm, 0, 1.f);
                // Primary in front
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
                if (bound) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            Ui::EndDockPanel();
        }

        // 6a. Viewport Navigation — compact, icon flips, no Reset button (Backspace on hover)
        if (state.showViewportNav) {
            Ui::BeginDockPanel("Viewport Navigation", &state.showViewportNav);
            ImGui::Text("Zoom %.0f%%   ·   Pan (%.0f, %.0f)",
                canvas.GetZoom() * 100.0f, canvas.GetPan().x, canvas.GetPan().y);
            ImGui::Spacing();
            bool flipH = canvas.GetViewportFlipH();
            bool flipV = canvas.GetViewportFlipV();
            if (Ui::IconToggle("##fliph", "ts_mirror_h", &flipH, ImVec2(32, 32),
                    "Flip Horizontal (on)", "Flip Horizontal (off)"))
                canvas.SetViewportFlipH(flipH);
            ImGui::SameLine();
            if (Ui::IconToggle("##flipv", "ts_mirror_v", &flipV, ImVec2(32, 32),
                    "Flip Vertical (on)", "Flip Vertical (off)"))
                canvas.SetViewportFlipV(flipV);
            float rotAngle = canvas.GetRotationAngle() * (180.0f / 3.14159265f);
            if (Ui::SmartSliderFloat("Rotation", &rotAngle, -180.0f, 180.0f, 0.f, 45.f, "%.0f°")) {
                canvas.SetRotationAngle(rotAngle * (3.14159265f / 180.0f));
            }
            ImGui::TextDisabled("Hover slider + Backspace: reset  ·  Ctrl: 45° snap");
            Ui::EndDockPanel();
        }

        // 6b. Properties — project / export only
        if (state.showProperties) {
            Ui::BeginDockPanel("Properties", &state.showProperties);
            
            ImGui::Text("Project Properties:");
            int pType =
                (canvas.GetProjectType() == Canvas::ProjectType::Simple) ? 0 :
                (canvas.GetProjectType() == Canvas::ProjectType::AdvancedModMode) ? 2 : 1;
            const char* pTypeNames[] = {
                "Simple Project",
                "Advanced Project (.rayp)",
                "Advanced Mod Mode (.rayp)"
            };
            if (UiCombo("##cmb_pType", &pType, pTypeNames, IM_ARRAYSIZE(pTypeNames), "Project Type")) {
                if (pType == 0) canvas.SetProjectType(Canvas::ProjectType::Simple);
                else if (pType == 2) canvas.SetProjectType(Canvas::ProjectType::AdvancedModMode);
                else canvas.SetProjectType(Canvas::ProjectType::Advanced);
            }

            ImGui::Text("Document Bit Depth:");
            int bd = (int)canvas.GetDocumentBitDepth();
            const char* bdNames[] = {
                "8-bit (U8) — default / diffuse",
                "16-bit float (F16) — HDR mid",
                "32-bit float (F32) — height / full float"
            };
            if (UiCombo("##cmb_bitDepth", &bd, bdNames, IM_ARRAYSIZE(bdNames),
                    "Working space for paint storage. Export format stays free.")) {
                canvas.SetDocumentBitDepth(static_cast<Canvas::DocumentBitDepth>(bd));
            }
            ImGui::TextDisabled("Canvas %d x %d · storage %d B/px",
                canvas.GetWidth(), canvas.GetHeight(),
                BytesPerPixel(Canvas::FormatForBitDepth(canvas.GetDocumentBitDepth())));

            char propProjPath[512] = "";
            std::strncpy(propProjPath, canvas.GetCurrentProjectFilePath().c_str(), sizeof(propProjPath));
            if (Ui::PathField("##projpath", "Project Path", propProjPath, sizeof(propProjPath),
                    ShowOpenFileWin32, "RayP Projects (*.rayp)\0*.rayp\0All Files (*.*)\0*.*\0")) {
                canvas.SetCurrentProjectFilePath(propProjPath);
            }

            // Advanced Mod Mode — setup lives in dedicated window (not Properties)
            if (canvas.GetProjectType() == Canvas::ProjectType::AdvancedModMode) {
                ImGui::NewLine();
                ImGui::Separator();
                ImGui::Text("Advanced Mod");
                if (ImGui::Button("Mod Setup…##prop_mod"))
                    state.showModSetup = true;
                ImGui::SameLine();
                if (ImGui::Button("3D Preview##prop_3d")) {
                    state.showPreview3D = true;
                    state.preview3DNeedReload = true;
                }
                if (canvas.IsModParseOk())
                    ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.f), "INI parsed OK");
                else
                    ImGui::TextDisabled("INI not applied — open Mod Setup");
            }

            ImGui::NewLine();
            ImGui::Separator();
            ImGui::Text("Project Output Format (DDS ↔ PNG):");
            ImGui::TextDisabled("Hard container switch — batch + quick export both use this.");

            char propExportPath[512] = "";
            std::strncpy(propExportPath, canvas.GetExportPath().c_str(), sizeof(propExportPath));

            int outFmt = (canvas.GetExportContainer() == Canvas::ExportContainer::DDS) ? 1 : 0;
            const char* outFmtNames[] = { "PNG / Standard image", "DDS (compressed)" };
            if (UiCombo("##cmb_outFmt", &outFmt, outFmtNames, IM_ARRAYSIZE(outFmtNames), "Output Type")) {
                canvas.SetExportContainer(outFmt == 1
                    ? Canvas::ExportContainer::DDS
                    : Canvas::ExportContainer::PNG);
                std::string synced = canvas.GetExportPath();
                std::strncpy(propExportPath, synced.c_str(), sizeof(propExportPath) - 1);
                propExportPath[sizeof(propExportPath) - 1] = '\0';
            }

            if (Ui::PathField("##exppath", "Export Path", propExportPath, sizeof(propExportPath),
                    ShowSaveFileWin32, "PNG (*.png)\0*.png\0DDS (*.dds)\0*.dds\0All Files (*.*)\0*.*\0")
                || std::string(propExportPath) != canvas.GetExportPath()) {
                canvas.SetExportPath(propExportPath);
                // Infer container from path extension when user picks a file
                std::string pathStr = propExportPath;
                size_t d = pathStr.find_last_of('.');
                if (d != std::string::npos) {
                    std::string e = pathStr.substr(d + 1);
                    std::transform(e.begin(), e.end(), e.begin(), ::tolower);
                    if (e == "dds")
                        canvas.SetExportContainer(Canvas::ExportContainer::DDS);
                    else if (e == "png")
                        canvas.SetExportContainer(Canvas::ExportContainer::PNG);
                }
                canvas.SyncExportPathExtension();
                std::string synced = canvas.GetExportPath();
                std::strncpy(propExportPath, synced.c_str(), sizeof(propExportPath) - 1);
                propExportPath[sizeof(propExportPath) - 1] = '\0';
            }

            if (canvas.GetExportContainer() == Canvas::ExportContainer::DDS) {
                static const char* formats[] = {
                    "BC7 (sRGB, DX 11+)", "BC7 (Linear, DX 11+)",
                    "BC3 (Linear, DXT5)", "BC1 (Linear, DXT1)",
                    "BC5 (Linear, Unsigned)", "R8G8B8A8 (Linear, A8B8G8R8)",
                    "RGBA16_FLOAT", "RGBA32_FLOAT", "R32 (Linear, Float)"
                };
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
                const char* speeds[] = { "Fast", "Medium", "Slow", "Best" };
                int si = 1;
                std::string cs = canvas.GetExportCompressionSpeed();
                for (int i = 0; i < 4; ++i) if (cs == speeds[i]) si = i;
                if (UiCombo("##cmb_compSpeed", &si, speeds, 4, "Quality"))
                    canvas.SetExportCompressionSpeed(speeds[si]);
            } else {
                DrawIccPresetCombo(canvas, "ICC Profile");
            }

            if (ImGui::Button("Quick Export (project format)", ImVec2(-1, 0))) {
                state.openQuickExportTrigger = true;
            }
            if (ImGui::IsItemHovered()) Ui::Tooltip("Export using the path/format above (same as Ctrl+E)");

            Ui::EndDockPanel();
        }

        UI::DrawLayersPanel(state, canvas, device);
        UI::DrawAssetBrowserPanel(state, canvas, device);
        Ui::DrawAssetPicker(device);

        // Layer Effects modal (extracted)
        UI::DrawLayerEffectsPanel(state, canvas, device);


        UI::DrawChannelsPanel(state, canvas, device);

        // Project Setup (extracted)
        UI::DrawProjectSetupPanel(state, canvas, device);


        // File Explorer / project wizard (always non-modal floating window)
        if (state.openNewProjectWizard) {
            state.openNewProjectWizard = false;
            UI::FileExplorerOpen(state.fileExplorer, UI::FileExplorerMode::ProjectCreate);
        }
        if (state.fileExplorer.open) {
            Project* p = ProjectManager::Get().ActiveProject();
            UI::DrawFileExplorer(state.fileExplorer, p, canvas, device);
        }

        // 8. Tool Settings strip (extracted)
        UI::DrawToolSettingsPanel(state, canvas, brush, activeTool, device);

        // 9. Draw Logging Console Panel (selectable / copyable)
        if (state.showConsole) {
            Ui::BeginDockPanel("Console Logs", &state.showConsole);
            auto logs = Logger::Get().GetRecentLogs();
            if (ImGui::Button("Clear")) {
                Logger::Get().ClearRecentLogs();
            }
            ImGui::SameLine();
            if (ImGui::Button("Copy All")) {
                std::string joined;
                joined.reserve(logs.size() * 80);
                for (size_t i = 0; i < logs.size(); ++i) {
                    if (i) joined.push_back('\n');
                    joined += logs[i];
                }
                ImGui::SetClipboardText(joined.c_str());
            }
            if (ImGui::IsItemHovered())
                Ui::Tooltip("Copy entire console to clipboard");
            ImGui::SameLine();
            ImGui::TextDisabled("Click line = copy · multi-select with Ctrl");
            ImGui::Separator();
            ImGui::BeginChild("LogScrollRegion", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
            static std::vector<int> s_logSelected;
            // prune selection indices if log shrank
            s_logSelected.erase(
                std::remove_if(s_logSelected.begin(), s_logSelected.end(),
                    [&](int i) { return i < 0 || i >= (int)logs.size(); }),
                s_logSelected.end());

            ImGuiListClipper clipper;
            clipper.Begin((int)logs.size());
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const auto& log = logs[(size_t)i];
                    ImVec4 col(0.9f, 0.9f, 0.92f, 1.f);
                    if (log.find("[ERROR]") != std::string::npos)
                        col = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
                    else if (log.find("[WARN ]") != std::string::npos)
                        col = ImVec4(1.0f, 0.8f, 0.4f, 1.0f);
                    else if (log.find("[DEBUG]") != std::string::npos)
                        col = ImVec4(0.6f, 0.6f, 0.8f, 1.0f);

                    bool selected = std::find(s_logSelected.begin(), s_logSelected.end(), i) != s_logSelected.end();
                    ImGui::PushStyleColor(ImGuiCol_Text, col);
                    ImGui::PushID(i);
                    if (ImGui::Selectable(log.c_str(), selected,
                            ImGuiSelectableFlags_AllowDoubleClick)) {
                        const bool ctrl = ImGui::GetIO().KeyCtrl;
                        if (!ctrl)
                            s_logSelected.clear();
                        if (selected && ctrl) {
                            s_logSelected.erase(
                                std::remove(s_logSelected.begin(), s_logSelected.end(), i),
                                s_logSelected.end());
                        } else if (!selected) {
                            s_logSelected.push_back(i);
                        }
                        // Always copy current selection (or just this line)
                        std::string clip;
                        if (s_logSelected.empty()) {
                            clip = log;
                        } else {
                            std::vector<int> sorted = s_logSelected;
                            std::sort(sorted.begin(), sorted.end());
                            for (size_t k = 0; k < sorted.size(); ++k) {
                                if (k) clip.push_back('\n');
                                int idx = sorted[k];
                                if (idx >= 0 && idx < (int)logs.size())
                                    clip += logs[(size_t)idx];
                            }
                        }
                        ImGui::SetClipboardText(clip.c_str());
                    }
                    if (ImGui::BeginPopupContextItem("##logctx")) {
                        if (ImGui::MenuItem("Copy line"))
                            ImGui::SetClipboardText(log.c_str());
                        if (ImGui::MenuItem("Copy selection") && !s_logSelected.empty()) {
                            std::vector<int> sorted = s_logSelected;
                            std::sort(sorted.begin(), sorted.end());
                            std::string clip;
                            for (size_t k = 0; k < sorted.size(); ++k) {
                                if (k) clip.push_back('\n');
                                int idx = sorted[k];
                                if (idx >= 0 && idx < (int)logs.size())
                                    clip += logs[(size_t)idx];
                            }
                            ImGui::SetClipboardText(clip.c_str());
                        }
                        if (ImGui::MenuItem("Copy all")) {
                            std::string joined;
                            for (size_t k = 0; k < logs.size(); ++k) {
                                if (k) joined.push_back('\n');
                                joined += logs[k];
                            }
                            ImGui::SetClipboardText(joined.c_str());
                        }
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                    ImGui::PopStyleColor();
                }
            }
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.f) {
                ImGui::SetScrollHereY(1.0f);
            }
            ImGui::EndChild();
            Ui::EndDockPanel();
        }

        // 10. Colors (extracted)
        UI::DrawColorsPanel(state, canvas, brush);

        // About modal
        if (state.openAboutModal)
            ImGui::OpenPopup("About RayV-Paint##about");
        {
            ImGuiViewport* avp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(avp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(480, 420), ImGuiCond_Appearing);
            if (ImGui::BeginPopupModal("About RayV-Paint##about", &state.openAboutModal, ImGuiWindowFlags_NoResize)) {
                static ID3D11ShaderResourceView* s_MascotSrv = nullptr;
                static int s_MascotW = 0, s_MascotH = 0;
                static bool s_MascotTried = false;
                if (!s_MascotTried && device) {
                    s_MascotTried = true;
                    const char* paths[] = {
                        "testfield/ray_chan_kissing.png",
                        "../testfield/ray_chan_kissing.png",
                        "../../testfield/ray_chan_kissing.png",
                        "C:/Users/Rayvy/Documents/GitHub/RayV-Paint/testfield/ray_chan_kissing.png"
                    };
                    for (const char* p : paths) {
                        int w = 0, h = 0, n = 0;
                        unsigned char* px = stbi_load(p, &w, &h, &n, 4);
                        if (!px) continue;
                        D3D11_TEXTURE2D_DESC desc = {};
                        desc.Width = w; desc.Height = h; desc.MipLevels = 1; desc.ArraySize = 1;
                        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                        desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_IMMUTABLE;
                        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                        D3D11_SUBRESOURCE_DATA srd = {};
                        srd.pSysMem = px; srd.SysMemPitch = w * 4;
                        ID3D11Texture2D* tex = nullptr;
                        if (SUCCEEDED(device->CreateTexture2D(&desc, &srd, &tex))) {
                            device->CreateShaderResourceView(tex, nullptr, &s_MascotSrv);
                            tex->Release();
                            s_MascotW = w; s_MascotH = h;
                        }
                        stbi_image_free(px);
                        break;
                    }
                }
                ImGui::Text("RayV-Paint");
                ImGui::TextDisabled("by Rayvich");
                ImGui::Separator();
                if (s_MascotSrv) {
                    float maxW = 200.f;
                    float sc = maxW / (float)std::max(1, s_MascotW);
                    ImGui::Image((ImTextureID)s_MascotSrv, ImVec2(s_MascotW * sc, s_MascotH * sc));
                }
                ImGui::Spacing();
                ImGui::TextWrapped("Bla bla Bla. Better look at this, RayChan is kissing!");
                ImGui::Spacing();
                if (ImGui::Button("Close", ImVec2(120, 0))) {
                    state.openAboutModal = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }

        // ---- Mod Setup (INI / dump / semantics) — separate from Properties ----
        if (state.showModSetup) {
            ImGui::SetNextWindowSize(ImVec2(520, 640), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Mod Setup", &state.showModSetup)) {
                ImGui::TextDisabled("XXMI / 3DMigoto sources for optional 3D preview. Paint works without this.");

                char iniPath[512] = "";
                std::strncpy(iniPath, canvas.GetModIniPath().c_str(), sizeof(iniPath) - 1);
                if (Ui::PathField("##mod_ini", "INI Path", iniPath, sizeof(iniPath),
                        ShowOpenFileWin32, "INI Files (*.ini)\0*.ini\0All Files (*.*)\0*.*\0")) {
                    canvas.SetModIniPath(iniPath);
                }
                char dumpPath[512] = "";
                std::strncpy(dumpPath, canvas.GetModDumpPath().c_str(), sizeof(dumpPath) - 1);
                if (Ui::PathField("##mod_dump", "Dump Path", dumpPath, sizeof(dumpPath),
                        ShowOpenFileWin32, "All Files (*.*)\0*.*\0")) {
                    canvas.SetModDumpPath(dumpPath);
                }

                if (ImGui::Button("Apply INI##mod_apply", ImVec2(120, 0))) {
                    canvas.ApplyModIniParse();
                    state.preview3DNeedReload = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Apply Dump##mod_dump", ImVec2(120, 0))) {
                    canvas.ApplyModDumpParse();
                    state.preview3DNeedReload = true;
                }
                ImGui::SameLine();
                if (canvas.IsModParseOk())
                    ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.f), "OK");
                else if (!canvas.GetModParseSummary().empty())
                    ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.3f, 1.f), "Issues");

                if (ImGui::Button("Open 3D Preview##mod_3d")) {
                    state.showPreview3D = true;
                    state.preview3DNeedReload = true;
                }

                auto& sceneMut = canvas.GetModScene();
                const auto& scene = sceneMut;
                if (scene.ok || !scene.components.empty()) {
                    ImGui::Separator();
                    ImGui::Text("Components: %d  Parts: %d  Draws: %d  Binds: %d",
                        (int)scene.components.size(), scene.PartCount(),
                        scene.DrawCount(), scene.TextureBindCount());

                    if (ImGui::TreeNode("Vertex semantics (roles)##mod_sem")) {
                        ImGui::TextDisabled("TEXCOORD ≠ always UV. Role=None ignores attribute.");
                        int roleCount = 0;
                        const char* const* roleNames = modio::AttrRoleNameTable(roleCount);
                        auto drawLayoutEditor = [&](const char* label, modio::BufferLayout& layout) {
                            if (!ImGui::TreeNode(label)) return;
                            ImGui::Text("stride=%d %s", layout.stride, layout.valid ? "valid" : "invalid");
                            if (ImGui::BeginTable("##lay", 4,
                                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                                ImGui::TableSetupColumn("Dump");
                                ImGui::TableSetupColumn("Fmt");
                                ImGui::TableSetupColumn("Off", ImGuiTableColumnFlags_WidthFixed, 36);
                                ImGui::TableSetupColumn("Role");
                                ImGui::TableHeadersRow();
                                for (auto& el : layout.elements) {
                                    ImGui::TableNextRow();
                                    ImGui::TableNextColumn();
                                    if (el.dumpSemanticIndex > 0)
                                        ImGui::Text("%s%d", el.dumpSemanticName.c_str(), el.dumpSemanticIndex);
                                    else
                                        ImGui::TextUnformatted(el.dumpSemanticName.c_str());
                                    ImGui::TableNextColumn();
                                    ImGui::TextUnformatted(modio::AttrFormatName(el.format));
                                    ImGui::TableNextColumn();
                                    ImGui::Text("%d", el.offset);
                                    ImGui::TableNextColumn();
                                    int role = (int)el.role;
                                    ImGui::PushID(el.index + layout.stride * 100 + (int)layout.kind * 10000);
                                    if (UiCombo("##role", &role, roleNames, roleCount)) {
                                        modio::SetElementRole(layout, el.index, static_cast<modio::AttrRole>(role));
                                        canvas.SetDocumentModified(true);
                                        state.preview3DNeedReload = true;
                                    }
                                    ImGui::PopID();
                                }
                                ImGui::EndTable();
                            }
                            ImGui::TreePop();
                        };
                        for (auto& c : sceneMut.components) {
                            if (ImGui::TreeNode(c.name.c_str())) {
                                drawLayoutEditor("Position", c.positionLayout);
                                drawLayoutEditor("Texcoord", c.texcoordLayout);
                                ImGui::TreePop();
                            }
                        }
                        ImGui::TreePop();
                    }

                    if (ImGui::TreeNode("Component / batches / draws##mod_tree")) {
                        auto& sceneEdit = canvas.GetModScene();
                        for (auto& c : sceneEdit.components) {
                            if (ImGui::TreeNode(c.name.c_str())) {
                                ImGui::Checkbox("Component visible", &c.visible);
                                for (auto& p : c.parts) {
                                    if (ImGui::TreeNode((p.name + "##" + p.sectionName).c_str())) {
                                        ImGui::Checkbox("Part visible", &p.visible);
                                        ImGui::TextDisabled("IB %s  batches=%d draws=%d",
                                            p.ibResource.c_str(), (int)p.batches.size(), p.TotalDraws());
                                        for (int bi = 0; bi < (int)p.batches.size(); ++bi) {
                                            auto& bat = p.batches[bi];
                                            ImGui::PushID(bi);
                                            if (ImGui::TreeNode(bat.name.c_str())) {
                                                if (ImGui::Checkbox("Batch visible", &bat.visible))
                                                    state.preview3DNeedReload = true;
                                                for (const auto& t : bat.textures) {
                                                    ImGui::BulletText("%s → %s%s",
                                                        modio::MaterialSlotName(t.slot),
                                                        t.resourceName.c_str(),
                                                        t.exists ? "" : " [missing]");
                                                }
                                                for (int di = 0; di < (int)bat.draws.size(); ++di) {
                                                    auto& d = bat.draws[di];
                                                    ImGui::PushID(di);
                                                    char lab[256];
                                                    std::snprintf(lab, sizeof(lab),
                                                        "draw %d @%d%s", d.indexCount, d.indexStart,
                                                        d.commentLabel.empty() ? "" : (" ; " + d.commentLabel).c_str());
                                                    if (ImGui::Checkbox(lab, &d.visible))
                                                        state.preview3DNeedReload = true;
                                                    ImGui::PopID();
                                                }
                                                ImGui::TreePop();
                                            }
                                            ImGui::PopID();
                                        }
                                        ImGui::TreePop();
                                    }
                                }
                                ImGui::TreePop();
                            }
                        }
                        if (ImGui::Button("Reload 3D after visibility##mod_vis"))
                            state.preview3DNeedReload = true;
                        ImGui::TreePop();
                    }
                    if (!scene.warnings.empty() && ImGui::TreeNode("Warnings##mod_warn")) {
                        for (const auto& w : scene.warnings)
                            ImGui::TextWrapped("%s", w.message.c_str());
                        ImGui::TreePop();
                    }
                } else if (!canvas.GetModParseSummary().empty()) {
                    ImGui::TextWrapped("%s", canvas.GetModParseSummary().c_str());
                }
            }
            ImGui::End();
        }

        // ---- 3D Preview: viewport-first + Blender-style N-panel ----
        if (state.showPreview3D) {
            static preview3d::PreviewRenderer s_Preview;
            static bool s_PreviewInit = false;
            static ID3D11Texture2D* s_RT = nullptr;
            static ID3D11RenderTargetView* s_RTV = nullptr;
            static ID3D11ShaderResourceView* s_SRV = nullptr;
            static ID3D11Texture2D* s_Depth = nullptr;
            static ID3D11DepthStencilView* s_DSV = nullptr;
            static int s_RTw = 0, s_RTh = 0;
            static bool s_VpCapture = false;   // block window drag while using viewport
            static int s_NPanel = 0;           // 0=collapsed strip, 1=Light 2=Shade 3=Orient 4=Parts 5=Debug
            static int s_SelPart = 0;
            static float s_NBodyW = 380.f;     // resizable N-panel content width
            static bool s_Splitting = false;

            if (!s_PreviewInit && device)
                s_PreviewInit = s_Preview.Initialize(device);
            if (state.preview3DNeedReload && device && canvas.IsModParseOk()) {
                s_Preview.LoadScene(device, canvas.GetModScene());
                state.preview3DNeedReload = false;
            }

            ImGuiWindowFlags winFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
            if (s_VpCapture || s_Splitting)
                winFlags |= ImGuiWindowFlags_NoMove;

            ImGui::SetNextWindowSize(ImVec2(1100, 700), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("3D Preview", &state.showPreview3D, winFlags)) {
                // HOME = reset view when this window focused
                if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                    ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
                    s_Preview.ResetView();
                }

                const float nTabW = 32.f;
                const float splitW = 6.f;
                s_NBodyW = std::clamp(s_NBodyW, 220.f, 700.f);
                const float nPanelW = (s_NPanel > 0) ? (nTabW + s_NBodyW) : nTabW;
                ImVec2 avail = ImGui::GetContentRegionAvail();
                float splitSpace = (s_NPanel > 0) ? splitW : 2.f;
                float vpW = std::max(64.f, avail.x - nPanelW - splitSpace);
                float vpH = std::max(64.f, avail.y);

                // ===== Viewport (left) =====
                ImGui::BeginChild("##p3d_vp", ImVec2(vpW, vpH), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

                int tw = (int)vpW;
                int th = (int)vpH;
                auto releaseRT = [&]() {
                    if (s_SRV) { s_SRV->Release(); s_SRV = nullptr; }
                    if (s_RTV) { s_RTV->Release(); s_RTV = nullptr; }
                    if (s_RT) { s_RT->Release(); s_RT = nullptr; }
                    if (s_DSV) { s_DSV->Release(); s_DSV = nullptr; }
                    if (s_Depth) { s_Depth->Release(); s_Depth = nullptr; }
                    s_RTw = s_RTh = 0;
                };
                if (device && (tw != s_RTw || th != s_RTh || !s_RTV)) {
                    releaseRT();
                    D3D11_TEXTURE2D_DESC td{};
                    td.Width = tw; td.Height = th;
                    td.MipLevels = td.ArraySize = 1;
                    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    td.SampleDesc.Count = 1;
                    td.Usage = D3D11_USAGE_DEFAULT;
                    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                    if (SUCCEEDED(device->CreateTexture2D(&td, nullptr, &s_RT))) {
                        device->CreateRenderTargetView(s_RT, nullptr, &s_RTV);
                        device->CreateShaderResourceView(s_RT, nullptr, &s_SRV);
                    }
                    D3D11_TEXTURE2D_DESC dd = td;
                    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
                    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
                    if (SUCCEEDED(device->CreateTexture2D(&dd, nullptr, &s_Depth)))
                        device->CreateDepthStencilView(s_Depth, nullptr, &s_DSV);
                    s_RTw = tw; s_RTh = th;
                }

                ImVec2 vpPos = ImGui::GetCursorScreenPos();
                if (s_SRV && s_RTV && context) {
                    D3D11_VIEWPORT vp{};
                    vp.Width = (float)s_RTw; vp.Height = (float)s_RTh; vp.MaxDepth = 1.f;
                    context->RSSetViewports(1, &vp);
                    s_Preview.Render(context, s_RTV, s_DSV, (float)s_RTw / (float)std::max(1, s_RTh));

                    // Invisible button captures mouse so window does not drag (like 2D canvas)
                    ImGui::InvisibleButton("##p3d_hit", ImVec2((float)s_RTw, (float)s_RTh));
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    dl->AddImage((ImTextureID)s_SRV, vpPos,
                        ImVec2(vpPos.x + s_RTw, vpPos.y + s_RTh));

                    bool hovered = ImGui::IsItemHovered();
                    bool active = ImGui::IsItemActive();
                    s_VpCapture = active || (hovered && (
                        ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
                        ImGui::IsMouseDown(ImGuiMouseButton_Middle)));

                    ImGuiIO& io = ImGui::GetIO();
                    auto& cam = s_Preview.Camera();
                    if (active || hovered) {
                        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) &&
                            !ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
                            cam.yaw += io.MouseDelta.x * 0.01f;
                            cam.pitch += io.MouseDelta.y * 0.01f;
                            cam.pitch = std::clamp(cam.pitch, -1.45f, 1.45f);
                        }
                        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
                            cam.PanScreen(io.MouseDelta.x, io.MouseDelta.y, (float)s_RTh);
                        }
                        if (hovered && io.MouseWheel != 0.f) {
                            cam.distance *= (io.MouseWheel > 0 ? 0.9f : 1.1f);
                            cam.distance = std::clamp(cam.distance, 0.25f, 80.f);
                        }
                    }

                    // Floating Reset View (top-left of viewport)
                    ImGui::SetCursorScreenPos(ImVec2(vpPos.x + 8.f, vpPos.y + 8.f));
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.12f, 0.14f, 0.75f));
                    if (ImGui::Button("Reset View##p3d_rst"))
                        s_Preview.ResetView();
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered())
                        Ui::Tooltip("Reset camera (Home)");
                } else {
                    ImGui::Dummy(ImVec2(vpW, vpH));
                    ImGui::SetCursorScreenPos(ImVec2(vpPos.x + 16, vpPos.y + 16));
                    ImGui::TextDisabled("Apply INI in Mod Setup, then Reload in N-panel.");
                    s_VpCapture = false;
                }
                ImGui::EndChild();

                ImGui::SameLine(0, 0);

                // ===== Splitter (drag to resize N-panel) =====
                if (s_NPanel > 0) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.25f, 0.28f, 0.6f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.4f, 0.55f, 0.9f, 0.8f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.35f, 0.5f, 0.95f, 1.f));
                    ImGui::Button("##p3d_split", ImVec2(splitW, vpH));
                    if (ImGui::IsItemActive() || ImGui::IsItemHovered())
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                    if (ImGui::IsItemActive()) {
                        s_Splitting = true;
                        // Dragging left increases N-panel, right shrinks it
                        s_NBodyW = std::clamp(s_NBodyW - ImGui::GetIO().MouseDelta.x, 220.f, 700.f);
                    } else {
                        s_Splitting = false;
                    }
                    ImGui::PopStyleColor(3);
                    if (ImGui::IsItemHovered())
                        Ui::Tooltip("Drag to resize N-panel");
                    ImGui::SameLine(0, 0);
                } else {
                    ImGui::SameLine(0, 2);
                    s_Splitting = false;
                }

                // ===== N-panel (right) =====
                ImGui::BeginChild("##p3d_n", ImVec2(nPanelW, vpH), true,
                    ImGuiWindowFlags_None); // allow scroll inside panel

                auto nTab = [&](int id, const char* letter, const char* tip) {
                    bool on = (s_NPanel == id);
                    if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                    if (ImGui::Button(letter, ImVec2(nTabW - 6, 28)))
                        s_NPanel = on ? 0 : id;
                    if (on) ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) Ui::Tooltip(tip);
                };

                if (s_NPanel == 0) {
                    // Vertical tab strip only
                    nTab(1, "L", "Lighting");
                    nTab(2, "S", "Shading / presets");
                    nTab(3, "O", "Orientation (up axis / flip)");
                    nTab(4, "P", "Parts visibility");
                    nTab(5, "D", "Debug / reload");
                } else {
                    // Tab strip + content
                    ImGui::BeginGroup();
                    nTab(1, "L", "Lighting");
                    nTab(2, "S", "Shading");
                    nTab(3, "O", "Orientation");
                    nTab(4, "P", "Parts");
                    nTab(5, "D", "Debug");
                    if (ImGui::Button("«##np", ImVec2(nTabW - 6, 22)))
                        s_NPanel = 0;
                    if (ImGui::IsItemHovered()) Ui::Tooltip("Collapse N-panel");
                    ImGui::EndGroup();
                    ImGui::SameLine();
                    ImGui::BeginChild("##p3d_nbody", ImVec2(0, 0), false);

                    if (s_NPanel == 1) {
                        ImGui::TextUnformatted("Lighting");
                        auto& L = s_Preview.Lighting();
                        float yawDeg = L.yaw * (180.f / 3.14159265f);
                        float pitchDeg = L.pitch * (180.f / 3.14159265f);
                        if (Ui::SmartSliderFloat("Yaw", &yawDeg, -180.f, 180.f, 0.f, 1.f, "%.0f°"))
                            L.yaw = yawDeg * (3.14159265f / 180.f);
                        if (Ui::SmartSliderFloat("Pitch", &pitchDeg, -89.f, 89.f, 0.f, 1.f, "%.0f°"))
                            L.pitch = pitchDeg * (3.14159265f / 180.f);
                        Ui::SmartSliderFloat("Intensity", &L.intensity, 0.f, 2.f, 1.f, 0.05f, "%.2f");
                        Ui::SmartSliderFloat("Ambient", &L.ambient, 0.f, 1.f, 0.3f, 0.05f, "%.2f");
                        ImGui::Checkbox("Follow camera", &L.followCamera);
                        if (ImGui::Button("Left")) { L.yaw = -0.9f; L.pitch = 0.4f; }
                        ImGui::SameLine();
                        if (ImGui::Button("Front")) { L.yaw = 0.f; L.pitch = 0.35f; }
                        ImGui::SameLine();
                        if (ImGui::Button("Right")) { L.yaw = 0.9f; L.pitch = 0.4f; }
                    } else if (s_NPanel == 2) {
                        ImGui::TextUnformatted("Shading");
                        ImGui::TextDisabled("Uber shader · channel remaps");
                        auto& lib = preview3d::ShaderPresetLibrary::Get();
                        if (lib.All().empty()) lib.LoadBuiltins();
                        auto& items = s_Preview.Items();
                        if (!items.empty()) {
                            s_SelPart = std::clamp(s_SelPart, 0, (int)items.size() - 1);
                            if (ImGui::BeginCombo("Part",
                                    (items[s_SelPart].componentName + "/" + items[s_SelPart].partName).c_str())) {
                                for (int i = 0; i < (int)items.size(); ++i) {
                                    bool sel = (i == s_SelPart);
                                    if (ImGui::Selectable((items[i].componentName + "/" + items[i].partName).c_str(), sel))
                                        s_SelPart = i;
                                }
                                ImGui::EndCombo();
                            }
                            int presetIdx = lib.IndexOf(items[s_SelPart].presetId);
                            if (ImGui::BeginCombo("Preset", lib.At(presetIdx).displayName.c_str())) {
                                for (int i = 0; i < (int)lib.All().size(); ++i) {
                                    if (ImGui::Selectable(lib.At(i).displayName.c_str(), i == presetIdx))
                                        s_Preview.ApplyPresetToPart(s_SelPart, lib.At(i));
                                }
                                ImGui::EndCombo();
                            }
                            if (ImGui::Button("Apply to all parts"))
                                s_Preview.ApplyPresetToAll(items[s_SelPart].material);
                            ImGui::SameLine();
                            if (ImGui::Button("Save preset JSON")) {
                                std::string dir = ConfigManager::GetUserSubdirectory("presets/shaders");
                                lib.SavePreset(items[s_SelPart].material, dir);
                            }

                            // Bind diagnostics — verify multi-texture paths
                            if (ImGui::TreeNode("Bound textures (paths)##binds")) {
                                const char* slotN[] = { "Diffuse", "Normal", "LightMap", "Material" };
                                auto& it = items[s_SelPart];
                                for (int mi = 0; mi < 4; ++mi) {
                                    bool ok = !it.paths[mi].empty();
                                    // basename of path
                                    std::string base = it.paths[mi];
                                    size_t sl = base.find_last_of("/\\");
                                    if (sl != std::string::npos) base = base.substr(sl + 1);
                                    ImGui::TextColored(
                                        ok ? ImVec4(0.4f, 0.9f, 0.5f, 1.f) : ImVec4(0.95f, 0.45f, 0.3f, 1.f),
                                        "%s:", slotN[mi]);
                                    ImGui::SameLine();
                                    ImGui::TextWrapped("%s\n  file: %s",
                                        it.resNames[mi].empty() ? "?" : it.resNames[mi].c_str(),
                                        ok ? base.c_str() : "(missing — grey fallback)");
                                }
                                ImGui::TextDisabled(
                                    "LightMap debug uses UV0 now (not UV2).\n"
                                    "If still wrong: check basename matches expected DDS.\n"
                                    "ZZZ: LM R=Shadow G=Metal B=Gloss | Mat R=Opac B=Spec | N.B=AO");
                                ImGui::TreePop();
                            }

                            auto& mat = items[s_SelPart].material;
                            Ui::SmartSliderFloat("Toon thr", &mat.toonThreshold, 0.f, 1.f, 0.5f, 0.05f, "%.2f");
                            Ui::SmartSliderFloat("Toon soft", &mat.toonSoftness, 0.01f, 0.5f, 0.1f, 0.01f, "%.2f");
                            Ui::SmartSliderFloat("Rim", &mat.rimStrength, 0.f, 1.5f, 0.5f, 0.05f, "%.2f");
                            Ui::SmartSliderFloat("SSS", &mat.sssStrength, 0.f, 1.f, 0.f, 0.05f, "%.2f");
                            ImGui::Checkbox("Normal map", &mat.useNormalMap);
                            ImGui::SameLine();
                            ImGui::Checkbox("Normal RG only", &mat.normalRGOnly);
                            ImGui::Checkbox("Alpha clip (Material.R)", &mat.alphaClip);
                            if (ImGui::TreeNode("Channel remap##ch")) {
                                ImGui::TextDisabled("If look is wrong — remap first, then blame shader.");
                                auto editCh = [](const char* label, preview3d::ChannelSource& ch) {
                                    ImGui::PushID(label);
                                    ImGui::AlignTextToFramePadding();
                                    ImGui::TextUnformatted(label);
                                    ImGui::SameLine(100);
                                    int map = (ch.map == preview3d::MapSet::Constant) ? 4 : (int)ch.map;
                                    const char* maps[] = { "Diffuse", "Normal", "LightMap", "MaterialMap", "Const" };
                                    ImGui::SetNextItemWidth(110);
                                    if (UiCombo("##m", &map, maps, 5))
                                        ch.map = (map >= 4) ? preview3d::MapSet::Constant
                                                            : static_cast<preview3d::MapSet>(map);
                                    ImGui::SameLine();
                                    int sw = std::clamp((int)ch.swizzle, 0, 6);
                                    const char* swz[] = { "R", "G", "B", "A", "Luma", "1", "0" };
                                    ImGui::SetNextItemWidth(64);
                                    if (UiCombo("##s", &sw, swz, 7))
                                        ch.swizzle = static_cast<preview3d::ChanSwizzle>(sw);
                                    ImGui::SameLine();
                                    ImGui::Checkbox("inv", &ch.invert);
                                    if (ch.map == preview3d::MapSet::Constant) {
                                        ImGui::SameLine();
                                        ImGui::SetNextItemWidth(56);
                                        ImGui::DragFloat("##c", &ch.constantValue, 0.01f, 0.f, 1.f);
                                    }
                                    ImGui::PopID();
                                };
                                editCh("Shadow", mat.shadowMask);
                                editCh("Metallic", mat.metallic);
                                editCh("Rough/Gloss", mat.roughness);
                                editCh("Specular", mat.specular);
                                editCh("AO", mat.ao);
                                editCh("Opacity", mat.opacity);
                                editCh("Aniso", mat.anisotropy);
                                editCh("SSS", mat.sssMask);
                                editCh("Glow", mat.glow);
                                ImGui::TreePop();
                            }
                        } else {
                            ImGui::TextDisabled("No mesh — Apply INI + Reload");
                        }
                    } else if (s_NPanel == 3) {
                        ImGui::TextUnformatted("Orientation");
                        ImGui::TextDisabled("ZZZ dumps often need Up = +Z");
                        auto& O = s_Preview.Orientation();
                        int up = (int)O.upAxis;
                        const char* ups[] = { "+Y", "-Y", "+Z", "-Z", "+X", "-X" };
                        if (UiCombo("##up_axis", &up, ups, 6, "Model up axis"))
                            O.upAxis = static_cast<preview3d::ModelUpAxis>(up);
                        ImGui::Checkbox("Flip X", &O.flipX);
                        ImGui::Checkbox("Flip Y", &O.flipY);
                        ImGui::Checkbox("Flip Z", &O.flipZ);
                        Ui::SmartSliderFloat("Yaw offset", &O.yawOffsetDeg, -180.f, 180.f, 0.f, 1.f, "%.0f°");
                        if (ImGui::Button("ZZZ default (+Z up)")) {
                            O.upAxis = preview3d::ModelUpAxis::PlusZ;
                            O.flipX = O.flipY = O.flipZ = false;
                            O.yawOffsetDeg = 0.f;
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Identity (+Y up)")) {
                            O.upAxis = preview3d::ModelUpAxis::PlusY;
                            O.flipX = O.flipY = O.flipZ = false;
                        }
                    } else if (s_NPanel == 4) {
                        ImGui::TextUnformatted("Parts");
                        for (auto& it : s_Preview.Items()) {
                            ImGui::Checkbox((it.componentName + "/" + it.partName).c_str(), &it.visible);
                        }
                    } else if (s_NPanel == 5) {
                        ImGui::TextUnformatted("Passes / Debug");
                        ImGui::TextWrapped("%s", s_Preview.Status().c_str());
                        if (!s_Preview.LastError().empty())
                            ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "%s", s_Preview.LastError().c_str());

                        ImGui::Separator();
                        ImGui::TextUnformatted("Multi-pass (ZZZ)");
                        auto& P = s_Preview.Passes();
                        ImGui::Checkbox("Main", &P.enableMain);
                        ImGui::Checkbox("Outline (ZZZ TEXCOORD1)", &P.enableOutline);
                        ImGui::BeginDisabled(true);
                        ImGui::Checkbox("Glow (soon)", &P.enableGlow);
                        ImGui::Checkbox("Bloom (soon)", &P.enableBloom);
                        ImGui::EndDisabled();
                        Ui::SmartSliderFloat("Outline thick (view)", &P.outlineThickness, 0.2f, 3.0f, 1.f, 0.05f, "%.2f");
                        Ui::SmartSliderFloat("Outline ink (albedo*)", &P.outlineAlbedoMul, 0.15f, 0.75f, 0.4f, 0.05f, "%.2f");
                        ImGui::Checkbox("Outline × COLOR.r (thick)", &P.outlineUseVertexColor);
                        ImGui::TextDisabled("View-space expand ≈ game (not 3D balloon).\nInk ~0.4 = soft; 0.15 = black.");
                        ImGui::Checkbox("Fixed outline tint", &P.outlineUseFixedTint);
                        if (P.outlineUseFixedTint)
                            ImGui::ColorEdit3("Tint", P.outlineTint, ImGuiColorEditFlags_NoInputs);
                        ImGui::TextDisabled("Outline ≠ GI math. Mode locked to ZZZ for now.");

                        ImGui::Separator();
                        int dbg = s_Preview.GetDebugMode();
                        const char* modes[] = {
                            "Shaded (multipass)", "UV0", "Normals", "VertexColor", "OutlineUV pack",
                            "Shadow mask", "Metal/Rough/AO", "LightMap UV0", "MaterialMap",
                            "LightMap UV2 (diag)"
                        };
                        if (UiCombo("##view_mode", &dbg, modes, IM_ARRAYSIZE(modes), "View mode"))
                            s_Preview.SetDebugMode(dbg);
                        if (ImGui::Button("Reload scene", ImVec2(-1, 0)))
                            state.preview3DNeedReload = true;
                        if (ImGui::Button("Open Mod Setup", ImVec2(-1, 0)))
                            state.showModSetup = true;
                        ImGui::TextDisabled("LMB orbit · MMB pan · Wheel · Home");
                    }
                    ImGui::EndChild();
                }
                ImGui::EndChild();
            }
            ImGui::End();
        }

        Ui::TooltipEndFrame();

        // Non-modal open progress — never freeze the whole UI with a modal.
        // Document is locked via JobManager; chrome (panels, FE, console) stays interactive.
        if (g_LoadingState.isLoading) {
            ImGuiViewport* mainViewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(
                ImVec2(mainViewport->WorkPos.x + mainViewport->WorkSize.x * 0.5f,
                       mainViewport->WorkPos.y + 48.f),
                ImGuiCond_Always, ImVec2(0.5f, 0.f));
            ImGui::SetNextWindowBgAlpha(0.92f);
            ImGuiWindowFlags lf =
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                ImGuiWindowFlags_NoNav;
            if (ImGui::Begin("##OpenDocBanner", nullptr, lf)) {
                const std::string name = g_LoadingState.filepath.substr(
                    g_LoadingState.filepath.find_last_of("\\/") + 1);
                // Spinner
                {
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    const float r = 7.f;
                    const ImVec2 c(p.x + r + 2.f, p.y + ImGui::GetTextLineHeight() * 0.5f);
                    const float t = (float)ImGui::GetTime() * 7.5f;
                    for (int i = 0; i < 8; ++i) {
                        const float a = t + (float)i * 6.2831853f / 8.f;
                        const float fade = 0.2f + 0.8f * ((float)i / 8.f);
                        dl->AddCircleFilled(
                            ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r),
                            2.2f, IM_COL32(130, 175, 255, (int)(fade * 255.f)), 6);
                    }
                    ImGui::Dummy(ImVec2(r * 2.f + 10.f, ImGui::GetTextLineHeight()));
                    ImGui::SameLine();
                }
                ImGui::Text("Opening %s", name.c_str());
                float progress = g_LoadingState.progress;
                std::string stage;
                {
                    std::lock_guard<std::mutex> lock(g_LoadingState.mutex);
                    stage = g_LoadingState.stage;
                }
                ImGui::ProgressBar(progress > 0.f ? progress : -1.f * (float)ImGui::GetTime(),
                                   ImVec2(280, 0), stage.c_str());
                ImGui::TextDisabled("UI stays responsive · document locked until done");
            }
            ImGui::End();
        }

        // Python plugins draw in main.cpp *after* the canvas viewport so that
        // rayv.view has a current snapshot and overlays sit above the canvas.
    }
}