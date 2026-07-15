#pragma once
#include "Dx11ImGuiApp.h"
#include "../atlas/AtlasPacker.h"
#include <string>
#include <vector>
#include <d3d11.h>

namespace helpers {

struct AtlasUiState {
    std::vector<std::string> files;
    int padding = 2;
    int maxSizeIndex = 2; // 0=1024 1=2048 2=4096 3=8192
    bool powerOfTwo = true;
    bool alsoExportDds = false; // combined mode option (atlas only)
    int formatIndex = 0;
    bool generateMips = true;
    int speedIndex = 1;

    bool busy = false;
    std::string status;
    AtlasResult lastPack;
    ID3D11ShaderResourceView* previewSrv = nullptr;
    int previewW = 0;
    int previewH = 0;
};

void DrawAtlasUi(AtlasUiState& st, Dx11ImGuiApp& app);
void ReleaseAtlasPreview(AtlasUiState& st);

} // namespace helpers
