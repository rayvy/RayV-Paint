#include "AssetBrowserPanel.h"
#include "../EditorPanels.h"
#include "../widgets/UiPanel.h"
#include "../widgets/UiAssetGrid.h"
#include "../widgets/UiAssetPicker.h"
#include "../style/UiTokens.h"
#include "../dialogs/Win32FileDialogs.h"
#include "../../assets/AssetManager.h"
#include "../../Canvas.h"
#include <imgui.h>

namespace UI {

void DrawAssetBrowserPanel(UIState& state, Canvas& canvas, ID3D11Device* device) {
    if (!state.showAssetBrowser) return;

    if (!Ui::BeginDockPanel("Asset Browser", &state.showAssetBrowser)) {
        Ui::EndDockPanel();
        return;
    }

    static Ui::AssetGridState grid;
    assets::AssetFilter filter;
    filter.kind = assets::AssetKind::Texture;
    filter.includeCore = true;
    filter.includeUser = true;
    filter.includeProject = true;

    auto items = assets::AssetManager::Get().List(filter);

    auto onActivate = [&](const std::string& key) {
        // Double-click: if active fill layer, bind texture
        int ai = canvas.GetActiveLayerIndex();
        const auto& layers = canvas.GetLayers();
        if (ai >= 0 && ai < (int)layers.size() && layers[ai].IsFill()) {
            canvas.BindFillTextureAsset(ai, key);
        }
    };

    Ui::AssetGrid("##browser", grid, device, filter, items, onActivate);

    if (ImGui::Button("Import to User…")) {
        char path[512] = {};
        if (Ui::ShowOpenFile(
                path, sizeof(path),
                "Images (*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds)\0*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds\0All\0*.*\0")) {
            assets::AssetManager::Get().ImportFileToUser(path);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Import to Project…")) {
        char path[512] = {};
        if (Ui::ShowOpenFile(
                path, sizeof(path),
                "Images (*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds)\0*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds\0All\0*.*\0")) {
            std::string key = assets::AssetManager::Get().ImportFileToProject(path);
            if (!key.empty()) grid.selectedKey = key;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        assets::AssetManager::Get().RefreshAllLibrariesAsync();
    }

    int ai = canvas.GetActiveLayerIndex();
    const auto& layers = canvas.GetLayers();
    bool fillActive = ai >= 0 && ai < (int)layers.size() && layers[ai].IsFill();
    if (fillActive && !grid.selectedKey.empty()) {
        if (ImGui::Button("Use on Fill Layer")) {
            canvas.BindFillTextureAsset(ai, grid.selectedKey);
        }
    } else {
        ImGui::TextDisabled("Select a Fill layer + asset to bind");
    }

    Ui::EndDockPanel();
    (void)state;
}

} // namespace UI
