#include "LayersPanel.h"
#include <imgui.h>
#include <imgui_internal.h>
#include "../EditorPanels.h"
#include "../FileExplorer.h"
#include "../dialogs/Win32FileDialogs.h"
#include "../widgets/UiPanel.h"
#include "../widgets/UiDropdown.h"
#include "../widgets/UiIconButton.h"
#include "../widgets/UiTooltip.h"
#include "../widgets/UiPathField.h"
#include "../widgets/UiVisualSlider.h"
#include "../widgets/UiColorField.h"
#include "../widgets/UiAssetPicker.h"
#include "../style/UiTokens.h"
#include "../../assets/AssetManager.h"
#include "../../core/ProjectManager.h"
#include "../../core/Logger.h"
#include "../../core/KeymapManager.h"
#include "../../core/UndoRedoManager.h"
#include "../../Canvas.h"
#include "../../texset/TextureSetTypes.h"
#include "../../layer/LayerTypes.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

extern bool g_IsLayersHovered;

namespace UI {

// True if `maybeAncestor` is parent/ancestor of `layerIdx` (cycle guard for drag-into-group).
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

void DrawLayersPanel(UIState& state, Canvas& canvas, ID3D11Device* device) {
    if (!state.showLayers) return;
        // Outer panel never scrolls — only LayersList child does.
        Ui::BeginDockPanel("Layers", &state.showLayers);
        Ui::ClampDockLeafBox(180.f, 520.f, 160.f, 2400.f);
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

        // Fixed top: opacity + blend + FX (Photoshop-like header)
        {
            int ai = canvas.GetActiveLayerIndex();
            if (ai >= 0 && ai < (int)layers.size()) {
                auto& al = layers[ai];
                ImGui::PushID("##active_hdr");
                const float hdrGap = 12.f;
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.30f);
                // Undo: keep last committed opacity/blend per layer for slider/combo sessions
                static int s_propAi = -1;
                static float s_committedOpacity = 1.f;
                static BlendMode s_committedBlend = BlendMode::Normal;
                if (s_propAi != ai) {
                    s_propAi = ai;
                    s_committedOpacity = al.opacity;
                    s_committedBlend = al.blendMode;
                }
                if (Ui::SmartSliderFloat("##op_top", &al.opacity, 0.f, 1.f, 1.f, 0.05f, "Fill %.2f")) {
                    // Content/fill opacity — styles keep independent style.opacity
                    if (al.HasEnabledStyles() || al.isGroup)
                        canvas.RequestPresentationRebuild(ai);
                    else
                        canvas.MarkCompositeDirty();
                }
                if (ImGui::IsItemDeactivatedAfterEdit() && al.opacity != s_committedOpacity) {
                    LayerPropsCommand::Props before = Canvas::CaptureLayerProps(al);
                    before.opacity = s_committedOpacity;
                    canvas.CommitLayerPropsEdit(ai, before, "Layer Opacity");
                    s_committedOpacity = al.opacity;
                }
                ImGui::SameLine(0, hdrGap);
                static const char* blendNamesTop[] = {
                    "Normal","Multiply","Screen","Overlay","Add","Subtract","Darken","Lighten","HardLight","SoftLight"
                };
                int blendIdx = (int)al.blendMode;
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 92.f - hdrGap * 2.f);
                if (Ui::Combo("##bl_top", &blendIdx, blendNamesTop, IM_ARRAYSIZE(blendNamesTop))) {
                    BlendMode newBm = (BlendMode)blendIdx;
                    if (newBm != s_committedBlend) {
                        LayerPropsCommand::Props before = Canvas::CaptureLayerProps(al);
                        before.blendMode = s_committedBlend;
                        al.blendMode = newBm;
                        canvas.CommitLayerPropsEdit(ai, before, "Layer Blend Mode");
                        s_committedBlend = newBm;
                    } else {
                        al.blendMode = newBm;
                    }
                    canvas.MarkCompositeDirty();
                }
                ImGui::SameLine(0, hdrGap);
                bool hasFxTop = !al.filters.empty() || !al.styles.empty();
                if (hasFxTop) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.9f, 0.55f));
                if (ImGui::Button("Fx##top", ImVec2(40, 0))) {
                    state.showLayerEffects = true;
                    // Prefer first style, else first filter
                    if (!al.styles.empty()) {
                        state.layerEffectsSelKind = 0;
                        state.layerEffectsSelIdx = 0;
                        state.layerEffectsFocusIdx = 0;
                    } else if (!al.filters.empty()) {
                        state.layerEffectsSelKind = 1;
                        state.layerEffectsSelIdx = 0;
                        state.layerEffectsFocusIdx = 0;
                    } else {
                        state.layerEffectsSelKind = -1;
                        state.layerEffectsSelIdx = -1;
                        state.layerEffectsFocusIdx = -1;
                    }
                }
                if (hasFxTop) ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    Ui::Tooltip("Layer Effects (filters + styles)…");
                ImGui::SameLine(0, 4);
                {
                    bool fxOn = canvas.GetEffectsPreviewEnabled();
                    if (!fxOn) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.25f, 0.15f, 0.75f));
                    if (ImGui::Button(fxOn ? "FX##fxprev" : "off##fxprev", ImVec2(36, 0))) {
                        canvas.SetEffectsPreviewEnabled(!fxOn);
                        // RefreshCanvas is called inside SetEffectsPreviewEnabled
                    }
                    if (!fxOn) ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered())
                        Ui::Tooltip(fxOn
                            ? "Effects preview ON (CPU bake)\nClick to disable for fast paint"
                            : "Effects preview OFF — raw content\nClick to re-enable bake");
                }

                if (!al.isGroup && !al.IsFill()) {
                    bool ar = al.alphaRewrite;
                    if (ImGui::Checkbox("Alpha Rewrite##ar_top", &ar)) {
                        al.alphaRewrite = ar;
                        canvas.MarkCompositeDirty(); // recompose + paint path
                    }
                    if (ImGui::IsItemHovered()) {
                        Ui::Tooltip(
                            "ON: this layer may overwrite alpha when painted / composited.\n"
                            "OFF: layer A is RGB morph strength only — underlay A never changes\n"
                            "(default for imported decals). Paint keeps layer A non-destructive.");
                    }
                }

                ImGui::PopID();
                ImGui::Separator();

                // Fill Layer: compact map chips with smart wrap
                if (al.IsFill()) {
                    ImGui::PushID("##fill_props");
                    al.fill.EnsureDefaults();
                    const texset::TextureSet* tset = nullptr;
                    if (Project* p = ProjectManager::Get().ActiveProject())
                        tset = p->textureSets.Active();

                    auto dirtyFill = [&](bool needsPresentation = false) {
                        al.needsUpload = true;
                        if (needsPresentation || al.HasEnabledStyles() ||
                            LayerFilterListHasEnabled(al.filters) || al.fill.HasTexture()) {
                            al.presentationDirty = true;
                        }
                        al.SyncWorkSpaceFromFillTarget(tset);
                        if (al.HasEnabledStyles()) canvas.RequestPresentationRebuild(ai);
                        else canvas.MarkCompositeDirty();
                        canvas.SetDocumentModified(true);
                    };

                    ImGui::TextDisabled("Fill maps · swatch opens color · R/G/B/A = write mask");
                    std::vector<texset::MapKind> mapsToShow;
                    if (tset) {
                        for (const auto& m : tset->maps)
                            if (m.enabled || m.kind == texset::MapKind::Diffuse)
                                mapsToShow.push_back(m.kind);
                    } else {
                        mapsToShow.push_back(texset::MapKind::Diffuse);
                    }

                    // Fixed chip size + wrap like ImGui button demo
                    const float chipW = 168.f;
                    const float chipH = 52.f;
                    const float swatch = 20.f;
                    ImGuiStyle& style = ImGui::GetStyle();
                    float winVisibleX2 = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;

                    for (int idx = 0; idx < (int)mapsToShow.size(); ++idx) {
                        texset::MapKind mk = mapsToShow[idx];
                        int mi = (int)mk;
                        auto& mc = al.fill.mapColor[mi];
                        ImGui::PushID(mi);
                        const char* lab = texset::MapKindName(mk);
                        if (tset) {
                            if (const texset::MapSlot* sl = tset->GetMap(mk))
                                lab = sl->DisplayName();
                        }

                        ImGui::BeginChild("##chip", ImVec2(chipW, chipH), true,
                            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

                        if (mc.enabled)
                            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.18f, 0.40f, 0.70f, 0.55f));
                        bool en = mc.enabled;
                        if (ImGui::Checkbox(lab, &en)) {
                            mc.enabled = en;
                            dirtyFill();
                        }
                        if (mc.enabled) ImGui::PopStyleColor();

                        if (mc.enabled) {
                            ImGui::SameLine(0, 4);
                            bool pip = false;
                            if (Ui::ColorField("##fillcol", mc.rgba,
                                    Ui::ColorFieldFlags_FullPicker | Ui::ColorFieldFlags_AlphaBar |
                                    Ui::ColorFieldFlags_Pipette,
                                    nullptr, &pip)) {
                                dirtyFill();
                            }
                            if (pip)
                                UI::ArmFillPipette(ai, mi);
                            if (UI::FillPipetteArmedFor(ai, mi)) {
                                ImGui::SameLine(0, 4);
                                ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.f, 1.f), "…");
                            }
                            (void)swatch;
                            // Write mask row
                            const char* chs[4] = { "R", "G", "B", "A" };
                            for (int c = 0; c < 4; ++c) {
                                ImGui::PushID(c);
                                bool on = mc.WritesChannel(c);
                                if (c > 0) ImGui::SameLine(0, 4);
                                if (ImGui::Checkbox(chs[c], &on)) {
                                    mc.SetChannel(c, on);
                                    dirtyFill();
                                }
                                if (ImGui::IsItemHovered())
                                    Ui::Tooltip(on
                                        ? "ON: write this channel"
                                        : "OFF: leave underlay (no overwrite)");
                                ImGui::PopID();
                            }
                        }
                        ImGui::EndChild();

                        // Wrap to next line only when the next chip would clip
                        float lastX2 = ImGui::GetItemRectMax().x;
                        float nextX2 = lastX2 + style.ItemSpacing.x + chipW;
                        if (idx + 1 < (int)mapsToShow.size() && nextX2 < winVisibleX2)
                            ImGui::SameLine(0, style.ItemSpacing.x);

                        ImGui::PopID();
                    }
                    if (UI::IsFillPipetteArmed() && UI::FillPipetteArmedFor(ai, -1)) {
                        ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.f, 1.f),
                            "Pipette armed — click canvas to sample");
                    }
                    ImGui::TextDisabled("Unchecked R/G/B/A = no write (underlay stays)");

                    bool useTex = al.fill.useTexture;
                    if (ImGui::Checkbox("Use Texture##filltex", &useTex)) {
                        if (!useTex)
                            canvas.BindFillTextureAsset(ai, "");
                        else
                            al.fill.useTexture = true;
                        al.needsUpload = true;
                        canvas.MarkCompositeDirty();
                    }
                    // Consume asset picker result for this fill layer
                    {
                        static int s_FillPickLayer = -1;
                        std::string picked;
                        // Only consume picker when we opened it (avoid stealing Outline picks).
                        if (s_FillPickLayer >= 0 && Ui::AssetPickerResult(picked) && !picked.empty()) {
                            canvas.BindFillTextureAsset(s_FillPickLayer, picked);
                            s_FillPickLayer = -1;
                        }
                        if (al.fill.useTexture || !al.fill.textureAssetKey.empty()) {
                            std::string name = assets::AssetManager::Get().DisplayName(al.fill.textureAssetKey);
                            if (name.empty()) name = al.fill.textureAssetKey.empty() ? "(none)" : al.fill.textureAssetKey;
                            ID3D11ShaderResourceView* thumb =
                                assets::AssetManager::Get().GetThumbSrv(device, al.fill.textureAssetKey, false);
                            if (thumb) {
                                ImGui::Image((ImTextureID)thumb, ImVec2(32, 32));
                                ImGui::SameLine();
                            }
                            ImGui::TextWrapped("%s", name.c_str());
                            auto st = assets::AssetManager::Get().GetLoadState(al.fill.textureAssetKey);
                            if (st == assets::AssetLoadState::Pending)
                                ImGui::TextDisabled("Loading…");
                            else if (st == assets::AssetLoadState::Failed)
                                ImGui::TextColored(ImVec4(1.f, 0.4f, 0.35f, 1.f), "Missing / failed");
                            else if (al.fill.textureW > 0)
                                ImGui::TextDisabled("Texture %dx%d", al.fill.textureW, al.fill.textureH);

                            if (ImGui::Button("Choose Asset…##filltex")) {
                                s_FillPickLayer = ai;
                                assets::AssetFilter f;
                                f.kind = assets::AssetKind::Texture;
                                Ui::OpenAssetPicker(f, "Fill Texture");
                            }
                            ImGui::SameLine();
                            if (ImGui::Button("Import File…##filltex")) {
                                char path[512] = {};
                                if (Ui::ShowOpenFile(path, sizeof(path),
                                    "Images (*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds)\0*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds\0All\0*.*\0")) {
                                    canvas.LoadFillTexture(ai, path);
                                }
                            }
                            ImGui::SameLine();
                            if (ImGui::Button("Clear##filltex"))
                                canvas.BindFillTextureAsset(ai, "");

                            if (Ui::SmartSliderFloat("Scale X##fts", &al.fill.texScale[0], 0.05f, 8.f, 1.f, 0.05f) ||
                                Ui::SmartSliderFloat("Scale Y##fts", &al.fill.texScale[1], 0.05f, 8.f, 1.f, 0.05f) ||
                                Ui::SmartSliderFloat("Off X##fto", &al.fill.texOffset[0], -2.f, 2.f, 0.f, 0.05f) ||
                                Ui::SmartSliderFloat("Off Y##fto", &al.fill.texOffset[1], -2.f, 2.f, 0.f, 0.05f)) {
                                al.needsUpload = true;
                                al.presentationDirty = true;
                                al.presentationCache.reset();
                                canvas.MarkCompositeDirty();
                            }
                        } else if (useTex) {
                            if (ImGui::Button("Choose Asset…##filltex0")) {
                                s_FillPickLayer = ai;
                                assets::AssetFilter f;
                                f.kind = assets::AssetKind::Texture;
                                Ui::OpenAssetPicker(f, "Fill Texture");
                            }
                        }
                    }
                    ImGui::TextDisabled("Paint content blocked — paint the mask to shape fill");
                    ImGui::PopID();
                    ImGui::Separator();
                }
            }
        }

        // Rename state (double-click name zone or F2 while Layers hovered)
        static int s_RenameIdx = -1;
        static char s_RenameBuf[256] = {};
        if (g_IsLayersHovered && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F2)) {
            int ai = canvas.GetActiveLayerIndex();
            if (ai >= 0 && ai < (int)layers.size()) {
                s_RenameIdx = ai;
                std::snprintf(s_RenameBuf, sizeof(s_RenameBuf), "%s", layers[ai].name.c_str());
            }
        }

        // List fills remaining height minus bottom bar (only this region scrolls)
        const float barH = 40.f;
        ImGui::BeginChild("LayersList", ImVec2(0, -barH), true,
            ImGuiWindowFlags_None);
        const float thumb = 28.0f;
        const float eyeSz = 22.0f;
        const float rowPad = 10.0f;
        const float rowH = thumb + 4.f;
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

            const float lineY = ImGui::GetCursorPosY();
            auto alignMid = [&](float h) {
                ImGui::SetCursorPosY(lineY + (rowH - h) * 0.5f);
            };

            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1, 1));

            // Eye
            alignMid(eyeSz);
            {
                bool isIsolated = canvas.IsLayerIsolated(i);
                auto r = Ui::IconButton("##vis", "layer_visible", ImVec2(eyeSz, eyeSz),
                    "Visibility\nAlt+Click: Isolate", true, layer.visible || isIsolated);
                if (r.clicked) {
                    if (ImGui::GetIO().KeyAlt) canvas.ToggleLayerIsolation(i);
                    else { layer.visible = !layer.visible; canvas.MarkCompositeDirty(); }
                }
                if (!layer.visible && !isIsolated) {
                    ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
                    ImGui::GetWindowDrawList()->AddRectFilled(mn, mx, IM_COL32(0, 0, 0, 120), tok.rSm);
                }
            }
            ImGui::SameLine(0, rowPad);

            if (layer.isGroup) {
                float ah = 18.f;
                alignMid(ah);
                if (ImGui::ArrowButton("##exp", layer.groupExpanded ? ImGuiDir_Down : ImGuiDir_Right))
                    layer.groupExpanded = !layer.groupExpanded;
                ImGui::SameLine(0, rowPad);
            }

            // Thumb (fixed size) — use opaque RGB thumb so A=0 buffers stay visible
            alignMid(thumb);
            ID3D11ShaderResourceView* thumbSrv = nullptr;
            if (!layer.isGroup)
                thumbSrv = canvas.GetLayerThumbSRV(device, (int)i, (int)thumb);
            if (!thumbSrv && !layer.isGroup)
                thumbSrv = layer.srv;
            if (!layer.isGroup && thumbSrv) {
                bool isActiveContent = (canvas.GetActiveLayerIndex() == i && canvas.GetPaintTarget() == PaintTarget::LayerContent);
                if (isActiveContent) {
                    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.2f, 0.6f, 1.0f, 1.0f));
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                }
                if (ImGui::ImageButton("##thumb", (ImTextureID)thumbSrv, ImVec2(thumb, thumb), ImVec2(0,0), ImVec2(1,1))) {
                    ImGuiIO& io = ImGui::GetIO();
                    if (io.KeyAlt) {
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
                if (ImGui::IsItemHovered()) Ui::Tooltip("Content\nClick: select  ·  Ctrl: multi  ·  Shift: range\nAlt+Click: select opaque");
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
            } else {
                ImGui::Dummy(ImVec2(thumb, thumb)); // keep column even without srv
            }
            ImGui::SameLine(0, rowPad);

            // Mask column — always reserved for non-groups (fixed width)
            if (!layer.isGroup) {
                alignMid(thumb);
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
                        ImGuiIO& io = ImGui::GetIO();
                        canvas.SetActiveLayerIndex(i);
                        canvas.SetPaintTarget(PaintTarget::LayerMask);
                        setSoleSelection(i);
                        // Alt+Click: load mask → selection (PS-like "load selection from mask")
                        if (io.KeyAlt) {
                            canvas.SelectFromLayerMask(i);
                            canvas.UpdateSelectionMaskTexture(device);
                        }
                    }
                    if (ImGui::IsItemHovered()) {
                        Ui::Tooltip("Layer Mask\nClick: edit mask  ·  Alt+Click: load mask as selection\nRight-click: Apply / Delete");
                    }
                    if (ImGui::BeginPopupContextItem("##maskctx")) {
                        if (ImGui::MenuItem("Apply Mask")) canvas.ApplyLayerMask(i);
                        if (ImGui::MenuItem("Delete Mask")) canvas.DeleteLayerMask(i);
                        ImGui::EndPopup();
                    }
                    ImGui::PopStyleVar();
                    ImGui::PopStyleColor();
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.12f, 0.13f, 0.65f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.18f, 0.20f, 0.85f));
                    if (ImGui::Button("+M##addm", ImVec2(thumb, thumb))) {
                        if (canvas.HasSelection())
                            canvas.CreateLayerMaskFromSelection(device, i);
                        else
                            canvas.CreateLayerMask(device, i);
                        canvas.SetPaintTarget(PaintTarget::LayerMask);
                    }
                    ImGui::PopStyleColor(2);
                    if (ImGui::IsItemHovered()) {
                        Ui::Tooltip(canvas.HasSelection()
                            ? "Add Layer Mask from Selection"
                            : "Add Layer Mask");
                    }
                }
                ImGui::SameLine(0, rowPad);
            }

            ImGui::PopStyleVar(); // FramePadding

            // Name fills rest of row
            const bool isActive = (canvas.GetActiveLayerIndex() == i);
            const bool multiSel = isLayerSelected(i);
            float nameW = ImGui::GetContentRegionAvail().x;
            if (nameW < 48.f) nameW = 48.f;
            alignMid(thumb);

            if (s_RenameIdx == i) {
                ImGui::SetNextItemWidth(nameW);
                ImGui::SetKeyboardFocusHere();
                if (ImGui::InputText("##rename", s_RenameBuf, sizeof(s_RenameBuf),
                        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                    if (s_RenameBuf[0]) layer.name = s_RenameBuf;
                    s_RenameIdx = -1;
                }
                if (!ImGui::IsItemActive() && ImGui::IsMouseClicked(0) && !ImGui::IsItemHovered())
                    s_RenameIdx = -1;
                if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                    s_RenameIdx = -1;
            } else {
                char label[256];
                // Map participation badge (which texture map this layer belongs to)
                auto mapBadge = [&]() -> const char* {
                    if (layer.isGroup) return "";
                    uint32_t mm = layer.workSpace.mapMask;
                    if (mm == 0 || mm == (1u << (uint32_t)texset::MapKind::Diffuse))
                        return layer.IsFill() ? "" : "";
                    // single map?
                    int count = 0; texset::MapKind one = texset::MapKind::Diffuse;
                    for (int k = 0; k < (int)texset::MapKind::Count; ++k) {
                        if (mm & (1u << k)) { ++count; one = (texset::MapKind)k; }
                    }
                    if (count == 1) {
                        switch (one) {
                        case texset::MapKind::Diffuse: return "Diff";
                        case texset::MapKind::LightMap: return "LM";
                        case texset::MapKind::MaterialMap: return "Mat";
                        case texset::MapKind::NormalMap: return "Nrm";
                        case texset::MapKind::GlowMap: return "Glow";
                        case texset::MapKind::ExtraMap: return "Ext";
                        default: return "Map";
                        }
                    }
                    if (count > 1) return "Multi";
                    return "";
                };
                const char* mb = mapBadge();
                if (layer.isGroup) std::snprintf(label, sizeof(label), "[G] %s", layer.name.c_str());
                else if (layer.IsFill()) std::snprintf(label, sizeof(label), "[F%s%s] %s", mb[0]?" ":"", mb, layer.name.c_str());
                else if (layer.type == Layer::Type::VectorSvg) std::snprintf(label, sizeof(label), "[SVG] %s", layer.name.c_str());
                else if (layer.type == Layer::Type::SmartObject) std::snprintf(label, sizeof(label), "[SO] %s", layer.name.c_str());
                else if (mb[0]) std::snprintf(label, sizeof(label), "[%s] %s", mb, layer.name.c_str());
                else std::snprintf(label, sizeof(label), "%s", layer.name.c_str());

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
                if (isActive) {
                    ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
                    ImGui::GetWindowDrawList()->AddRect(mn, mx, tok.ColU32(tok.strokeActive), tok.rSm, 0, 1.5f);
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    s_RenameIdx = i;
                    std::snprintf(s_RenameBuf, sizeof(s_RenameBuf), "%s", layer.name.c_str());
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
                    if (ImGui::MenuItem("Rename", "F2")) {
                        s_RenameIdx = i;
                        std::snprintf(s_RenameBuf, sizeof(s_RenameBuf), "%s", layer.name.c_str());
                    }
                    if (ImGui::MenuItem("Remove from Group", nullptr, false, layer.parentGroupId != -1))
                        canvas.RemoveLayerFromGroup(i);
                    if (ImGui::BeginMenu("Add to Group")) {
                        for (int g = 0; g < (int)layers.size(); ++g) {
                            if (layers[g].isGroup && g != i && ImGui::MenuItem(layers[g].name.c_str()))
                                canvas.AddLayerToGroup(i, g);
                        }
                        ImGui::EndMenu();
                    }
                    {
                        ImGui::Separator();
                        const bool canConvertSO = !layer.isGroup && !layer.IsFill() &&
                            layer.type == Layer::Type::Raster;
                        if (ImGui::MenuItem("Convert to Smart Object", nullptr, false, canConvertSO))
                            canvas.ConvertLayerToSmartObject(device, i);
                        const bool canRast = layer.isGroup || layer.IsFill() ||
                            !layer.filters.empty() || !layer.styles.empty() ||
                            layer.type == Layer::Type::SmartObject || layer.type == Layer::Type::VectorSvg;
                        if (ImGui::MenuItem(layer.isGroup ? "Rasterize Group" : "Rasterize Layer",
                                nullptr, false, canRast))
                            canvas.RasterizeLayer(device, i);
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
                    if (!layer.isGroup && i > 0 && ImGui::MenuItem("Merge Down")) {
                        int r = canvas.MergeLayerDown(device, i);
                        if (r >= 0) setSoleSelection(r);
                    }
                    if (!layer.isGroup) {
                        bool ar = layer.alphaRewrite;
                        if (ImGui::MenuItem("Alpha Rewrite", nullptr, ar)) {
                            layer.alphaRewrite = !ar;
                            canvas.MarkCompositeDirty();
                        }
                        if (ImGui::IsItemHovered()) {
                            Ui::Tooltip("OFF: A = RGB strength only; underlay/layer A preserved.");
                        }
                    }
                    // Channel activity (maps + write mask) — RMB only
                    if (!layer.isGroup && canvas.GetProjectType() != Canvas::ProjectType::Simple) {
                        if (ImGui::BeginMenu("Channel Activity")) {
                            auto& ws = layer.workSpace;
                            const char* mapNames[] = {
                                "Diffuse","LightMap","MaterialMap","NormalMap",
                                "ExtraMap","GlowMap","WengineFX"
                            };
                            for (int mi = 0; mi < (int)texset::MapKind::Count; ++mi) {
                                bool on = ws.AffectsMap((texset::MapKind)mi);
                                if (ImGui::MenuItem(mapNames[mi], nullptr, on)) {
                                    ws.SetMap((texset::MapKind)mi, !on);
                                    canvas.MarkCompositeDirty();
                                    canvas.SetDocumentModified(true);
                                }
                            }
                            ImGui::Separator();
                            ImGui::TextDisabled("Write R G B A");
                            for (int c = 0; c < 4; ++c) {
                                bool on = ws.WritesChan((texset::Chan)c);
                                const char* cl[] = { "R", "G", "B", "A" };
                                if (ImGui::MenuItem(cl[c], nullptr, on)) {
                                    ws.SetWriteChan((texset::Chan)c, !on);
                                    canvas.SetDocumentModified(true);
                                }
                            }
                            ImGui::EndMenu();
                        }
                    }
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
            }

            // Next row
            ImGui::SetCursorPosY(lineY + rowH);
            if (depth > 0) ImGui::Unindent(12.0f * depth);
            ImGui::PopID();
        }
        ImGui::EndChild();

        // Bottom action bar — Add / Group / Duplicate / Merge / Delete
        {
            ImGui::Separator();
            const float iconSz = 32.f;
            auto doAdd = [&]() {
                canvas.CreateNewLayer(device, "Layer " + std::to_string(layers.size() + 1));
                setSoleSelection(canvas.GetActiveLayerIndex());
            };
            auto doAddFill = [&]() {
                canvas.CreateFillLayer(device, "Fill " + std::to_string(layers.size() + 1));
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
            auto doMerge = [&]() {
                auto t = targetsForAction();
                if (t.empty()) return;
                int r = canvas.MergeLayers(device, t);
                if (r >= 0) {
                    sel.clear();
                    setSoleSelection(r);
                }
            };
            auto doDel = [&]() {
                auto t = targetsForAction();
                if (t.empty()) return;
                canvas.DeleteLayers(t);
                sel.clear();
                if (canvas.GetActiveLayerIndex() >= 0)
                    setSoleSelection(canvas.GetActiveLayerIndex());
            };

            float gap = 6.f;
            float total = iconSz * 6 + gap * 5;
            float startX = std::max(0.f, (ImGui::GetContentRegionAvail().x - total) * 0.5f);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + startX);

            if (Ui::IconButton("##addL", "layer_add", ImVec2(iconSz, iconSz), "Add Layer").clicked)
                doAdd();
            ImGui::SameLine(0, gap);
            if (ImGui::Button("Fil##addF", ImVec2(iconSz, iconSz)))
                doAddFill();
            if (ImGui::IsItemHovered()) Ui::Tooltip("Add Fill Layer");
            ImGui::SameLine(0, gap);
            if (Ui::IconButton("##addG", "layer_group_add", ImVec2(iconSz, iconSz), "Add Group").clicked)
                doGroup();
            ImGui::SameLine(0, gap);
            if (Ui::IconButton("##dup", "layer_duplicate", ImVec2(iconSz, iconSz), "Duplicate (Ctrl+J)").clicked)
                doDup();
            ImGui::SameLine(0, gap);
            // Merge uses text button if no dedicated icon
            if (ImGui::Button("Mrg##merge", ImVec2(iconSz, iconSz)))
                doMerge();
            if (ImGui::IsItemHovered())
                Ui::Tooltip("Merge Down / Merge selected (blend modes applied)");
            ImGui::SameLine(0, gap);
            if (Ui::IconButton("##del", "layer_delete", ImVec2(iconSz, iconSz), "Delete selection / active").clicked)
                doDel();
        }

        Ui::EndDockPanel();
}

} // namespace UI
