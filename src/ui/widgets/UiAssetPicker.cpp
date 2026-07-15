#include "UiAssetPicker.h"
#include "UiAssetGrid.h"
#include "../style/UiTokens.h"
#include "../dialogs/Win32FileDialogs.h"
#include "../../assets/AssetManager.h"
#include <imgui.h>
#include <cstdio>

namespace Ui {
namespace {

bool g_Open = false;
bool g_RequestOpen = false;
assets::AssetFilter g_Filter{};
char g_Title[64] = "Choose Asset";
AssetGridState g_Grid;
std::string g_ResultKey;
bool g_HasResult = false;

} // namespace

void OpenAssetPicker(const assets::AssetFilter& filter, const char* title) {
    g_Filter = filter;
    g_Filter.kind = filter.kind; // enforce
    if (title && title[0]) {
        std::snprintf(g_Title, sizeof(g_Title), "%s", title);
    }
    g_Grid = AssetGridState{};
    g_RequestOpen = true;
    g_Open = true;
    g_HasResult = false;
    g_ResultKey.clear();
}

bool AssetPickerResult(std::string& outKey) {
    if (!g_HasResult) return false;
    outKey = g_ResultKey;
    g_HasResult = false;
    g_ResultKey.clear();
    return true;
}

bool IsAssetPickerOpen() { return g_Open; }

void DrawAssetPicker(ID3D11Device* device) {
    if (!g_Open && !g_RequestOpen) return;

    if (g_RequestOpen) {
        ImGui::OpenPopup(g_Title);
        g_RequestOpen = false;
    }

    ImGui::SetNextWindowSize(ImVec2(520, 420), ImGuiCond_FirstUseEver);
    bool open = g_Open;
    if (!ImGui::BeginPopupModal(g_Title, &open, ImGuiWindowFlags_None)) {
        g_Open = open;
        return;
    }
    g_Open = open;

    auto items = assets::AssetManager::Get().List(g_Filter);
    auto activate = [&](const std::string& key) {
        g_ResultKey = key;
        g_HasResult = true;
        g_Open = false;
        ImGui::CloseCurrentPopup();
    };

    AssetGrid("##pickergrid", g_Grid, device, g_Filter, items, activate);

    if (ImGui::Button("Import File…")) {
        char path[512] = {};
        if (Ui::ShowOpenFile(
                path, sizeof(path),
                "Images (*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds)\0*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds\0All\0*.*\0")) {
            std::string key = assets::AssetManager::Get().ImportFileToProject(path);
            if (!key.empty()) {
                g_Grid.selectedKey = key;
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        assets::AssetManager::Get().RefreshAllLibrariesAsync();
    }
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x - 160, 0));
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        g_Open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    bool canOk = !g_Grid.selectedKey.empty();
    if (!canOk) ImGui::BeginDisabled();
    if (ImGui::Button("OK") && canOk) {
        activate(g_Grid.selectedKey);
    }
    if (!canOk) ImGui::EndDisabled();

    ImGui::EndPopup();
    if (!g_Open && ImGui::IsPopupOpen(g_Title) == false) {
        // closed via X
    }
}

} // namespace Ui
