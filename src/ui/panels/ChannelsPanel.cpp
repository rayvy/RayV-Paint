#include "ChannelsPanel.h"
#include <imgui.h>
#include <imgui_internal.h>
#include "../EditorPanels.h"
#include "../FileExplorer.h"
#include "../widgets/UiPanel.h"
#include "../widgets/UiTooltip.h"
#include "../style/UiTokens.h"
#include "../../core/ProjectManager.h"
#include "../../texset/TextureSetTypes.h"
#include <algorithm>
#include <cstdio>

namespace UI {

void DrawChannelsPanel(UIState& state, Canvas& canvas, ID3D11Device* device) {
    if (!state.showChannels) return;
        Ui::BeginDockPanel("Channels", &state.showChannels);

        Project* proj = ProjectManager::Get().ActiveProject();
        auto ptype = canvas.GetProjectType();
        const bool advanced = (ptype != Canvas::ProjectType::Simple);
        if (proj) proj->textureSets.EnsureSimpleDefault();
        texset::TextureSet* set = proj ? proj->textureSets.Active() : nullptr;

        // ---- TOP: compact map switchers (Advanced) ----
        if (advanced && set) {
            float avail = ImGui::GetContentRegionAvail().x;
            int nMaps = 0;
            for (const auto& m : set->maps)
                if (m.enabled || m.kind == texset::MapKind::Diffuse) ++nMaps;
            nMaps = std::max(1, nMaps);
            float bw = std::max(48.f, (avail - ImGui::GetStyle().ItemSpacing.x * (nMaps - 1)) / nMaps);
            int ci = 0;
            for (auto& m : set->maps) {
                if (!m.enabled && m.kind != texset::MapKind::Diffuse) continue;
                ImGui::PushID((int)m.kind);
                bool on = canvas.GetViewMapKind() == m.kind;
                if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.50f, 0.85f, 0.9f));
                if (ImGui::Button(m.DisplayName(), ImVec2(bw, 22))) {
                    set->activeMap = m.kind;
                    canvas.SetViewMapKind(m.kind);
                    canvas.MarkCompositeDirty();
                }
                if (on) ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) {
                    char tip[96];
                    std::snprintf(tip, sizeof(tip), "%s  %dx%d",
                        m.DisplayName(),
                        m.width > 0 ? m.width : canvas.GetWidth(),
                        m.height > 0 ? m.height : canvas.GetHeight());
                    Ui::Tooltip(tip);
                }
                ImGui::PopID();
                ++ci;
                if (ci < nMaps) ImGui::SameLine(0, 4);
            }
            ImGui::Spacing();
            ImGui::Separator();
        }

        // ---- R G B A thumbs ----
        bool r = canvas.GetChannelR();
        bool g = canvas.GetChannelG();
        bool b = canvas.GetChannelB();
        bool a = canvas.GetChannelA();
        auto& tok = Ui::Tokens();
        float availX = ImGui::GetContentRegionAvail().x;
        float gap = 6.f;
        float thumb = std::clamp((availX - gap * 3.f) / 4.f, 36.f, 64.f);
        // Soft pack labels (hints only)
        const char* roleUnder[4] = { "R", "G", "B", "A" };
        if (advanced && set) {
            if (const texset::MapSlot* slot = set->GetMap(canvas.GetViewMapKind())) {
                for (int i = 0; i < 4; ++i) {
                    if (slot->pack[i].role != texset::ChannelRole::None)
                        roleUnder[i] = texset::ChannelRoleShortName(slot->pack[i].role);
                    else
                        roleUnder[i] = (i == 0 ? "R" : i == 1 ? "G" : i == 2 ? "B" : "A");
                }
            }
        }
        struct Ch { const char* name; bool* flag; Canvas::ChannelPreview preview; const char* role; };
        Ch chans[] = {
            { "R", &r, Canvas::ChannelPreview::R, roleUnder[0] },
            { "G", &g, Canvas::ChannelPreview::G, roleUnder[1] },
            { "B", &b, Canvas::ChannelPreview::B, roleUnder[2] },
            { "A", &a, Canvas::ChannelPreview::A, roleUnder[3] },
        };
        const ImVec4 chTint[] = {
            ImVec4(1.f, 0.2f, 0.2f, 1.f), ImVec4(0.2f, 1.f, 0.2f, 1.f),
            ImVec4(0.3f, 0.5f, 1.f, 1.f), ImVec4(1.f, 1.f, 1.f, 1.f),
        };
        for (int i = 0; i < 4; ++i) {
            ImGui::PushID(i);
            if (i) ImGui::SameLine(0, gap);
            ImGui::BeginGroup();
            ImVec2 t0 = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##ch", ImVec2(thumb, thumb));
            bool hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) *chans[i].flag = !*chans[i].flag;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 t1(t0.x + thumb, t0.y + thumb);
            const bool on = *chans[i].flag;
            dl->AddRectFilled(t0, t1, tok.ColU32(ImVec4(0.04f, 0.04f, 0.05f, 1.f)), tok.rSm);
            if (on || hovered)
                dl->AddRect(t0, t1, tok.ColU32(on ? tok.strokeActive : tok.strokeHairline), tok.rSm, 0, on ? 1.75f : 1.f);
            ID3D11ShaderResourceView* prev = canvas.GetChannelPreviewSRV(device, chans[i].preview);
            ImVec4 tint = chTint[i];
            if (!on) tint.w = 0.4f;
            if (prev)
                dl->AddImage((ImTextureID)prev, ImVec2(t0.x + 2, t0.y + 2), ImVec2(t1.x - 2, t1.y - 2),
                    ImVec2(0, 0), ImVec2(1, 1), ImGui::ColorConvertFloat4ToU32(tint));
            if (!on) {
                float m = thumb * 0.22f;
                dl->AddLine(ImVec2(t0.x + m, t0.y + m), ImVec2(t1.x - m, t1.y - m), IM_COL32(255,255,255,50), 2.f);
                dl->AddLine(ImVec2(t1.x - m, t0.y + m), ImVec2(t0.x + m, t1.y - m), IM_COL32(255,255,255,50), 2.f);
            }
            float tw = ImGui::CalcTextSize(chans[i].role).x;
            ImGui::SetCursorScreenPos(ImVec2(t0.x + (thumb - tw) * 0.5f, t1.y + 2.f));
            ImGui::TextUnformatted(chans[i].role);
            ImGui::Dummy(ImVec2(thumb, ImGui::GetTextLineHeight() + 2.f));
            ImGui::EndGroup();
            if (hovered) {
                char tip[80];
                std::snprintf(tip, sizeof(tip), "%s channel  (label: %s)", chans[i].name, chans[i].role);
                Ui::Tooltip(tip);
            }
            ImGui::PopID();
        }
        canvas.SetChannelR(r);
        canvas.SetChannelG(g);
        canvas.SetChannelB(b);
        canvas.SetChannelA(a);

        if (advanced)
            ImGui::TextDisabled("Maps switch above В· Setup in header");

        Ui::EndDockPanel();
    (void)device;
}

} // namespace UI
