#pragma once
#include <d3d11.h>

class Canvas;

namespace UI {
struct UIState;

// Layer Effects modal (styles + filters for active layer).
void DrawLayerEffectsPanel(UIState& state, Canvas& canvas, ID3D11Device* device = nullptr);

} // namespace UI
