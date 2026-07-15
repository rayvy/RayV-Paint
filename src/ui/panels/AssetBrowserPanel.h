#pragma once
#include <d3d11.h>

class Canvas;

namespace UI {
struct UIState;

void DrawAssetBrowserPanel(UIState& state, Canvas& canvas, ID3D11Device* device);

} // namespace UI
