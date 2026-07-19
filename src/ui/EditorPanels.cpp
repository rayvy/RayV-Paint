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
#include "panels/ToolbarPanel.h"
#include "panels/ViewportNavPanel.h"
#include "panels/PropertiesPanel.h"
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

    // Fill Layer popup pipette (arm в†’ click canvas once)
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
        // Matches what the viewport shows вЂ” not the unfiltered all-maps blend.
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

    // 5-point star (no font glyph dependency вЂ” avoids diamond/?)
    static void DrawStarIcon(ImDrawList* dl, ImVec2 c, float r, ImU32 col, bool filled) {
        if (!dl) return;
        ImVec2 pts[10];
        const float pi = 3.14159265f;
        for (int i = 0; i < 5; ++i) {
            float aOuter = -pi * 0.5f + (float)i * (2.f * pi / 5.f);
            float aInner = aOuter + pi / 5.f;
            pts[i * 2]     = ImVec2(c.x + std::cos(aOuter) * r,     c.y + std::sin(aOuter) * r);
            pts[i * 2 + 1] = ImVec2(c.x + std::cos(aInner) * r * 0.42f,
                                    c.y + std::sin(aInner) * r * 0.42f);
        }
        if (filled) {
            // Star is non-convex вЂ” fill as triangle fan from center
            for (int i = 0; i < 10; ++i)
                dl->AddTriangleFilled(c, pts[i], pts[(i + 1) % 10], col);
        } else {
            dl->AddPolyline(pts, 10, col, ImDrawFlags_Closed, 1.6f);
        }
    }

    // Shared body for popup + dock workshop
    static void DrawBrushWorkshopBody(BrushSettings& brush, bool isPopup) {
        auto& lib = BrushLibrary::Get();
        auto& T = Ui::Tokens();
        // Owned tip for live workshop edits (Load tip image в†’ brush.tip points here)
        static BrushTip s_WorkshopTip;
        static bool s_WorkshopTipValid = false;

        ImGui::TextUnformatted("Brush Workshop");
        ImGui::SameLine();
        ImGui::TextDisabled(isPopup
            ? "Save в†’ disk В· star = Selector Wheel В· Load tip = texture brush"
            : "в… favorites for RMB-hold wheel В· Load tip for texture brushes");
        ImGui::Separator();

        // --- Left: list ---
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
        if (ImGui::IsItemHovered())
            Ui::Tooltip("Duplicate current settings into a new custom preset (then Save)");
        ImGui::Spacing();

        const std::string active = lib.GetActiveId();
        const float rowH = 64.f;
        const float starColW = 26.f;
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
            // Favorite star column (own hit target вЂ” not under Selectable)
            {
                bool fav = lib.IsFavorite(m.id);
                if (ImGui::InvisibleButton("##fav", ImVec2(starColW, rowH)))
                    lib.SetFavorite(m.id, !fav);
                if (ImGui::IsItemHovered())
                    Ui::Tooltip(fav ? "Remove from Selector Wheel" : "Add to Selector Wheel (RMB hold)");
                ImVec2 bmin = ImGui::GetItemRectMin();
                ImVec2 bmax = ImGui::GetItemRectMax();
                ImVec2 sc((bmin.x + bmax.x) * 0.5f, bmin.y + 14.f);
                ImDrawList* sdl = ImGui::GetWindowDrawList();
                DrawStarIcon(sdl, sc, 7.f,
                    fav ? IM_COL32(255, 200, 60, 255) : IM_COL32(140, 140, 150, 200), fav);
            }
            ImGui::SameLine(0, 0);

            if (ImGui::Selectable("##row", sel, 0, ImVec2(0, rowH))) {
                const bool er = brush.erase;
                lib.ApplyTo(m.id, brush);
                brush.erase = er;
                lib.SetActiveId(m.id);
                // After ApplyTo, tip may point into library storage
                s_WorkshopTipValid = false;
            }
            ImVec2 row1 = ImGui::GetItemRectMax();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(ImVec2(row0.x + starColW, row0.y + 4),
                ImVec2(row0.x + starColW + 5, row1.y - 4),
                ImGui::ColorConvertFloat4ToU32(chip), 2.f);
            char label[160];
            std::snprintf(label, sizeof(label), "%s%s", m.displayName.c_str(), m.isDirty ? " *" : "");
            dl->AddText(ImVec2(row0.x + starColW + 10, row0.y + 4), T.ColU32(T.textPrimary), label);
            char metaLine[96];
            std::snprintf(metaLine, sizeof(metaLine), "r=%.0f  h=%.0f%%  op=%.0f%%  sp=%.2f",
                params.radius, params.hardness * 100.f, params.opacity * 100.f, params.spacing);
            dl->AddText(ImVec2(row0.x + starColW + 10, row0.y + 20), T.ColU32(T.textSecondary), metaLine);
            ImVec2 pmin(row0.x + starColW + 10, row0.y + 36);
            ImVec2 pmax(row1.x - 8, row1.y - 4);
            dl->AddRectFilled(pmin, pmax, IM_COL32(18, 18, 20, 255), 3.f);
            DrawBrushStrokePreview(dl, pmin, pmax, prev, IM_COL32(230, 230, 235, 255));
            ImGui::PopID();
        }
        ImGui::EndChild();

        ImGui::SameLine(0, 12);

        // --- Right: inspector ---
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

            // Rename custom
            if (has && !meta.isBuiltin) {
                static char nameBuf[128] = {};
                static std::string nameForId;
                if (nameForId != meta.id) {
                    nameForId = meta.id;
                    std::snprintf(nameBuf, sizeof(nameBuf), "%s", meta.displayName.c_str());
                }
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("##rename", nameBuf, sizeof(nameBuf)))
                    lib.Rename(meta.id, nameBuf);
            }

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

            // Builtin tip chips
            if (ImGui::SmallButton("Soft")) { brush.tip = &BrushPresets::SoftRound(); s_WorkshopTipValid = false; }
            ImGui::SameLine(0, 4);
            if (ImGui::SmallButton("Hard")) { brush.tip = &BrushPresets::HardRound(); s_WorkshopTipValid = false; }
            ImGui::SameLine(0, 4);
            if (ImGui::SmallButton("Pencil")) { brush.tip = &BrushPresets::Pencil(); s_WorkshopTipValid = false; }
            ImGui::SameLine(0, 4);
            if (ImGui::SmallButton("Air")) { brush.tip = &BrushPresets::Airbrush(); s_WorkshopTipValid = false; }
            ImGui::SameLine(0, 4);
            if (ImGui::SmallButton("None")) { brush.tip = nullptr; brush.tipSourcePath.clear(); s_WorkshopTipValid = false; }

            if (ImGui::Button("Load tip imageвЂ¦")) {
                char path[512] = {};
                if (Ui::ShowOpenFile(path, sizeof(path),
                    "Images (*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds)\0*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds\0All\0*.*\0")) {
                    std::vector<uint8_t> rgba;
                    int w = 0, h = 0;
                    if (ImageManager::LoadImageFromFile(path, rgba, w, h) && w > 0 && h > 0) {
                        // Downsample / square crop to max 256, grayscale from alpha*luma
                        const int maxS = 256;
                        int side = std::min(w, h);
                        int srcX = (w - side) / 2, srcY = (h - side) / 2;
                        int outS = std::min(side, maxS);
                        s_WorkshopTip.pixels.assign((size_t)outS * outS, 0);
                        for (int y = 0; y < outS; ++y) {
                            for (int x = 0; x < outS; ++x) {
                                int sx = srcX + x * side / outS;
                                int sy = srcY + y * side / outS;
                                const uint8_t* p = rgba.data() + ((size_t)sy * w + sx) * 4;
                                float a = p[3] / 255.f;
                                float luma = (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) / 255.f;
                                float v = std::clamp(luma * a + (1.f - a) * 0.f, 0.f, 1.f);
                                // Prefer alpha as coverage if image is white-on-transparent
                                if (p[3] < 250 && (p[0] + p[1] + p[2]) > 600)
                                    v = a;
                                s_WorkshopTip.pixels[(size_t)y * outS + x] = (uint8_t)std::lround(v * 255.f);
                            }
                        }
                        s_WorkshopTip.size = outS;
                        s_WorkshopTip.spacingMul = 1.f;
                        s_WorkshopTip.name = "Custom";
                        s_WorkshopTipValid = true;
                        brush.tip = &s_WorkshopTip;
                        brush.tipSourcePath = path;
                        core::Notifications::Get().Push("Tip texture loaded (" + std::to_string(outS) + "ВІ)",
                            core::NotifyLevel::Info);
                    } else {
                        core::Notifications::Get().Push("Failed to load tip image", core::NotifyLevel::Error);
                    }
                }
            }
            if (ImGui::IsItemHovered())
                Ui::Tooltip("Load PNG/JPG/TGA/DDS as grayscale brush tip (embedded into preset on Save)");
            ImGui::SameLine();
            if (ImGui::Button("Clear tip")) {
                brush.tip = nullptr;
                brush.tipSourcePath.clear();
                s_WorkshopTipValid = false;
            }
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
            ImGui::Checkbox("в†’ Radius", &brush.pressureRadius); ImGui::SameLine();
            ImGui::Checkbox("в†’ Hardness", &brush.pressureHardness); ImGui::SameLine();
            ImGui::Checkbox("в†’ Opacity", &brush.pressureOpacity);

            ImGui::Separator();
            ImGui::Text("Rotation / dynamics");
            Ui::SmartSliderFloat("Rotation", &brush.rotationDeg, 0.f, 360.f, 0.f, 1.f, "%.0fВ°");
            ImGui::Checkbox("Pressure в†’ Rotation", &brush.pressureRotation);
            Ui::SmartSliderFloat("Scatter", &brush.scatter, 0.f, 1.f, 0.f, 0.05f, "%.2f");
            Ui::SmartSliderFloat("Angle jitter", &brush.angleJitter, 0.f, 1.f, 0.f, 0.05f, "%.2f");

            ImGui::Separator();
            if (has && meta.isBuiltin) {
                if (ImGui::Button("Save as new customвЂ¦", ImVec2(-1, 0))) {
                    std::string id = lib.CreateFromCurrent(brush, meta.displayName + " copy");
                    if (!id.empty()) {
                        lib.UpdateStaging(id, brush);
                        if (lib.SaveToDisk(id))
                            core::Notifications::Get().Push("Saved custom brush", core::NotifyLevel::Info);
                        else
                            core::Notifications::Get().Push("Save failed вЂ” check console", core::NotifyLevel::Error);
                        lib.SetActiveId(id);
                        const bool er = brush.erase;
                        lib.ApplyTo(id, brush);
                        brush.erase = er;
                    }
                }
            } else {
                ImGui::BeginDisabled(!has || meta.isBuiltin);
                if (ImGui::Button("Save preset", ImVec2(-1, 0))) {
                    lib.UpdateStaging(lib.GetActiveId(), brush);
                    if (lib.SaveToDisk(lib.GetActiveId()))
                        core::Notifications::Get().Push("Brush saved", core::NotifyLevel::Info);
                    else
                        core::Notifications::Get().Push("Save failed вЂ” check console", core::NotifyLevel::Error);
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && (!has || meta.isBuiltin))
                    Ui::Tooltip("Select or Create a custom brush first");
            }
            ImGui::BeginDisabled(!has || meta.isBuiltin);
            if (ImGui::Button("Delete", ImVec2(-1, 0))) {
                lib.DeleteCustom(lib.GetActiveId());
                lib.SetActiveId("builtin.soft_round");
                const bool er = brush.erase;
                lib.ApplyTo("builtin.soft_round", brush);
                brush.erase = er;
                s_WorkshopTipValid = false;
            }
            ImGui::EndDisabled();
        }
        ImGui::EndChild();

        // Keep dirty staging in sync while open (no-op if params match saved)
        if (!lib.GetActiveId().empty()) {
            BrushPresetMeta meta;
            if (lib.GetMeta(lib.GetActiveId(), meta) && !meta.isBuiltin)
                lib.UpdateStaging(lib.GetActiveId(), brush);
        }
        (void)s_WorkshopTipValid;
    }

    void DrawBrushPickerPopup(bool& openFlag, ImVec2 popupPos, BrushSettings& brush) {
        auto& T = Ui::Tokens();
        if (openFlag) {
            ImGui::OpenPopup("##BrushWorkshopPopup");
            openFlag = false;
        }
        ImGui::SetNextWindowPos(popupPos, ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(780, 480), ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints(ImVec2(640, 400), ImVec2(1400, 900));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, T.rMd);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 12));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(T.bgElevated.x, T.bgElevated.y, T.bgElevated.z, 0.97f));
        if (ImGui::BeginPopup("##BrushWorkshopPopup")) {
            DrawBrushWorkshopBody(brush, true);
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }

    void DrawBrushWorkshopPanel(UIState& state, BrushSettings& brush) {
        if (!state.showBrushWorkshop) return;
        Ui::BeginDockPanel("Brush Workshop", &state.showBrushWorkshop);
        Ui::ClampDockLeafBox(420.f, 1200.f, 320.f, 1200.f);
        DrawBrushWorkshopBody(brush, false);
        Ui::EndDockPanel();
    }

    bool DrawBrushSelectorWheel(bool holdingRmb, ImVec2 center, BrushSettings& brush) {
        // Call every frame while wheel is active, including the release frame
        // (holdingRmb=false on release в†’ apply hover and return false next frame).
        static bool s_wasActive = false;
        static int s_lastHover = -1;
        static std::vector<std::string> s_slotIds;

        if (!holdingRmb && !s_wasActive)
            return false;

        auto& lib = BrushLibrary::Get();
        auto& T = Ui::Tokens();

        if (holdingRmb && !s_wasActive) {
            s_slotIds = lib.ListFavoriteIds();
            s_lastHover = -1;
            s_wasActive = true;
        }
        if (s_slotIds.empty()) {
            // Nothing to pick вЂ” dismiss and hint via workshop
            s_wasActive = false;
            return false;
        }

        const int n = (int)s_slotIds.size();
        const float outerR = 140.f;
        const float innerR = 48.f;
        const float slotR = (outerR + innerR) * 0.5f;
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        ImVec2 mouse = ImGui::GetIO().MousePos;

        // Background ring
        dl->AddCircleFilled(center, outerR + 8.f, IM_COL32(12, 12, 14, 200), 64);
        dl->AddCircle(center, outerR + 8.f, T.ColU32(T.strokeHairline), 64, 1.5f);
        dl->AddCircleFilled(center, innerR, IM_COL32(20, 20, 24, 230), 48);

        float ang = std::atan2(mouse.y - center.y, mouse.x - center.x);
        int hover = -1;
        const float twoPi = 6.2831853f;
        float sector = twoPi / (float)n;

        for (int i = 0; i < n; ++i) {
            float a0 = -1.5708f + sector * (float)i; // start at top
            float a1 = a0 + sector;
            float amid = (a0 + a1) * 0.5f;
            float an = ang;
            while (an < a0) an += twoPi;
            while (an >= a0 + twoPi) an -= twoPi;
            bool hot = (an >= a0 && an < a1);
            float dist = std::sqrt((mouse.x - center.x) * (mouse.x - center.x) +
                                  (mouse.y - center.y) * (mouse.y - center.y));
            if (hot && dist > innerR && dist < outerR + 20.f) hover = i;

            ImU32 col = (hot || i == s_lastHover)
                ? T.ColU32(T.accent) : IM_COL32(40, 40, 48, 220);
            const int segs = 10;
            for (int s = 0; s < segs; ++s) {
                float t0 = a0 + (a1 - a0) * ((float)s / segs);
                float t1 = a0 + (a1 - a0) * ((float)(s + 1) / segs);
                ImVec2 p0(center.x + std::cos(t0) * innerR, center.y + std::sin(t0) * innerR);
                ImVec2 p1(center.x + std::cos(t0) * outerR, center.y + std::sin(t0) * outerR);
                ImVec2 p2(center.x + std::cos(t1) * outerR, center.y + std::sin(t1) * outerR);
                ImVec2 p3(center.x + std::cos(t1) * innerR, center.y + std::sin(t1) * innerR);
                dl->AddQuadFilled(p0, p1, p2, p3, col);
            }

            ImVec2 slot(center.x + std::cos(amid) * slotR, center.y + std::sin(amid) * slotR);
            BrushSettings prev;
            lib.ApplyTo(s_slotIds[i], prev);
            ImVec2 pmin(slot.x - 28, slot.y - 14);
            ImVec2 pmax(slot.x + 28, slot.y + 14);
            dl->AddRectFilled(pmin, pmax, IM_COL32(16, 16, 18, 255), 4.f);
            DrawBrushStrokePreview(dl, pmin, pmax, prev, IM_COL32(240, 240, 245, 255));

            BrushPresetMeta meta;
            if (lib.GetMeta(s_slotIds[i], meta)) {
                ImVec2 ts = ImGui::CalcTextSize(meta.displayName.c_str());
                dl->AddText(ImVec2(slot.x - ts.x * 0.5f, slot.y + 16), T.ColU32(T.textPrimary),
                    meta.displayName.c_str());
            }
        }

        const char* mid = "Brushes";
        ImVec2 mts = ImGui::CalcTextSize(mid);
        dl->AddText(ImVec2(center.x - mts.x * 0.5f, center.y - mts.y * 0.5f),
            T.ColU32(T.textSecondary), mid);

        if (holdingRmb) {
            s_lastHover = hover;
            return true;
        }

        // Release frame: apply last hovered favorite
        if (s_lastHover >= 0 && s_lastHover < n) {
            const bool er = brush.erase;
            lib.ApplyTo(s_slotIds[s_lastHover], brush);
            brush.erase = er;
            lib.SetActiveId(s_slotIds[s_lastHover]);
        }
        s_wasActive = false;
        s_lastHover = -1;
        s_slotIds.clear();
        return true;
    }

    // Themed combo вЂ” kit API (Ui::Combo). Local alias for call-site brevity.
    static bool UiCombo(const char* id, int* idx, const char* const* items, int count,
                        const char* label = nullptr) {
        return Ui::Combo(id, idx, items, count, label);
    }

    // ICC preset combo (presets only вЂ” no free-text path). Returns true if changed.
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

    void RenderAll(UIState& state, Canvas& canvas, BrushSettings& brush, ActiveTool& activeTool, ID3D11Device* device, ID3D11DeviceContext* context, GLFWwindow* window) {
        if (device)
            Ui::SvgIconCache::Get().SetDevice(device);
        Ui::TooltipSetDelay(Ui::Tokens().tooltipDelaySec);
        ImGuiViewport* mainViewport = ImGui::GetMainViewport();

        // =====================================================================
        // Chrome (UX questionnaire): menu bar ON TOP, document tabs BELOW.
        // Window caption buttons flush-right, equal size, vertically centered.
        // =====================================================================

        // Caption button strip width вЂ” reserved on the menu bar so tools don't collide.
        static constexpr float kCapBtnW = 46.f;
        static constexpr float kCapCount = 3.f;
        static constexpr float kCapStripW = kCapBtnW * kCapCount;

        // Undecorated window drag state (ImGui item Active alone is unreliable on menu/tabs).
        static bool s_ChromeDragging = false;
        static int s_ChromeDragWinX = 0, s_ChromeDragWinY = 0;
        static ImVec2 s_ChromeDragMouse0{};

        auto chromeDragBegin = [&]() {
            if (!window || s_ChromeDragging) return;
            // Maximized: Win32 ignores SetWindowPos вЂ” restore first under cursor.
            if (glfwGetWindowAttrib(window, GLFW_MAXIMIZED)) {
                glfwRestoreWindow(window);
            }
            glfwGetWindowPos(window, &s_ChromeDragWinX, &s_ChromeDragWinY);
            s_ChromeDragMouse0 = ImGui::GetIO().MousePos;
            s_ChromeDragging = true;
        };
        auto chromeDragUpdate = [&]() {
            if (!s_ChromeDragging || !window) return;
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                s_ChromeDragging = false;
                return;
            }
            ImVec2 m = ImGui::GetIO().MousePos;
            glfwSetWindowPos(window,
                s_ChromeDragWinX + (int)(m.x - s_ChromeDragMouse0.x),
                s_ChromeDragWinY + (int)(m.y - s_ChromeDragMouse0.y));
        };
        auto chromeDragOnItem = [&]() {
            // Call after InvisibleButton / drag handle that owns the click.
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && window) {
                s_ChromeDragging = false;
                if (glfwGetWindowAttrib(window, GLFW_MAXIMIZED))
                    glfwRestoreWindow(window);
                else
                    glfwMaximizeWindow(window);
                return;
            }
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f))
                chromeDragBegin();
        };
        // Keep drag alive even if ActiveId is lost mid-gesture.
        if (s_ChromeDragging)
            chromeDragUpdate();

        auto drawCaptionButtons = [&](float barY, float barH) {
            // Absolute right of main viewport вЂ” equal cells, full bar height (not SmallButton drift).
            const float x0 = mainViewport->Pos.x + mainViewport->Size.x - kCapStripW;
            ImDrawList* dl = ImGui::GetForegroundDrawList(mainViewport);
            auto& tok = Ui::Tokens();
            const char* labelsA[3] = { "-", "[]", "X" };
            // Subtle separator left of caption strip
            dl->AddLine(ImVec2(x0 - 1.f, barY + 4.f), ImVec2(x0 - 1.f, barY + barH - 4.f),
                        IM_COL32(70, 70, 80, 180), 1.f);
            for (int i = 0; i < 3; ++i) {
                ImVec2 a(x0 + kCapBtnW * (float)i, barY);
                ImVec2 b(a.x + kCapBtnW, barY + barH);
                const bool hov = ImGui::IsMouseHoveringRect(a, b, false);
                ImU32 bg = hov
                    ? (i == 2 ? IM_COL32(200, 55, 55, 255) : IM_COL32(58, 58, 68, 255))
                    : IM_COL32(36, 36, 42, 255);
                dl->AddRectFilled(a, b, bg);
                ImVec2 ts = ImGui::CalcTextSize(labelsA[i]);
                dl->AddText(ImVec2(a.x + (kCapBtnW - ts.x) * 0.5f,
                                   a.y + (barH - ts.y) * 0.5f),
                            tok.ColU32(tok.textPrimary), labelsA[i]);
                if (hov && ImGui::IsMouseClicked(0) && window) {
                    if (i == 0) glfwIconifyWindow(window);
                    else if (i == 1) {
                        if (glfwGetWindowAttrib(window, GLFW_MAXIMIZED))
                            glfwRestoreWindow(window);
                        else
                            glfwMaximizeWindow(window);
                    } else {
                        glfwSetWindowShouldClose(window, GLFW_TRUE);
                    }
                }
                if (hov) {
                    if (i == 0) Ui::Tooltip("Minimize");
                    else if (i == 1) Ui::Tooltip("Maximize / Restore");
                    else Ui::Tooltip("Close");
                }
            }
        };

        // 0. Main menu bar FIRST (top of window) вЂ” File / Edit / вЂ¦ + mode tools + caption
        if (ImGui::BeginMainMenuBar()) {
            // Brand + small drag handle (menu gap + tab strip are primary grab zones)
            {
                ImGui::TextDisabled("RayV");
                ImGui::SameLine(0, 4.f);
                ImGui::InvisibleButton("##branddrag", ImVec2(28.f, ImGui::GetFrameHeight()));
                chromeDragOnItem();
                if (ImGui::IsItemHovered())
                    Ui::Tooltip("Drag to move window");
                ImGui::SameLine(0, 8.f);
            }
            if (ImGui::BeginMenu("File")) {
                // Catalog-backed: same poll + execute as hotkeys
                core::ops::MenuAction("NewProject");
                if (ImGui::MenuItem("New Blank Tab")) {
                    ProjectManager::Get().CreateEmptyProject();
                }
                if (ImGui::MenuItem("Import TextureвЂ¦")) {
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
                    const char* aeLabel = advanced ? "Batch ExportвЂ¦" : "Advanced ExportвЂ¦";
                    std::string sc = KeymapManager::Get().GetActionShortcutString("AdvancedExport");
                    if (ImGui::MenuItem(aeLabel, (sc == "вЂ”" || sc == "None") ? nullptr : sc.c_str())) {
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
                        (undoSc == "вЂ”" || undoSc == "None") ? nullptr : undoSc.c_str(),
                        false, canvas.CanUndo())) {
                    core::ops::Invoke("Undo");
                }

                std::string redoLabel = "Redo";
                if (canvas.CanRedo()) {
                    redoLabel += " (" + canvas.GetRedoName() + ")";
                }
                std::string redoSc = KeymapManager::Get().GetActionShortcutString("Redo");
                if (ImGui::MenuItem(redoLabel.c_str(),
                        (redoSc == "вЂ”" || redoSc == "None") ? nullptr : redoSc.c_str(),
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
                if (ImGui::MenuItem("Export Vector Layer as SVGвЂ¦", nullptr, false, isVec)) {
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
                    ImGui::TextDisabled("Working space only вЂ” export packing free.");
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
                ImGui::MenuItem("Brush Workshop", nullptr, &state.showBrushWorkshop);
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
                            "ON: bake shadows/outlines/filters for display (CPU вЂ” can lag).\n"
                            "OFF: show raw paint content so brush stays fast.\n"
                            "Effects stay on the layer; toggle ON to re-bake.\n"
                            "Export always applies full effects.");
                }
                ImGui::MenuItem("Rulers", nullptr, &state.showRulers);
                ImGui::MenuItem("Mod SetupвЂ¦", nullptr, &state.showModSetup);
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
                        ImGui::TextDisabled("(no plugins вЂ” Refresh Scripts)");
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
                                "Register incomplete вЂ” accept UAC, ensure RayVPaint_DdsThumb.dll "
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
                    // ---- Helper microservices (PNGв†’DDS + atlas) вЂ” registry only ----
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
                        if (ImGui::MenuItem("Register helpers (PNG в†’ DDS + Atlas)")) {
                            if (HelperShellRegister::EnsureRegistered())
                                Logger::Get().Info(
                                    "PNG helpers registered. Right-click PNG(s) in Explorer.");
                            else
                                Logger::Get().Error(
                                    "Helpers register failed вЂ” build RayVHelpers.exe next to Core.");
                        }
                        if (ImGui::IsItemHovered())
                            Ui::Tooltip(
                                "Registers Explorer context menu for multi-select PNG:\n"
                                "вЂў Convert to DDS (RayVPaint)\n"
                                "вЂў Create Texture Atlas (RayVPaint)\n"
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
                if (ImGui::MenuItem("About RayV-PaintвЂ¦"))
                    state.openAboutModal = true;
                ImGui::EndMenu();
            }

            // ---- Drag gap between menus and right tools (primary grab for title drag) ----
            {
                Project* hp = ProjectManager::Get().ActiveProject();
                auto ptype = canvas.GetProjectType();
                const bool adv = (ptype != Canvas::ProjectType::Simple);
                if (hp) hp->textureSets.EnsureSimpleDefault();
                texset::TextureSet* hset = hp ? hp->textureSets.Active() : nullptr;

                float rightTools = 210.f;
                if (adv && hset) {
                    for (const auto& m : hset->maps)
                        if (m.enabled || m.kind == texset::MapKind::Diffuse)
                            rightTools += 72.f;
                }
                float barW = ImGui::GetWindowWidth();
                float afterMenus = ImGui::GetCursorPosX();
                float dragW = barW - afterMenus - rightTools - kCapStripW - 16.f;
                if (dragW > 12.f) {
                    ImGui::SameLine(0, 4.f);
                    ImGui::InvisibleButton("##menudrag", ImVec2(dragW, ImGui::GetFrameHeight()));
                    chromeDragOnItem();
                    if (ImGui::IsItemHovered())
                        Ui::Tooltip("Drag to move window");
                }

                // Caption strip always owns the last kCapStripW of the bar
                float x = barW - rightTools - kCapStripW - 12.f;
                if (x > ImGui::GetCursorPosX() + 8.f)
                    ImGui::SameLine(x);

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
                        Ui::Tooltip("Project mode вЂ” workspace layout");
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

            // Window caption buttons (flush right, equal cells) вЂ” drawn after menu content
            {
                ImVec2 wp = ImGui::GetWindowPos();
                float barH = ImGui::GetWindowSize().y;
                drawCaptionButtons(wp.y, barH);
            }

            ImGui::EndMainMenuBar();
        }

        // 1. Document tabs strip BELOW the menu
        {
            const float tabsH = 28.f;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.f, 3.f));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.f, 0.f));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.10f, 0.12f, 1.f));
            ImGui::BeginViewportSideBar("##DocTabs", mainViewport, ImGuiDir_Up, tabsH,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoDocking);

            ImVec2 barMin = ImGui::GetWindowPos();
            ImVec2 barSize = ImGui::GetContentRegionAvail();
            // Full-strip drag handle (tabs drawn on top; empty space + handle still drag)
            ImGui::SetCursorScreenPos(barMin);
            ImGui::InvisibleButton("##tabs_drag", ImVec2(std::max(1.f, barSize.x), tabsH));
            chromeDragOnItem();

            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                auto& tok = Ui::Tokens();
                float tabX = barMin.x + 8.f;
                float tabY = barMin.y + 3.f;
                const float tabH = tabsH - 6.f;
                auto tabs = ProjectManager::Get().ListTabs();
                for (const auto& tab : tabs) {
                    std::string label = tab.title;
                    if (tab.dirty) label += " *";
                    if (tab.restoring) label += " ...";
                    else if (tab.diskHibernated) label += " [disk]";
                    else if (tab.gpuSuspended) label += " [z]";
                    ImVec2 ts = ImGui::CalcTextSize(label.c_str());
                    float tw = ts.x + 28.f;
                    ImVec2 a(tabX, tabY);
                    ImVec2 b(tabX + tw, tabY + tabH);
                    ImU32 bg = tab.active ? IM_COL32(50, 70, 110, 230) : IM_COL32(32, 32, 38, 200);
                    if (!tab.active && tab.diskHibernated)
                        bg = IM_COL32(28, 32, 40, 160);
                    else if (!tab.active && tab.gpuSuspended)
                        bg = IM_COL32(30, 34, 48, 170);
                    dl->AddRectFilled(a, b, bg, 4.f);
                    ImU32 tc = tok.ColU32(tok.textPrimary);
                    if (!tab.active && (tab.gpuSuspended || tab.diskHibernated))
                        tc = tok.ColU32(tok.textSecondary);
                    const float textY = a.y + (tabH - ts.y) * 0.5f;
                    dl->AddText(ImVec2(a.x + 8.f, textY), tc, label.c_str());
                    if (ImGui::IsMouseHoveringRect(a, b) && !ImGui::IsMouseDragging(0, 2.f)) {
                        if (tab.diskHibernated)
                            Ui::Tooltip("Disk hibernate вЂ” click to RESTORE (pixels on disk)");
                        else if (tab.gpuSuspended)
                            Ui::Tooltip("GPU sleep вЂ” click to wake (pixels in RAM)");
                        else if (tab.restoring)
                            Ui::Tooltip("RESTORINGвЂ¦");
                    }
                    // Close вЂ” vertically centered
                    ImVec2 xa(b.x - 16.f, a.y + (tabH - ImGui::GetFontSize()) * 0.5f);
                    ImVec2 hitMin(b.x - 18.f, a.y);
                    ImVec2 hitMax(b.x - 2.f, b.y);
                    if (ImGui::IsMouseHoveringRect(hitMin, hitMax)) {
                        dl->AddText(xa, IM_COL32(255, 120, 120, 255), "x");
                        if (ImGui::IsMouseClicked(0) && !s_ChromeDragging) {
                            if (!ProjectManager::Get().CloseProject(tab.id, false)) {
                                g_ProjectTabCloseRequest.projectId = tab.id;
                                g_ProjectTabCloseRequest.pending = true;
                            }
                        }
                    } else {
                        dl->AddText(xa, tok.ColU32(tok.textSecondary), "x");
                    }
                    // Select tab (click, not drag)
                    if (ImGui::IsMouseHoveringRect(a, b) && ImGui::IsMouseReleased(0) &&
                        !s_ChromeDragging && !ImGui::IsMouseDragging(0, 4.f)) {
                        // Only if release inside and not on close
                        if (!ImGui::IsMouseHoveringRect(hitMin, hitMax) && !tab.active)
                            ProjectManager::Get().SwitchTo(tab.id);
                    }
                    tabX += tw + 4.f;
                }
                // New tab
                ImVec2 na(tabX, tabY);
                ImVec2 nb(tabX + 24.f, tabY + tabH);
                dl->AddRectFilled(na, nb, IM_COL32(40, 40, 48, 200), 4.f);
                ImVec2 plusSz = ImGui::CalcTextSize("+");
                dl->AddText(ImVec2(na.x + (24.f - plusSz.x) * 0.5f,
                                   na.y + (tabH - plusSz.y) * 0.5f),
                            tok.ColU32(tok.textPrimary), "+");
                if (ImGui::IsMouseHoveringRect(na, nb) && ImGui::IsMouseReleased(0) &&
                    !s_ChromeDragging)
                    ProjectManager::Get().CreateEmptyProject();
            }

            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
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

        // Operator modals: NO dim/scrim вЂ” user must see live image changes (not a "cat in a bag").
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
            ImGui::Text("Blur (3-pass box) вЂ” preview on active layer");
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
            ImGui::TextDisabled("Live preview on active layer В· selection mask");
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

        // Curves Modal вЂ” interactive spline editor + live canvas preview
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
            ImGui::SameLine(); ImGui::TextDisabled("Live layer preview В· selection");

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

        // 2. Persistent Footer вЂ” FIXED height (never jumps when jobs appear)
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
        // Always 28px вЂ” job status is centered in-band (no second row)
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
            // Heights fixed вЂ” job strip never changes footer height.
            const float winW = ImGui::GetWindowWidth();
            const float contextBtnW = 72.f;
            const float notifyChipW = 148.f;
            const float centerJobW = 280.f;
            const float rightReserve = contextBtnW + notifyChipW + 16.f;
            const float leftMax = std::max(80.f, winW - rightReserve - centerJobW - 24.f);

            // LEFT вЂ” compact metrics (clipped)
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

            // CENTER вЂ” fixed job slot (always reserved position, content optional)
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
                            std::snprintf(line, sizeof(line), "%sвЂ¦", j.name.c_str());
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
                            ImGui::TextUnformatted("OpeningвЂ¦");
                        else if (state.fileExplorer.dirListingBusy)
                            ImGui::TextUnformatted("Indexing folderвЂ¦");
                        else if (assets::AssetManager::Get().IsIndexScanning())
                            ImGui::TextUnformatted("Indexing assetsвЂ¦");
                        else if (feBusy || assetsBusy)
                            ImGui::TextUnformatted("BackgroundвЂ¦");
                    }
                }
                ImGui::EndChild();
            }

            // RIGHT вЂ” notification + context (absolute positions, stable)
            {
                std::string preview = core::Notifications::Get().LatestPreview(28);
                if (preview.empty()) preview = "вЂ”";
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
            ImGui::TextDisabled("Presets only вЂ” no free-text ICC path");
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
                        "cycle variants вЂ” members show \"via вЂ¦\" when unbound.");
                    ImGui::Separator();
                    ImGui::Spacing();

                    // Rebind capture (Escape cancels)
                    if (state.listeningForKey) {
                        core::ops::AppContext::NotifyUiKeyboardCapture();
                        ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.f, 1.f),
                            "Listening for \"%s\" вЂ” press key (+ mods). Esc cancels.",
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
                ImGui::TextUnformatted("Welcome back вЂ” recent autosaves");
                ImGui::TextDisabled("UNTITLED / project В· time stamp В· type В· quit saves included");
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
                    std::string label = e.baseName + "  В·  " + e.projectType +
                        (e.isQuit ? "  В·  quit" : "");
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
                    ImGui::TextDisabled("No autosaves yet. Paint something вЂ” saves every %d min.",
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

        // 5–6. Tool strip + Viewport Nav + Properties (extracted panels)
        UI::DrawToolbarPanel(state, canvas, brush, activeTool);
        UI::DrawViewportNavPanel(state, canvas);
        UI::DrawPropertiesPanel(state, canvas);

        UI::DrawLayersPanel(state, canvas, device);
        UI::DrawAssetBrowserPanel(state, canvas, device);
        UI::DrawBrushWorkshopPanel(state, brush);
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
            ImGui::TextDisabled("Click line = copy В· multi-select with Ctrl");
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

        // ---- Mod Setup (INI / dump / semantics) вЂ” separate from Properties ----
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
                        ImGui::TextDisabled("TEXCOORD в‰  always UV. Role=None ignores attribute.");
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
                                                    ImGui::BulletText("%s в†’ %s%s",
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
                    if (ImGui::Button("В«##np", ImVec2(nTabW - 6, 22)))
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
                        if (Ui::SmartSliderFloat("Yaw", &yawDeg, -180.f, 180.f, 0.f, 1.f, "%.0fВ°"))
                            L.yaw = yawDeg * (3.14159265f / 180.f);
                        if (Ui::SmartSliderFloat("Pitch", &pitchDeg, -89.f, 89.f, 0.f, 1.f, "%.0fВ°"))
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
                        ImGui::TextDisabled("Uber shader В· channel remaps");
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

                            // Bind diagnostics вЂ” verify multi-texture paths
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
                                        ok ? base.c_str() : "(missing вЂ” grey fallback)");
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
                                ImGui::TextDisabled("If look is wrong вЂ” remap first, then blame shader.");
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
                            ImGui::TextDisabled("No mesh вЂ” Apply INI + Reload");
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
                        Ui::SmartSliderFloat("Yaw offset", &O.yawOffsetDeg, -180.f, 180.f, 0.f, 1.f, "%.0fВ°");
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
                        ImGui::Checkbox("Outline Г— COLOR.r (thick)", &P.outlineUseVertexColor);
                        ImGui::TextDisabled("View-space expand в‰€ game (not 3D balloon).\nInk ~0.4 = soft; 0.15 = black.");
                        ImGui::Checkbox("Fixed outline tint", &P.outlineUseFixedTint);
                        if (P.outlineUseFixedTint)
                            ImGui::ColorEdit3("Tint", P.outlineTint, ImGuiColorEditFlags_NoInputs);
                        ImGui::TextDisabled("Outline в‰  GI math. Mode locked to ZZZ for now.");

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
                        ImGui::TextDisabled("LMB orbit В· MMB pan В· Wheel В· Home");
                    }
                    ImGui::EndChild();
                }
                ImGui::EndChild();
            }
            ImGui::End();
        }

        Ui::TooltipEndFrame();

        // Non-modal open progress вЂ” never freeze the whole UI with a modal.
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
                ImGui::TextDisabled("UI stays responsive В· document locked until done");
            }
            ImGui::End();
        }

        // Python plugins draw in main.cpp *after* the canvas viewport so that
        // rayv.view has a current snapshot and overlays sit above the canvas.
    }
}
