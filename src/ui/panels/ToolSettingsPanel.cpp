#include "ToolSettingsPanel.h"
#include "../EditorPanels.h"
#include "../../vector/VectorToolSession.h"
#include "../dialogs/Win32FileDialogs.h"
#include "../widgets/UiPanel.h"
#include "../widgets/UiDropdown.h"
#include "../widgets/UiIconToggle.h"
#include "../widgets/UiIconButton.h"
#include "../widgets/UiVisualSlider.h"
#include "../widgets/UiTooltip.h"
#include "../icons/SvgIconCache.h"
#include "../../core/ConfigManager.h"
#include "../../core/ImageManager.h"
#include "../../core/PaintEngine.h"
#include "../../core/ops/OperatorRegistry.h"
#include "../../layer/LayerTypes.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace UI {

void DrawToolSettingsPanel(UIState& state, Canvas& canvas, BrushSettings& brush,
                           ActiveTool activeTool, ID3D11Device* device) {
    if (!state.showToolSettings) return;

    ImVec2 preAvail = ImGui::GetContentRegionAvail(); // may be 0 before begin
    Ui::BeginDockPanel("Tool Settings", &state.showToolSettings);
    // Horizontal tool strip: hard height clamp (no rubber-band stretch)
    Ui::ClampDockLeafCrossAxis(/*verticalStrip=*/false, 28.f, 48.f);

    ImVec2 tsAvail = ImGui::GetContentRegionAvail();
    bool tsHorizontal = (tsAvail.x >= tsAvail.y * 0.85f);
    (void)preAvail;
    (void)tsHorizontal;

    // Exact text entry via kit (double-click / Ctrl+click). Do not use raw ImGui::SliderFloat.
    auto MiniSlider = [&](const char* id, float* v, float mn, float mx, const char* tip, float width = 110.f) {
        ImGui::SetNextItemWidth(width);
        float def = (mn + mx) * 0.5f;
        if (Ui::SmartSliderFloat(id, v, mn, mx, def, 0.f, "%.2f") && tip) { /* value changed */ }
        if (ImGui::IsItemHovered() && tip) Ui::Tooltip(tip);
    };

    bool isBrushLike = (activeTool == ActiveTool::Brush || activeTool == ActiveTool::Eraser ||
                        activeTool == ActiveTool::Stamp);
    bool isVector = UI::IsVectorTool(activeTool);

    if (isVector) {
        // ---- Clear workflow panel (vectors are confusing without this) ----
        ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.f, 1.f), "%s",
            vec::VectorToolTitle(activeTool));
        ImGui::SameLine();
        {
            std::string live = vec::VectorToolLiveStatus(canvas, activeTool);
            ImGui::TextDisabled("· %s", live.c_str());
        }

        ImGui::Separator();
        ImGui::TextWrapped("%s", vec::VectorToolHowTo(activeTool));

        ImGui::Spacing();
        ImGui::TextUnformatted("Style for new shapes:");
        ImGui::Checkbox("Fill##vfill", &g_VectorToolStyle.fillEnabled);
        ImGui::SameLine();
        ImGui::ColorEdit4("##vfillc", g_VectorToolStyle.fillRgba,
            ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
        ImGui::SameLine();
        ImGui::Checkbox("Stroke##vstr", &g_VectorToolStyle.strokeEnabled);
        ImGui::SameLine();
        ImGui::ColorEdit4("##vstrc", g_VectorToolStyle.strokeRgba,
            ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
        ImGui::SameLine();
        MiniSlider("##vsw", &g_VectorToolStyle.strokeWidth, 0.5f, 64.f, "Stroke width (document px)", 90.f);
        ImGui::SameLine();
        ImGui::Checkbox("Scale styles##vss", &g_VectorToolStyle.scaleStyles);
        if (ImGui::IsItemHovered())
            Ui::Tooltip("When resizing, also scale stroke width");
        if (activeTool == ActiveTool::VectorFreehand) {
            ImGui::SameLine();
            ImGui::Checkbox("Close path##vfh", &g_VectorToolStyle.freehandClosed);
        }
        if (activeTool == ActiveTool::VectorPolygon) {
            ImGui::SameLine();
            ImGui::Checkbox("Closed##vpg", &g_VectorToolStyle.polygonClosed);
            if (ImGui::IsItemHovered())
                Ui::Tooltip("On = polygon (closed), Off = polyline (open)");
        }
        if (activeTool == ActiveTool::VectorRect) {
            ImGui::SameLine();
            MiniSlider("##vrx", &g_VectorToolStyle.rectCornerRx, 0.f, 256.f, "Default corner radius X", 70.f);
            ImGui::SameLine();
            MiniSlider("##vry", &g_VectorToolStyle.rectCornerRy, 0.f, 256.f, "Default corner radius Y", 70.f);
        }

        // Advanced paint
        ImGui::Checkbox("Linear gradient fill##vlg", &g_VectorToolStyle.fillLinearGrad);
        if (g_VectorToolStyle.fillLinearGrad) {
            ImGui::SameLine();
            ImGui::ColorEdit4("##vg1", g_VectorToolStyle.gradRgba1,
                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
            if (ImGui::IsItemHovered()) Ui::Tooltip("Gradient end color (start = Fill color)");
        }
        ImGui::SameLine();
        MiniSlider("##vdash", &g_VectorToolStyle.dashLen, 0.f, 64.f, "Dash length (0 = solid)", 70.f);
        ImGui::SameLine();
        MiniSlider("##vgap", &g_VectorToolStyle.gapLen, 0.f, 64.f, "Gap length", 70.f);

        // Numeric transform for selected shape
        const bool hasSel = vec::VectorSelectedShapeId() != 0;
        const int selN = vec::VectorSelectionCount();
        float bx = 0, by = 0, bw = 0, bh = 0;
        if (selN > 1) {
            ImGui::TextDisabled("%d shapes selected (Shift+click). Align / Join below.", selN);
        }
        if (hasSel && selN == 1 && vec::VectorGetSelectionBounds(canvas, bx, by, bw, bh)) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextDisabled("Selection transform (document px)");
            ImGui::SetNextItemWidth(60.f);
            bool ch = false;
            ch |= ImGui::DragFloat("X##vsx", &bx, 1.f);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60.f);
            ch |= ImGui::DragFloat("Y##vsy", &by, 1.f);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60.f);
            ch |= ImGui::DragFloat("W##vsw2", &bw, 1.f, 1.f, 100000.f);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60.f);
            ch |= ImGui::DragFloat("H##vsh", &bh, 1.f, 1.f, 100000.f);
            if (ch)
                vec::VectorSetSelectionBounds(canvas, bx, by, bw, bh, g_VectorToolStyle.scaleStyles);
            float rx = 0, ry = 0;
            if (vec::VectorGetSelectionRound(canvas, rx, ry)) {
                ImGui::SetNextItemWidth(70.f);
                bool cr = false;
                cr |= ImGui::DragFloat("Round X##vrx2", &rx, 0.5f, 0.f, bw * 0.5f);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(70.f);
                cr |= ImGui::DragFloat("Round Y##vry2", &ry, 0.5f, 0.f, bh * 0.5f);
                if (cr)
                    vec::VectorSetSelectionRound(canvas, rx, ry);
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("Quick tools");
        if (ImGui::SmallButton("Select##v2s"))
            core::ops::Invoke("VectorSelectTool");
        ImGui::SameLine();
        if (ImGui::SmallButton("Edit##v2e"))
            core::ops::Invoke("VectorEditTool");
        ImGui::SameLine();
        if (ImGui::SmallButton("Rect##v2r"))
            core::ops::Invoke("VectorRectTool");
        ImGui::SameLine();
        if (ImGui::SmallButton("Pen##v2p"))
            core::ops::Invoke("VectorPenTool");
        ImGui::SameLine();
        if (ImGui::SmallButton("Freehand##v2f"))
            core::ops::Invoke("VectorFreehandTool");
        ImGui::SameLine();
        if (ImGui::SmallButton("Polygon##v2g"))
            core::ops::Invoke("VectorPolygonTool");

        ImGui::Spacing();
        ImGui::BeginDisabled(!hasSel);
        if (ImGui::SmallButton("Apply style##v2as"))
            vec::VectorActionApplyStyle(canvas);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            Ui::Tooltip("Apply Fill/Stroke/Gradient/Dash to selection");
        ImGui::SameLine();
        if (ImGui::SmallButton("To path##v2cp"))
            vec::VectorActionConvertToPath(canvas);
        ImGui::SameLine();
        if (ImGui::SmallButton("Break##v2br"))
            vec::VectorActionBreakAtNode(canvas);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            Ui::Tooltip("Edit: select middle node, Break splits path");
        ImGui::SameLine();
        if (ImGui::SmallButton("Join##v2jn"))
            vec::VectorActionJoinPaths(canvas);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            Ui::Tooltip("Select 2 open paths (Shift+click) with nearby ends → Join");
        ImGui::SameLine();
        if (ImGui::SmallButton("Front##v2f2"))
            vec::VectorActionBringFront(canvas);
        ImGui::SameLine();
        if (ImGui::SmallButton("Back##v2b"))
            vec::VectorActionSendBack(canvas);
        ImGui::SameLine();
        if (ImGui::SmallButton("Dup##v2d"))
            vec::VectorActionDuplicate(canvas);
        ImGui::EndDisabled();

        // Align (2+)
        ImGui::BeginDisabled(selN < 2);
        ImGui::TextDisabled("Align");
        if (ImGui::SmallButton("L##val")) vec::VectorActionAlign(canvas, 0);
        ImGui::SameLine();
        if (ImGui::SmallButton("HC##vah")) vec::VectorActionAlign(canvas, 1);
        ImGui::SameLine();
        if (ImGui::SmallButton("R##var")) vec::VectorActionAlign(canvas, 2);
        ImGui::SameLine();
        if (ImGui::SmallButton("T##vat")) vec::VectorActionAlign(canvas, 3);
        ImGui::SameLine();
        if (ImGui::SmallButton("VC##vav")) vec::VectorActionAlign(canvas, 4);
        ImGui::SameLine();
        if (ImGui::SmallButton("B##vab")) vec::VectorActionAlign(canvas, 5);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            Ui::Tooltip("Align needs 2+ shapes (Shift+click)");

        ImGui::SameLine();
        if (ImGui::SmallButton("Export SVG##v2ex"))
            vec::VectorActionExportSvg(canvas);

        Ui::EndDockPanel();
        return;
    }

    if (isBrushLike) {
        if (activeTool == ActiveTool::Stamp) {
            ImGui::TextDisabled(canvas.StampHasSource()
                ? (canvas.StampHasOffset()
                    ? "Stamp: source+offset locked · Alt+click = new source"
                    : "Stamp: source set · first dab locks offset")
                : "Stamp: Alt+click sets source · then paint elsewhere");
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear##stamp_src"))
                canvas.StampClearSource();
        }
        // Brush blend mode — icon + combo (no verbose label)
        if (activeTool == ActiveTool::Brush && !brush.erase) {
            static const char* blendNames[] = {
                "Normal","Multiply","Screen","Overlay","Add","Subtract","Darken","Lighten","HardLight","SoftLight"
            };
            int bi = (int)brush.blendMode;
            Ui::IconButton("##blend_ico", "ts_blend_mode", ImVec2(22, 22),
                "Blend mode", true, false);
            ImGui::SameLine(0, 4);
            ImGui::SetNextItemWidth(100.f);
            if (Ui::Combo("##brush_blend", &bi, blendNames, IM_ARRAYSIZE(blendNames), "Blend mode"))
                brush.blendMode = (BlendMode)bi;
            if (ImGui::IsItemHovered()) Ui::Tooltip("Brush blend mode");
            ImGui::SameLine();
        }
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
        Ui::IconButton("##tip_ico", "ts_brush_tip", ImVec2(22, 22), "Brush tip shape", true, false);
        ImGui::SameLine(0, 4);
        ImGui::SetNextItemWidth(72.f);
        if (Ui::Combo("##tip", &tipIdx, tipNames, IM_ARRAYSIZE(tipNames))) {
            state.brushTipPreset = tipIdx;
            if (tipIdx >= 0 && tipIdx < 5) {
                canvas.SetBrushTipId(tipIds[tipIdx]);
                s_LastSyncedTipId = tipIds[tipIdx];
                ApplyTipId(tipIds[tipIdx]);
            }
        }
        if (ImGui::IsItemHovered()) Ui::Tooltip("Brush tip (saved in project)");
        ImGui::SameLine();
        if (ImGui::SmallButton("Load…")) {
            char path[512] = "";
            if (Ui::ShowOpenFile(path, sizeof(path), "Images (*.png;*.jpg;*.bmp;*.tga)\0*.png;*.jpg;*.bmp;*.tga\0All\0*.*\0")) {
                std::vector<uint8_t> px; int tw = 0, th = 0;
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
                            uint8_t r8 = px[si], g8 = px[si + 1], b8 = px[si + 2], a8 = px[si + 3];
                            float lum = (0.2126f * r8 + 0.7152f * g8 + 0.0722f * b8) * (a8 / 255.f);
                            s_CustomTip.pixels[(size_t)y * side + x] = (uint8_t)std::clamp(lum, 0.f, 255.f);
                        }
                    }
                    s_CustomLoaded = true;
                    state.hasCustomBrushTip = true;
                    state.customBrushTipName = path;
                    brush.tipSourcePath = path;
                    state.brushTipPreset = 4;
                    brush.tip = &s_CustomTip;
                    canvas.SetCustomBrushTip(side, s_CustomTip.pixels); // also sets id=custom
                    s_LastSyncedTipId = "custom";
                }
            }
        }
        if (ImGui::IsItemHovered()) Ui::Tooltip("Load grayscale stamp (persisted in .rayp)");
        ImGui::SameLine();

        float maxR = ConfigManager::Get().GetMaxBrushRadius();
        MiniSlider("##rad", &brush.radius, 1.f, maxR, "Radius (px)", 100.f);
        ImGui::SameLine();
        Ui::IconToggle("##pr", "ts_pressure_radius", &brush.pressureRadius, ImVec2(28, 28),
            "Pressure → Radius (on)", "Pressure → Radius (off)");
        ImGui::SameLine();
        MiniSlider("##hrd", &brush.hardness, 0.f, 1.f, "Hardness", 80.f);
        ImGui::SameLine();
        Ui::IconToggle("##ph", "ts_pressure_hardness", &brush.pressureHardness, ImVec2(28, 28),
            "Pressure → Hardness (on)", "Pressure → Hardness (off)");
        ImGui::SameLine();
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
        ImGui::SetNextItemWidth(60.f);
        Ui::SmartSliderInt("##stb", &brush.stabilization, 1, 50, 1, 1);
        if (ImGui::IsItemHovered()) Ui::Tooltip("Stabilization");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.f);
        Ui::SmartSliderFloat("##rot", &brush.rotationDeg, 0.f, 360.f, 0.f, 1.f, "R %.0f°");
        if (ImGui::IsItemHovered())
            Ui::Tooltip("Brush rotation (tip) — Ctrl+Alt+LMB drag in viewport\nDouble-click slider for exact value");

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
            if (ImGui::IsItemActive()) changed = true;
            ImGui::SameLine();
            if (ImGui::Checkbox("##cont", &state.magicWandContiguous)) changed = true;
            if (ImGui::IsItemHovered()) Ui::Tooltip("Contiguous");
            if (changed && canvas.HasWandSeed()) {
                bool add = ImGui::GetIO().KeyCtrl;
                bool subtract = ImGui::GetIO().KeyAlt;
                canvas.PreviewWandFromSeed(device, state.magicWandTolerance, add, subtract, state.magicWandContiguous);
            }
        } else if (activeTool == ActiveTool::QuickSelect) {
            float maxR = ConfigManager::Get().GetMaxBrushRadius();
            MiniSlider("##qsr", &brush.radius, 1.f, maxR, "Quick Select size (same as brush · [ ])", 140.f);
            ImGui::SameLine();
            ImGui::TextDisabled("paint = add · Alt = subtract · live ants");
        } else {
            ImGui::TextDisabled("Smart Select: draw contour");
        }
    }
    else if (activeTool == ActiveTool::BucketFill) {
        MiniSlider("##bft", &state.bucketFillTolerance, 0.f, 1.f, "Fill Tolerance", 140.f);
    }
    else if (IsSelectTool(activeTool) || IsLassoTool(activeTool)) {
        if (activeTool == ActiveTool::PolygonalLasso)
            ImGui::TextDisabled("Click vertices · near start (Ø3px) closes · Enter/Dbl · Esc  ·  Ctrl/Alt");
        else if (activeTool == ActiveTool::RectSelect || activeTool == ActiveTool::EllipseSelect)
            ImGui::TextDisabled("Ctrl: add  ·  Alt: subtract  ·  Shift: 1:1 proportions");
        else
            ImGui::TextDisabled("Ctrl: add  ·  Alt: subtract");
    }
    else if (activeTool == ActiveTool::Gradient) {
        ImGui::TextDisabled("Drag: Primary → Secondary");
    }
    else if (activeTool == ActiveTool::Pipette) {
        ImGui::TextDisabled("Hover: live sample HUD  ·  Click: set primary color  ·  Alt+brush also samples");
    }
    else if (activeTool == ActiveTool::Smudge) {
        MiniSlider("##smr", &state.smudge.radius, 1.f, 150.f, "Smudge Radius", 110.f);
        ImGui::SameLine();
        MiniSlider("##sms", &state.smudge.strength, 0.f, 1.f, "Strength (finger push)", 100.f);
        ImGui::SameLine();
        MiniSlider("##smp", &state.smudge.spacing, 0.01f, 1.f, "Spacing", 90.f);
    }
    else if (activeTool == ActiveTool::BlurTool) {
        MiniSlider("##blr", &state.blurTool.radius, 1.f, 150.f, "Blur brush size", 110.f);
        ImGui::SameLine();
        MiniSlider("##bls", &state.blurTool.strength, 0.f, 1.f, "Strength", 100.f);
        ImGui::SameLine();
        MiniSlider("##blp", &state.blurTool.spacing, 0.01f, 1.f, "Spacing", 90.f);
    }
    else if (activeTool == ActiveTool::MovePixels) {
        if (state.freeTransformActive) {
            // Free Transform operator (Ctrl+T): full params + Enter/OK to apply
            float sx = canvas.GetFloatingScaleX();
            float sy = canvas.GetFloatingScaleY();
            float rotDeg = canvas.GetFloatingRotation() * (180.0f / 3.14159265f);
            ImGui::SetNextItemWidth(80.f);
            if (Ui::SmartSliderFloat("##sx", &sx, 0.05f, 5.f, 1.f, 0.05f, "X:%.2f")) canvas.SetFloatingScaleX(sx);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.f);
            if (Ui::SmartSliderFloat("##sy", &sy, 0.05f, 5.f, 1.f, 0.05f, "Y:%.2f")) canvas.SetFloatingScaleY(sy);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.f);
            if (Ui::SmartSliderFloat("##rot", &rotDeg, -180.f, 180.f, 0.f, 1.f, "%.0f°"))
                canvas.SetFloatingRotation(rotDeg * (3.14159265f / 180.0f));
            ImGui::SameLine();
            if (ImGui::Button("H")) canvas.SetFloatingScaleX(-canvas.GetFloatingScaleX());
            if (ImGui::IsItemHovered()) Ui::Tooltip("Flip H");
            ImGui::SameLine();
            if (ImGui::Button("V##flip")) canvas.SetFloatingScaleY(-canvas.GetFloatingScaleY());
            if (ImGui::IsItemHovered()) Ui::Tooltip("Flip V");
            ImGui::SameLine();
            if (ImGui::Button("Reset")) {
                canvas.SetFloatingScaleX(1.f); canvas.SetFloatingScaleY(1.f); canvas.SetFloatingRotation(0.f);
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.55f, 0.18f, 1.0f));
            if (ImGui::Button("OK")) state.commitTransform = true;
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) Ui::Tooltip("Apply (Enter)");
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.18f, 1.0f));
            if (ImGui::Button("X")) state.cancelTransform = true;
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) Ui::Tooltip("Cancel (Esc) — returns to previous tool");
        } else {
            ImGui::TextDisabled("Move: drag pixels · Esc cancel · switch tool / click away = apply");
            if (canvas.IsMovingPixels()) {
                ImGui::SameLine();
                if (ImGui::Button("Apply##move")) state.commitTransform = true;
                ImGui::SameLine();
                if (ImGui::Button("Cancel##move")) state.cancelTransform = true;
            }
        }
    }
    else {
        ImGui::TextDisabled("Hand: pan · RMB/Shift: rotate");
    }

    Ui::EndDockPanel();
}

} // namespace UI
