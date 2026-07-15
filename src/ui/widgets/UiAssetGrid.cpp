#include "UiAssetGrid.h"
#include "../style/UiTokens.h"
#include "../../assets/AssetManager.h"
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

namespace Ui {

bool AssetGrid(const char* id, AssetGridState& st, ID3D11Device* device,
               const assets::AssetFilter& baseFilter,
               const std::vector<assets::AssetInfo>& items,
               const std::function<void(const std::string& key)>& onActivate) {
    const auto& T = Ui::Tokens();
    bool activated = false;
    ImGui::PushID(id);

    // Search
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##search", "Search assets…", st.search, sizeof(st.search));

    // Category tabs
    const char* tabs[] = { "All", "Core", "User", "Project" };
    for (int i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine();
        bool sel = (st.categoryTab == i);
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, T.accent);
        if (ImGui::SmallButton(tabs[i])) st.categoryTab = i;
        if (sel) ImGui::PopStyleColor();
    }

    // Filter items client-side for tab + search
    std::vector<const assets::AssetInfo*> view;
    view.reserve(items.size());
    std::string q = st.search;
    for (char& c : q) c = (char)std::tolower((unsigned char)c);
    for (const auto& e : items) {
        if (baseFilter.kind != assets::AssetKind::Unknown && e.kind != baseFilter.kind)
            continue;
        if (st.categoryTab == 1 && e.category != assets::AssetCategory::BuiltIn) continue;
        if (st.categoryTab == 2 && e.category != assets::AssetCategory::User) continue;
        if (st.categoryTab == 3 && e.category != assets::AssetCategory::Project) continue;
        if (!q.empty()) {
            std::string n = e.displayName;
            for (char& c : n) c = (char)std::tolower((unsigned char)c);
            if (n.find(q) == std::string::npos) continue;
        }
        view.push_back(&e);
    }

    ImGui::BeginChild("##assetgrid", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.2f), true);
    const float cell = st.cellSize;
    const float pad = T.s2;
    float avail = ImGui::GetContentRegionAvail().x;
    int cols = std::max(1, (int)((avail + pad) / (cell + pad)));
    int i = 0;
    for (const assets::AssetInfo* e : view) {
        ImGui::PushID(e->key.c_str());
        if (i % cols != 0) ImGui::SameLine(0, pad);

        ImVec2 p0 = ImGui::GetCursorScreenPos();
        bool selected = (st.selectedKey == e->key);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImU32 bg = ImGui::GetColorU32(selected ? T.accent : T.bgElevated);
        dl->AddRectFilled(p0, ImVec2(p0.x + cell, p0.y + cell + 18.f), bg, T.rSm);

        // Thumb
        ID3D11ShaderResourceView* srv = nullptr;
        if (device)
            srv = assets::AssetManager::Get().GetThumbSrv(device, e->key, false);
        // Kick async full load only if needed for dims later — thumbs self-load
        ImVec2 imgMin(p0.x + 4, p0.y + 4);
        ImVec2 imgMax(p0.x + cell - 4, p0.y + cell - 4);
        if (srv) {
            dl->AddImage((ImTextureID)srv, imgMin, imgMax);
        } else {
            dl->AddRectFilled(imgMin, imgMax, ImGui::GetColorU32(T.bgWindow), T.rSm);
            const char* mark = "?";
            if (e->loadState == assets::AssetLoadState::Pending) mark = "…";
            else if (e->loadState == assets::AssetLoadState::Failed) mark = "!";
            ImVec2 ts = ImGui::CalcTextSize(mark);
            dl->AddText(ImVec2((imgMin.x + imgMax.x - ts.x) * 0.5f,
                               (imgMin.y + imgMax.y - ts.y) * 0.5f),
                        ImGui::GetColorU32(T.textSecondary), mark);
        }

        // Label
        char label[64];
        std::snprintf(label, sizeof(label), "%.12s", e->displayName.c_str());
        dl->AddText(ImVec2(p0.x + 4, p0.y + cell + 2), ImGui::GetColorU32(T.textPrimary), label);

        ImGui::InvisibleButton("##cell", ImVec2(cell, cell + 18.f));
        bool hovered = ImGui::IsItemHovered();
        if (hovered) {
            if (st.hoverKey != e->key) {
                st.hoverKey = e->key;
                st.hoverTimer = 0.f;
            } else {
                st.hoverTimer += ImGui::GetIO().DeltaTime;
            }
            if (st.hoverTimer >= T.tooltipDelaySec * 0.5f) {
                // HQ preview popup
                ID3D11ShaderResourceView* hi =
                    device ? assets::AssetManager::Get().GetThumbSrv(device, e->key, true) : nullptr;
                ImGui::BeginTooltip();
                if (hi) ImGui::Image((ImTextureID)hi, ImVec2(128, 128));
                ImGui::TextUnformatted(e->displayName.c_str());
                ImGui::TextDisabled("%s · %s", assets::CategoryDisplayName(e->category),
                                    assets::KindName(e->kind));
                if (e->w > 0 && e->h > 0)
                    ImGui::TextDisabled("%d × %d", e->w, e->h);
                ImGui::EndTooltip();
            }
        }

        if (ImGui::IsItemClicked()) {
            st.selected = i;
            st.selectedKey = e->key;
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            st.selectedKey = e->key;
            if (onActivate) onActivate(e->key);
            activated = true;
        }
        ImGui::PopID();
        ++i;
    }
    if (view.empty()) {
        ImGui::TextDisabled(assets::AssetManager::Get().IsIndexReady()
            ? "No assets in this category"
            : "Indexing libraries…");
    }
    ImGui::EndChild();

    // Footer selection info
    if (!st.selectedKey.empty()) {
        ImGui::TextDisabled("%s", assets::AssetManager::Get().DisplayName(st.selectedKey).c_str());
    } else {
        ImGui::TextDisabled("%d item(s)", (int)view.size());
    }

    ImGui::PopID();
    return activated;
}

} // namespace Ui
