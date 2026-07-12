#include "ProjectSetupPanel.h"
#include "../EditorPanels.h"
#include "../FileExplorer.h"
#include "../widgets/UiDropdown.h"
#include "../widgets/UiTooltip.h"
#include "../../core/ProjectManager.h"
#include "../../texset/TextureSetTypes.h"
#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace UI {

void DrawProjectSetupPanel(UIState& state, Canvas& canvas, ID3D11Device* device) {
    (void)device;
// ---- Project Setup (non-modal window — modal blocked nested File Explorer) ----
if (state.openProjectSetup) {
    state.showProjectSetup = true;
    state.openProjectSetup = false;
}
if (state.showProjectSetup) {
    ImGui::SetNextWindowSize(ImVec2(560, 480), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Project Setup", &state.showProjectSetup, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
    } else {
    Project* proj = ProjectManager::Get().ActiveProject();
    texset::TextureSet* set = proj ? proj->textureSets.Active() : nullptr;

    // Active map switcher INSIDE setup (no need to leave)
    if (set) {
        ImGui::TextDisabled("Active map (viewport)");
        for (auto& m : set->maps) {
            if (!m.enabled && m.kind != texset::MapKind::Diffuse) continue;
            ImGui::PushID(9000 + (int)m.kind);
            bool on = canvas.GetViewMapKind() == m.kind;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.85f, 0.9f));
            if (ImGui::SmallButton(m.DisplayName())) {
                set->activeMap = m.kind;
                canvas.SetViewMapKind(m.kind);
                canvas.MarkCompositeDirty();
            }
            if (on) ImGui::PopStyleColor();
            ImGui::SameLine(0, 4);
            ImGui::PopID();
        }
        ImGui::NewLine();
        ImGui::Separator();
    }

    if (ImGui::BeginTabBar("##setupTabs")) {
        if (ImGui::BeginTabItem("Maps")) {
            if (set) {
                if (ImGui::BeginTable("##maptbl", 5,
                        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 28);
                    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("W", ImGuiTableColumnFlags_WidthFixed, 72);
                    ImGui::TableSetupColumn("H", ImGuiTableColumnFlags_WidthFixed, 72);
                    ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 90);
                    ImGui::TableHeadersRow();
                    for (auto& m : set->maps) {
                        ImGui::PushID((int)m.kind);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        bool en = m.enabled;
                        if (ImGui::Checkbox("##en", &en)) {
                            if (en) {
                                int w = m.width > 0 ? m.width : canvas.GetWidth();
                                int h = m.height > 0 ? m.height : canvas.GetHeight();
                                set->EnableMap(m.kind, w, h);
                            } else set->DisableMap(m.kind);
                            canvas.SetDocumentModified(true);
                        }
                        ImGui::TableNextColumn();
                        char nameBuf[64];
                        std::snprintf(nameBuf, sizeof(nameBuf), "%s", m.DisplayName());
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::InputText("##nm", nameBuf, sizeof(nameBuf))) {
                            m.displayName = nameBuf;
                            canvas.SetDocumentModified(true);
                        }
                        ImGui::TableNextColumn();
                        int mw = m.width > 0 ? m.width : canvas.GetWidth();
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::InputInt("##w", &mw, 0, 0)) {
                            m.width = std::clamp(mw, 1, 16384);
                            canvas.SetDocumentModified(true);
                        }
                        ImGui::TableNextColumn();
                        int mh = m.height > 0 ? m.height : canvas.GetHeight();
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::InputInt("##h", &mh, 0, 0)) {
                            m.height = std::clamp(mh, 1, 16384);
                            canvas.SetDocumentModified(true);
                        }
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("%s", texset::MapKindName(m.kind));
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::Spacing();
                const char* temps[] = { "Default", "ZZZ", "GI" };
                int ti = 0;
                if (set->templateId == "ZZZ") ti = 1;
                else if (set->templateId == "GI") ti = 2;
                ImGui::SetNextItemWidth(160);
                if (Ui::Combo("##ts_template", &ti, temps, 3, "Template")) {
                    if (proj) proj->ApplyActiveSetTemplate(temps[ti]);
                    canvas.SetDocumentModified(true);
                }
                if (ImGui::Button("Import maps…")) {
                    // Non-modal Setup + non-modal Explorer → both usable
                    UI::FileExplorerOpen(state.fileExplorer, UI::FileExplorerMode::ImportTexture);
                }
            } else ImGui::TextDisabled("No texture set");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Labels")) {
            ImGui::TextWrapped("Optional labels for 3D. Paint always uses RGBA.");
            if (set) {
                // Map picker for which map's labels we edit
                texset::MapSlot* slot = set->GetMap(canvas.GetViewMapKind());
                if (!slot) {
                    for (auto& m : set->maps) if (m.enabled) { slot = &m; break; }
                }
                if (slot) {
                    ImGui::Text("Editing labels for: %s", slot->DisplayName());
                    if (ImGui::BeginTable("##lab", 2, ImGuiTableFlags_SizingFixedFit)) {
                        ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed, 24);
                        ImGui::TableSetupColumn("Role", ImGuiTableColumnFlags_WidthFixed, 160);
                        const char* chLab[4] = { "R", "G", "B", "A" };
                        for (int c = 0; c < 4; ++c) {
                            ImGui::PushID(c);
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(chLab[c]);
                            ImGui::TableNextColumn();
                            int ri = texset::ChannelRoleToComboIndex(slot->pack[c].role);
                            ImGui::SetNextItemWidth(150);
                            if (Ui::Combo("##r", &ri, texset::ChannelRoleComboNames(),
                                        texset::ChannelRoleComboCount())) {
                                slot->pack[c].role = texset::ChannelRoleFromComboIndex(ri);
                                canvas.SetDocumentModified(true);
                            }
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                }
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Export")) {
            if (set) {
                if (ImGui::BeginTable("##exptbl", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("Map", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Space", ImGuiTableColumnFlags_WidthFixed, 80);
                    ImGui::TableSetupColumn("Codec", ImGuiTableColumnFlags_WidthFixed, 100);
                    ImGui::TableSetupColumn("Mips", ImGuiTableColumnFlags_WidthFixed, 40);
                    ImGui::TableHeadersRow();
                    for (auto& m : set->maps) {
                        if (!m.enabled) continue;
                        ImGui::PushID(300 + (int)m.kind);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(m.DisplayName());
                        ImGui::TableNextColumn();
                        int cs = (m.colorSpace == texset::MapColorSpace::sRGB) ? 0 : 1;
                        const char* css[] = { "sRGB", "Linear" };
                        ImGui::SetNextItemWidth(-1);
                        if (Ui::Combo("##cs", &cs, css, 2)) {
                            m.colorSpace = cs == 0 ? texset::MapColorSpace::sRGB : texset::MapColorSpace::Linear;
                            canvas.SetDocumentModified(true);
                        }
                        ImGui::TableNextColumn();
                        const char* codecs[] = { "PNG", "BC7_sRGB", "BC7", "BC5", "R8G8", "R32", "RGBA8" };
                        int ci = (int)m.exportCodec;
                        if (ci < 0 || ci > 6) ci = 0;
                        ImGui::SetNextItemWidth(-1);
                        if (Ui::Combo("##codec", &ci, codecs, 7)) {
                            m.exportCodec = (texset::MapExportCodec)ci;
                            canvas.SetDocumentModified(true);
                        }
                        ImGui::TableNextColumn();
                        ImGui::Checkbox("##mips", &m.exportMips);
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::Spacing();
                if (ImGui::Button("Choose export folder…"))
                    UI::FileExplorerOpen(state.fileExplorer, UI::FileExplorerMode::ExportTemplate);
                ImGui::SameLine();
                if (ImGui::Button("Export All Now"))
                    state.openQuickExportTrigger = true;
                ImGui::TextDisabled("Pick a folder (not a file). BC7/BC5 → .dds · PNG → .png");
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(120, 0)))
        state.showProjectSetup = false;
    ImGui::End();
    } // else Begin
}
}

} // namespace UI
