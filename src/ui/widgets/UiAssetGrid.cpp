#include "UiAssetGrid.h"
#include "../style/UiTokens.h"
#include "../style/UiMotion.h"
#include "../icons/SvgIconCache.h"
#include "../widgets/UiIconButton.h"
#include "../widgets/UiTooltip.h"
#include "../../assets/AssetManager.h"
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cmath>
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

    // ---- Header: search icon expands; categories fade when searching ----
    static Ui::AnimFloat s_searchExpand;
    const bool wantExpand = st.searchOpen || st.search[0] != '\0';
    s_searchExpand.SetTarget(wantExpand ? 1.f : 0.f, T.durMed, Ui::EaseKind::EaseOutCubic);
    s_searchExpand.Update(Ui::DeltaTime());
    const float expT = s_searchExpand.value;

    const float rowH = 26.f;
    const float iconBtn = 26.f;
    ImVec2 row0 = ImGui::GetCursorScreenPos();
    float availW = ImGui::GetContentRegionAvail().x;

    // Search icon button (always)
    {
        auto r = Ui::IconButton("##search_ico", "search", ImVec2(iconBtn, iconBtn),
            st.searchOpen ? "Close search" : "Search assets", true, st.searchOpen);
        if (r.clicked) {
            st.searchOpen = !st.searchOpen;
            if (!st.searchOpen && st.search[0] == '\0') {
                // collapsed empty
            } else if (st.searchOpen) {
                ImGui::SetKeyboardFocusHere();
            }
        }
    }

    // Expanding search field over the rest of the row
    const float searchW = std::max(0.f, (availW - iconBtn - 6.f) * expT);
    if (searchW > 8.f) {
        ImGui::SameLine(0, 4);
        ImGui::SetNextItemWidth(searchW);
        ImGui::InputTextWithHint("##search", "Search…", st.search, sizeof(st.search));
        if (st.searchOpen && ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsItemActive()) {
            st.searchOpen = false;
            st.search[0] = 0;
        }
    }

    // Category tabs — fade + shrink when search expands
    const float catAlpha = 1.f - expT;
    if (catAlpha > 0.02f) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, catAlpha);
        // Next line only when search not fully expanded (avoid double height flash)
        if (expT < 0.85f) {
            // same row if space: draw after icon when collapsed
            if (expT < 0.15f) {
                ImGui::SameLine(0, 8);
            } else {
                ImGui::SetCursorScreenPos(ImVec2(row0.x + iconBtn + 4.f + searchW + 6.f, row0.y));
            }
            const char* tabs[] = { "All", "Core", "User", "Project" };
            for (int i = 0; i < 4; ++i) {
                if (i) ImGui::SameLine(0, 4);
                bool sel = (st.categoryTab == i);
                if (sel) ImGui::PushStyleColor(ImGuiCol_Button, T.accent);
                if (ImGui::SmallButton(tabs[i])) st.categoryTab = i;
                if (sel) ImGui::PopStyleColor();
            }
        }
        ImGui::PopStyleVar();
    }
    // Reserve header row height
    ImGui::SetCursorScreenPos(ImVec2(row0.x, row0.y + rowH + 4.f));
    ImGui::Dummy(ImVec2(1, 0));

    // Filter items
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

    // Content-only scroll (footer buttons live outside AssetGrid in panel)
    ImGui::BeginChild("##assetgrid", ImVec2(0, 0), false,
        ImGuiWindowFlags_None);
    const float cell = st.cellSize;
    const float pad = T.s2;
    float avail = ImGui::GetContentRegionAvail().x;
    int cols = std::max(1, (int)((avail + pad) / (cell + pad)));
    int i = 0;
    const float prefetch = (cell + 18.f + pad) * 2.f;
    for (const assets::AssetInfo* e : view) {
        ImGui::PushID(e->key.c_str());
        if (i % cols != 0) ImGui::SameLine(0, pad);

        ImVec2 p0 = ImGui::GetCursorScreenPos();
        bool selected = (st.selectedKey == e->key);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImU32 bg = ImGui::GetColorU32(selected ? T.accent : T.bgElevated);
        dl->AddRectFilled(p0, ImVec2(p0.x + cell, p0.y + cell + 18.f), bg, T.rSm);

        const bool nearView = ImGui::IsRectVisible(
            ImVec2(p0.x, p0.y - prefetch),
            ImVec2(p0.x + cell, p0.y + cell + 18.f + prefetch));

        ID3D11ShaderResourceView* srv = nullptr;
        if (device && nearView)
            srv = assets::AssetManager::Get().GetThumbSrv(device, e->key, false);
        ImVec2 imgMin(p0.x + 4, p0.y + 4);
        ImVec2 imgMax(p0.x + cell - 4, p0.y + cell - 4);
        if (srv) {
            dl->AddImage((ImTextureID)srv, imgMin, imgMax);
        } else {
            dl->AddRectFilled(imgMin, imgMax, ImGui::GetColorU32(T.bgWindow), T.rSm);
            const bool pending = assets::AssetManager::Get().IsThumbPending(e->key);
            const bool failed = assets::AssetManager::Get().IsThumbFailed(e->key);
            const ImVec2 c((imgMin.x + imgMax.x) * 0.5f, (imgMin.y + imgMax.y) * 0.5f);
            if (pending) {
                const float rad = (imgMax.x - imgMin.x) * 0.14f;
                const float t = (float)ImGui::GetTime() * 7.5f;
                for (int s = 0; s < 8; ++s) {
                    const float a = t + (float)s * 6.2831853f / 8.f;
                    const float fade = 0.2f + 0.8f * ((float)s / 8.f);
                    dl->AddCircleFilled(
                        ImVec2(c.x + cosf(a) * rad, c.y + sinf(a) * rad),
                        rad * 0.28f,
                        IM_COL32(130, 175, 255, (int)(fade * 220.f)), 6);
                }
            } else {
                const char* mark = failed ? "!" : "…";
                ImVec2 ts = ImGui::CalcTextSize(mark);
                dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f),
                            ImGui::GetColorU32(T.textSecondary), mark);
            }
        }

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
        // Drag asset key (drop onto Fill layer / canvas bind)
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            ImGui::SetDragDropPayload("RAYV_ASSET_KEY", e->key.c_str(), e->key.size() + 1);
            ImGui::TextUnformatted(e->displayName.c_str());
            if (srv) ImGui::Image((ImTextureID)srv, ImVec2(48, 48));
            ImGui::EndDragDropSource();
        }
        ImGui::PopID();
        ++i;
    }
    ImGui::EndChild();

    ImGui::PopID();
    return activated;
}

} // namespace Ui
