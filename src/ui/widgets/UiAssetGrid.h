#pragma once
#include "../../assets/AssetTypes.h"
#include <d3d11.h>
#include <functional>
#include <string>
#include <vector>

namespace Ui {

struct AssetGridState {
    int selected = -1;
    std::string selectedKey;
    char search[128] = {};
    bool searchOpen = false; // icon → expand full-width field
    int categoryTab = 0; // 0=All 1=Core 2=User 3=Project
    float cellSize = 88.f;
    std::string hoverKey;
    float hoverTimer = 0.f;
};

// Draws a filtered thumb grid. Returns true if selection changed (double-click or Enter).
// onActivate: called when user activates an asset (double-click).
bool AssetGrid(const char* id, AssetGridState& st, ID3D11Device* device,
               const assets::AssetFilter& baseFilter,
               const std::vector<assets::AssetInfo>& items,
               const std::function<void(const std::string& key)>& onActivate = {});

} // namespace Ui
