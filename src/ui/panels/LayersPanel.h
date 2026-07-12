#pragma once
#include <d3d11.h>

class Canvas;

namespace UI {
struct UIState;

// Layers dock panel (list, fill chips, mask, footer actions).
void DrawLayersPanel(UIState& state, Canvas& canvas, ID3D11Device* device);

} // namespace UI
