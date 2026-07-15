#include "LayerEffectsPanel.h"
#include "../EditorPanels.h"
#include "../dialogs/Win32FileDialogs.h"
#include "../widgets/UiDropdown.h"
#include "../widgets/UiVisualSlider.h"
#include "../widgets/UiPathField.h"
#include "../widgets/UiTooltip.h"
#include "../widgets/UiColorField.h"
#include "../../Canvas.h"
#include "../../layer/LayerTypes.h"
#include "../../core/UndoRedoManager.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

extern std::vector<float> Canvas_BuildSplineLUT(const std::vector<std::pair<float, float>>& pts);

namespace UI {

void DrawLayerEffectsPanel(UIState& state, Canvas& canvas) {
// Layer Effects — modal (styles + filters, unified list)
if (state.showLayerEffects)
    ImGui::OpenPopup("Layer Effects##modal");

ImGuiViewport* fxVp = ImGui::GetMainViewport();
ImGui::SetNextWindowPos(fxVp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
ImGui::SetNextWindowSize(ImVec2(620, 480), ImGuiCond_Appearing);
if (ImGui::BeginPopupModal("Layer Effects##modal", &state.showLayerEffects, 0)) {
    int ai = canvas.GetActiveLayerIndex();
    auto& L = canvas.GetLayers();

    static const char* filterTypeNames[] = {"Blur", "HSV", "Curves", "Alpha Invert", "Noise"};
    static const FilterType ftypes[] = {
        FilterType::Blur, FilterType::HSV, FilterType::Curves,
        FilterType::AlphaInvert, FilterType::Noise
    };

    // Undo session: capture props on first edit, commit when interaction ends
    static LayerPropsCommand::Props s_fxBefore;
    static bool s_fxEditing = false;
    static int s_fxLayer = -1;
    auto ensureFxUndoBegin = [&](Layer& layer) {
        int idx = canvas.GetActiveLayerIndex();
        if (!s_fxEditing || s_fxLayer != idx) {
            s_fxBefore = Canvas::CaptureLayerProps(layer);
            s_fxEditing = true;
            s_fxLayer = idx;
        }
    };
    auto commitFxUndo = [&](const char* name) {
        if (!s_fxEditing) return;
        canvas.CommitLayerPropsEdit(s_fxLayer, s_fxBefore, name);
        s_fxEditing = false;
        s_fxLayer = -1;
    };

    auto markStyleDirty = [&](Layer& layer) {
        ensureFxUndoBegin(layer);
        int idx = canvas.GetActiveLayerIndex();
        layer.gpuDisplayKind = 0xFF;
        canvas.RequestPresentationRebuild(idx);
        // Groups: also dirty self if editing group layer
        if (layer.isGroup) {
            layer.filtersDirty = true;
        }
    };
    auto markFilterDirty = [&](Layer& layer) {
        ensureFxUndoBegin(layer);
        layer.filtersDirty = true;
        layer.presentationDirty = true;
        layer.gpuDisplayKind = 0xFF; // force GPU re-upload after filter change
        if (layer.tileCache) layer.tileCache->MarkAllDirty();
        if (layer.isGroup || layer.HasEnabledStyles())
            canvas.RequestPresentationRebuild(canvas.GetActiveLayerIndex());
        else
            canvas.MarkCompositeDirty();
    };
    auto selectStyle = [&](int idx) {
        state.layerEffectsSelKind = 0;
        state.layerEffectsSelIdx = idx;
        state.layerEffectsFocusIdx = idx;
    };
    auto selectFilter = [&](int idx) {
        state.layerEffectsSelKind = 1;
        state.layerEffectsSelIdx = idx;
        state.layerEffectsFocusIdx = idx;
    };
    auto clearFxSel = [&]() {
        state.layerEffectsSelKind = -1;
        state.layerEffectsSelIdx = -1;
        state.layerEffectsFocusIdx = -1;
    };

    if (ai < 0 || ai >= (int)L.size()) {
        ImGui::TextDisabled("No active layer");
    } else {
        Layer& layer = L[ai];

        // Clamp selection if list shrank
        if (state.layerEffectsSelKind == 0 &&
            (state.layerEffectsSelIdx < 0 || state.layerEffectsSelIdx >= (int)layer.styles.size()))
            clearFxSel();
        if (state.layerEffectsSelKind == 1 &&
            (state.layerEffectsSelIdx < 0 || state.layerEffectsSelIdx >= (int)layer.filters.size()))
            clearFxSel();

        ImGui::Text("Layer: %s%s", layer.name.c_str(),
            layer.isGroup ? "  [Group]" : (layer.IsFill() ? "  [Fill]" : ""));
        ImGui::SameLine();
        ImGui::TextDisabled("  Fill opacity does not affect Styles");

        // Toolbar
        if (ImGui::Button("Add##fx_add_btn"))
            ImGui::OpenPopup("##fx_add_popup");
        if (ImGui::BeginPopup("##fx_add_popup")) {
            ImGui::TextDisabled("Styles");
            if (ImGui::MenuItem("Drop Shadow")) {
                int si = canvas.AddLayerStyle(ai, StyleType::Shadow);
                selectStyle(si);
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Outline")) {
                int si = canvas.AddLayerStyle(ai, StyleType::Outline);
                selectStyle(si);
                ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
            ImGui::TextDisabled("Filters");
            for (int ti = 0; ti < 5; ++ti) {
                if (ImGui::MenuItem(filterTypeNames[ti])) {
                    auto before = Canvas::CaptureLayerProps(layer);
                    LayerFilter nf;
                    nf.type = ftypes[ti];
                    nf.enabled = true;
                    if (ftypes[ti] == FilterType::Blur) nf.p[0] = 5.f;
                    if (ftypes[ti] == FilterType::Curves) {
                        nf.lut.resize(256);
                        for (int li = 0; li < 256; ++li) nf.lut[li] = (float)li / 255.f;
                        nf.curvesChannels = 0x7; // RGB on, A off
                        nf.curvePts[0] = {{0.f, 0.f}, {1.f, 1.f}};
                    }
                    layer.filters.push_back(std::move(nf));
                    markFilterDirty(layer);
                    canvas.CommitLayerPropsEdit(ai, before, "Add Filter");
                    s_fxEditing = false;
                    selectFilter((int)layer.filters.size() - 1);
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }

        ImGui::SameLine();
        const bool canDelete =
            (state.layerEffectsSelKind == 0 && state.layerEffectsSelIdx >= 0 &&
             state.layerEffectsSelIdx < (int)layer.styles.size()) ||
            (state.layerEffectsSelKind == 1 && state.layerEffectsSelIdx >= 0 &&
             state.layerEffectsSelIdx < (int)layer.filters.size());
        if (!canDelete) ImGui::BeginDisabled();
        if (ImGui::Button("Delete##fx_del_btn") && canDelete) {
            if (state.layerEffectsSelKind == 0) {
                canvas.RemoveLayerStyle(ai, state.layerEffectsSelIdx);
                if (layer.styles.empty()) clearFxSel();
                else selectStyle(std::min(state.layerEffectsSelIdx, (int)layer.styles.size() - 1));
            } else {
                auto before = Canvas::CaptureLayerProps(layer);
                layer.filters.erase(layer.filters.begin() + state.layerEffectsSelIdx);
                markFilterDirty(layer);
                canvas.CommitLayerPropsEdit(ai, before, "Remove Filter");
                s_fxEditing = false;
                if (layer.filters.empty()) clearFxSel();
                else selectFilter(std::min(state.layerEffectsSelIdx, (int)layer.filters.size() - 1));
            }
        }
        if (!canDelete) ImGui::EndDisabled();

        ImGui::Separator();

        // Left list | right params
        const float listW = 210.f;
        ImGui::BeginChild("##fx_list_panel", ImVec2(listW, -40.f), true);

        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted("STYLES");
        ImGui::PopStyleColor();
        if (layer.styles.empty()) {
            ImGui::TextDisabled("  (none)");
        }
        for (int si = 0; si < (int)layer.styles.size(); ++si) {
            ImGui::PushID(0x10000 + si);
            LayerStyle& st = layer.styles[si];
            bool en = st.enabled;
            if (ImGui::Checkbox("##st_en", &en)) {
                st.enabled = en;
                markStyleDirty(layer);
            }
            ImGui::SameLine();
            const char* sn = (st.type == StyleType::Outline) ? "Outline" : "Drop Shadow";
            bool sel = (state.layerEffectsSelKind == 0 && state.layerEffectsSelIdx == si);
            if (ImGui::Selectable(sn, sel, ImGuiSelectableFlags_SpanAvailWidth))
                selectStyle(si);
            ImGui::PopID();
        }

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted("FILTERS");
        ImGui::PopStyleColor();
        if (layer.filters.empty()) {
            ImGui::TextDisabled("  (none)");
        }
        for (int fi = 0; fi < (int)layer.filters.size(); ++fi) {
            ImGui::PushID(0x20000 + fi);
            LayerFilter& flt = layer.filters[fi];
            bool en = flt.enabled;
            if (ImGui::Checkbox("##ft_en", &en)) {
                flt.enabled = en;
                markFilterDirty(layer);
            }
            ImGui::SameLine();
            int tidx = (int)flt.type;
            if (tidx < 0 || tidx > 4) tidx = 0;
            bool sel = (state.layerEffectsSelKind == 1 && state.layerEffectsSelIdx == fi);
            if (ImGui::Selectable(filterTypeNames[tidx], sel, ImGuiSelectableFlags_SpanAvailWidth))
                selectFilter(fi);
            ImGui::PopID();
        }
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("##fx_params_panel", ImVec2(0, -40.f), true);
        bool dirty = false;

        if (state.layerEffectsSelKind == 0 &&
            state.layerEffectsSelIdx >= 0 &&
            state.layerEffectsSelIdx < (int)layer.styles.size()) {
            LayerStyle& st = layer.styles[state.layerEffectsSelIdx];
            if (st.type == StyleType::Shadow) {
                ImGui::TextUnformatted("Drop Shadow");
                ImGui::Separator();
                dirty |= Ui::SmartSliderFloat("Opacity##sh", &st.opacity, 0.f, 1.f, 0.75f, 0.05f);
                dirty |= Ui::ColorField("Color##sh", st.shadowColor,
                    Ui::ColorFieldFlags_NoInputs | Ui::ColorFieldFlags_AlphaBar);
                dirty |= Ui::SmartSliderFloat("Distance##sh", &st.distance, 0.f, 200.f, 8.f, 1.f, "%.0f");
                dirty |= Ui::SmartSliderFloat("Angle##sh", &st.angleDeg, 0.f, 360.f, 120.f, 1.f, "%.0f");
                dirty |= Ui::SmartSliderFloat("Offset X##sh", &st.offsetX, -200.f, 200.f, 0.f, 1.f, "%.0f");
                dirty |= Ui::SmartSliderFloat("Offset Y##sh", &st.offsetY, -200.f, 200.f, 0.f, 1.f, "%.0f");
                dirty |= Ui::SmartSliderFloat("Spread##sh", &st.spread, 0.f, 100.f, 0.f, 1.f, "%.0f");
                dirty |= Ui::SmartSliderFloat("Size##sh", &st.size, 0.f, 100.f, 8.f, 0.5f, "%.1f");
                ImGui::TextDisabled("Independent of layer Fill opacity");
            } else {
                ImGui::TextUnformatted("Outline");
                ImGui::Separator();
                dirty |= Ui::SmartSliderFloat("Opacity##ol", &st.opacity, 0.f, 1.f, 1.f, 0.05f);
                dirty |= Ui::ColorField("Tint##ol", st.outlineColor,
                    Ui::ColorFieldFlags_NoInputs | Ui::ColorFieldFlags_AlphaBar);
                dirty |= Ui::SmartSliderFloat("Size##ol", &st.outlineSize, 0.f, 100.f, 2.f, 0.5f, "%.1f");
                int pos = (int)st.outlinePos;
                const char* posNames[] = {"Outside", "Inside", "Center"};
                if (Ui::Combo("##ol_pos", &pos, posNames, 3, "Position")) {
                    st.outlinePos = (OutlinePosition)pos;
                    dirty = true;
                }
                int fm = (int)st.outlineFill;
                const char* fillNames[] = {"Solid", "Gradient", "Texture"};
                if (Ui::Combo("##ol_fill", &fm, fillNames, 3, "Fill Mode")) {
                    st.outlineFill = (OutlineFillMode)fm;
                    if (st.outlineFill == OutlineFillMode::Gradient && st.outlineGradient.size() < 2) {
                        st.outlineGradient = {
                            {0.f, {st.outlineColor[0], st.outlineColor[1], st.outlineColor[2], 1.f}},
                            {1.f, {0.f, 0.f, 0.f, 0.f}}
                        };
                    }
                    dirty = true;
                }
                if (st.outlineFill == OutlineFillMode::Gradient) {
                    int gmap = (int)st.outlineGradientMap;
                    const char* gmaps[] = {"Edge distance", "Horizontal", "Vertical"};
                    if (Ui::Combo("##ol_gmap", &gmap, gmaps, 3, "Gradient Map")) {
                        st.outlineGradientMap = (uint8_t)gmap;
                        dirty = true;
                    }
                    if (st.outlineGradient.size() < 2) {
                        st.outlineGradient = {
                            {0.f, {1.f, 1.f, 1.f, 1.f}},
                            {1.f, {0.f, 0.f, 0.f, 1.f}}
                        };
                    }
                    ImGui::Text("Stop 0");
                    dirty |= Ui::ColorField("##gs0", st.outlineGradient[0].rgba, Ui::ColorFieldFlags_AlphaBar);
                    dirty |= Ui::SmartSliderFloat("t0##gs", &st.outlineGradient[0].t, 0.f, 1.f, 0.f, 0.05f);
                    ImGui::Text("Stop 1");
                    dirty |= Ui::ColorField("##gs1", st.outlineGradient[1].rgba, Ui::ColorFieldFlags_AlphaBar);
                    dirty |= Ui::SmartSliderFloat("t1##gs", &st.outlineGradient[1].t, 0.f, 1.f, 1.f, 0.05f);
                }
                if (st.outlineFill == OutlineFillMode::Texture) {
                    static char olTexPath[512] = {};
                    if (st.outlineTexturePath.size() < sizeof(olTexPath))
                        std::snprintf(olTexPath, sizeof(olTexPath), "%s", st.outlineTexturePath.c_str());
                    if (Ui::PathField("##oltex", "Texture", olTexPath, sizeof(olTexPath),
                            Ui::ShowOpenFile,
                            "Images (*.png;*.jpg;*.jpeg;*.tga;*.bmp)\0*.png;*.jpg;*.jpeg;*.tga;*.bmp\0All\0*.*\0",
                            "Image for outline fill")) {
                        canvas.LoadOutlineTexture(ai, state.layerEffectsSelIdx, olTexPath);
                    }
                    dirty |= Ui::SmartSliderFloat("Tex Scale X##ol", &st.outlineTexScale[0], 0.05f, 8.f, 1.f, 0.05f);
                    dirty |= Ui::SmartSliderFloat("Tex Scale Y##ol", &st.outlineTexScale[1], 0.05f, 8.f, 1.f, 0.05f);
                    dirty |= Ui::SmartSliderFloat("Tex Off X##ol", &st.outlineTexOffset[0], -2.f, 2.f, 0.f, 0.05f);
                    dirty |= Ui::SmartSliderFloat("Tex Off Y##ol", &st.outlineTexOffset[1], -2.f, 2.f, 0.f, 0.05f);
                    if (st.outlineTextureW > 0)
                        ImGui::TextDisabled("Loaded %dx%d", st.outlineTextureW, st.outlineTextureH);
                }
                ImGui::TextDisabled("Independent of layer Fill opacity");
            }
            if (dirty) markStyleDirty(layer);
        } else if (state.layerEffectsSelKind == 1 &&
                   state.layerEffectsSelIdx >= 0 &&
                   state.layerEffectsSelIdx < (int)layer.filters.size()) {
            int fi = state.layerEffectsSelIdx;
            LayerFilter& flt = layer.filters[fi];
            int tidx = (int)flt.type;
            if (tidx < 0 || tidx > 4) tidx = 0;
            ImGui::TextUnformatted(filterTypeNames[tidx]);
            ImGui::Separator();
            switch (flt.type) {
            case FilterType::Blur:
                dirty |= Ui::SmartSliderFloat("Radius##ft", &flt.p[0], 0.5f, 80.f, 5.f, 0.5f, "%.1f");
                break;
            case FilterType::HSV:
                dirty |= Ui::SmartSliderFloat("Hue##ft", &flt.p[0], -0.5f, 0.5f, 0.f, 0.05f);
                dirty |= Ui::SmartSliderFloat("Sat##ft", &flt.p[1], -1.f, 1.f, 0.f, 0.05f);
                dirty |= Ui::SmartSliderFloat("Val##ft", &flt.p[2], -1.f, 1.f, 0.f, 0.05f);
                break;
            case FilterType::Noise:
                dirty |= Ui::SmartSliderFloat("Strength##ft", &flt.p[0], 0.f, 1.f, 0.1f, 0.05f);
                {
                    bool col = flt.p[1] > 0.5f;
                    if (ImGui::Checkbox("Color noise##ft", &col)) {
                        flt.p[1] = col ? 1.f : 0.f;
                        dirty = true;
                    }
                }
                break;
            case FilterType::AlphaInvert:
                ImGui::TextDisabled("Inverts alpha — no parameters");
                break;
            case FilterType::Curves: {
                auto chToggle = [&](const char* label, int bit) {
                    bool on = (flt.curvesChannels & (1u << bit)) != 0;
                    if (ImGui::Checkbox(label, &on)) {
                        if (on) flt.curvesChannels |= (uint8_t)(1u << bit);
                        else flt.curvesChannels &= (uint8_t)~(1u << bit);
                        dirty = true;
                    }
                };
                chToggle("R##cv", 0); ImGui::SameLine();
                chToggle("G##cv", 1); ImGui::SameLine();
                chToggle("B##cv", 2); ImGui::SameLine();
                chToggle("A##cv", 3);
                ImGui::TextDisabled("A is off by default");

                // Persist points on the filter itself
                auto& pts = flt.curvePts[0];
                if (pts.empty()) pts = {{0.f, 0.f}, {1.f, 1.f}};
                if (flt.lut.size() < 256)
                    flt.lut = Canvas_BuildSplineLUT(pts);

                const float gsz = 220.f;
                ImVec2 gp = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton("##fxcurve", ImVec2(gsz, gsz));
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(gp, ImVec2(gp.x + gsz, gp.y + gsz), IM_COL32(28, 28, 30, 255));
                dl->AddRect(gp, ImVec2(gp.x + gsz, gp.y + gsz), IM_COL32(100, 100, 110, 255));
                auto lut = Canvas_BuildSplineLUT(pts);
                for (int xi = 0; xi < 255; ++xi) {
                    float x0 = gp.x + gsz * xi / 255.f, x1 = gp.x + gsz * (xi + 1) / 255.f;
                    float y0 = gp.y + gsz * (1.f - lut[xi]), y1 = gp.y + gsz * (1.f - lut[xi + 1]);
                    dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(220, 220, 230, 255), 1.5f);
                }
                ImVec2 mpos = ImGui::GetIO().MousePos;
                static int dragPt = -1;
                for (int pi = 0; pi < (int)pts.size(); ++pi) {
                    float cx = gp.x + pts[pi].first * gsz;
                    float cy = gp.y + (1.f - pts[pi].second) * gsz;
                    bool hov = fabsf(mpos.x - cx) < 7.f && fabsf(mpos.y - cy) < 7.f;
                    dl->AddCircleFilled(ImVec2(cx, cy), 5.f,
                        hov ? IM_COL32(255, 200, 100, 255) : IM_COL32(200, 200, 210, 255));
                    if (hov && ImGui::IsMouseClicked(0)) dragPt = pi;
                    if (hov && ImGui::IsMouseClicked(1) && pi > 0 && pi < (int)pts.size() - 1) {
                        pts.erase(pts.begin() + pi);
                        dirty = true;
                        break;
                    }
                }
                if (dragPt >= 0 && ImGui::IsMouseDown(0)) {
                    float nx = std::clamp((mpos.x - gp.x) / gsz, 0.f, 1.f);
                    float ny = std::clamp(1.f - (mpos.y - gp.y) / gsz, 0.f, 1.f);
                    if (dragPt == 0) nx = 0.f;
                    if (dragPt == (int)pts.size() - 1) nx = 1.f;
                    pts[dragPt] = {nx, ny};
                    std::sort(pts.begin(), pts.end(),
                        [](const auto& a, const auto& b) { return a.first < b.first; });
                    dirty = true;
                } else if (!ImGui::IsMouseDown(0)) {
                    dragPt = -1;
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0) && dragPt < 0) {
                    float nx = std::clamp((mpos.x - gp.x) / gsz, 0.f, 1.f);
                    float ny = std::clamp(1.f - (mpos.y - gp.y) / gsz, 0.f, 1.f);
                    pts.push_back({nx, ny});
                    std::sort(pts.begin(), pts.end(),
                        [](const auto& a, const auto& b) { return a.first < b.first; });
                    dirty = true;
                }
                if (dirty)
                    flt.lut = Canvas_BuildSplineLUT(pts);
                ImGui::TextDisabled("RMB remove point · drag to edit");
                break;
            }
            }
            if (dirty) markFilterDirty(layer);
        } else {
            ImGui::TextDisabled("Select a style or filter from the list");
            ImGui::TextDisabled("or use Add to create one.");
        }
        ImGui::EndChild();

        // Commit undo when user finishes dragging a control
        if (s_fxEditing && ImGui::IsMouseReleased(0))
            commitFxUndo("Edit Layer Effects");
    }

    ImGui::Separator();
    if (ImGui::Button("Close##fx_close", ImVec2(120, 0))) {
        commitFxUndo("Edit Layer Effects");
        state.showLayerEffects = false;
        ImGui::CloseCurrentPopup();
    }
    // If modal closed via X without Close button
    if (!state.showLayerEffects)
        commitFxUndo("Edit Layer Effects");

    ImGui::EndPopup();
    } // BeginPopupModal

} // DrawLayerEffectsPanel

} // namespace UI
