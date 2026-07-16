#include "AssetBrowserPanel.h"
#include "../EditorPanels.h"
#include "../widgets/UiPanel.h"
#include "../widgets/UiAssetGrid.h"
#include "../widgets/UiAssetPicker.h"
#include "../widgets/UiTooltip.h"
#include "../style/UiTokens.h"
#include "../dialogs/Win32FileDialogs.h"
#include "../../assets/AssetManager.h"
#include "../../Canvas.h"
#include <imgui.h>

namespace UI {

void DrawAssetBrowserPanel(UIState& state, Canvas& canvas, ID3D11Device* device) {
    if (!state.showAssetBrowser) return;

    // Outer panel fixed (no own scrollbar); grid child scrolls.
    Ui::BeginDockPanel("Asset Browser", &state.showAssetBrowser);
    Ui::ClampDockLeafBox(160.f, 480.f, 160.f, 2400.f);

    static Ui::AssetGridState grid;
    assets::AssetFilter filter;
    filter.kind = assets::AssetKind::Texture;
    filter.includeCore = true;
    filter.includeUser = true;
    filter.includeProject = true;

    auto items = assets::AssetManager::Get().List(filter);

    auto onActivate = [&](const std::string& key) {
        int ai = canvas.GetActiveLayerIndex();
        const auto& layers = canvas.GetLayers();
        if (ai >= 0 && ai < (int)layers.size() && layers[ai].IsFill()) {
            canvas.BindFillTextureAsset(ai, key);
        }
    };

    // Footer reserve: import row only
    const float footerH = ImGui::GetFrameHeightWithSpacing() + 4.f;
    ImGui::BeginChild("##ab_body", ImVec2(0, -footerH), false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    Ui::AssetGrid("##browser", grid, device, filter, items, onActivate);
    ImGui::EndChild();

    // Fixed footer actions
    if (ImGui::SmallButton("Import User")) {
        char path[512] = {};
        if (Ui::ShowOpenFile(
                path, sizeof(path),
                "Images (*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds)\0*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds\0All\0*.*\0")) {
            assets::AssetManager::Get().ImportFileToUserAsync(path);
        }
    }
    if (ImGui::IsItemHovered()) Ui::Tooltip("Import texture to User library");
    ImGui::SameLine();
    if (ImGui::SmallButton("Import Proj")) {
        char path[512] = {};
        if (Ui::ShowOpenFile(
                path, sizeof(path),
                "Images (*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds)\0*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds\0All\0*.*\0")) {
            assets::AssetManager::Get().ImportFileToProjectAsync(path);
        }
    }
    if (ImGui::IsItemHovered()) Ui::Tooltip("Import into this project session");
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) {
        assets::AssetManager::Get().RefreshAllLibrariesAsync();
    }
    if (ImGui::IsItemHovered()) Ui::Tooltip("Rescan libraries");

    // Bind fill if applicable (icon-only, no long status text)
    int ai = canvas.GetActiveLayerIndex();
    const auto& layers = canvas.GetLayers();
    bool fillActive = ai >= 0 && ai < (int)layers.size() && layers[ai].IsFill();
    if (fillActive && !grid.selectedKey.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Use Fill")) {
            canvas.BindFillTextureAsset(ai, grid.selectedKey);
        }
        if (ImGui::IsItemHovered()) Ui::Tooltip("Bind selected asset to active Fill layer");
    }

    Ui::EndDockPanel();
    (void)state;
}

} // namespace UI
