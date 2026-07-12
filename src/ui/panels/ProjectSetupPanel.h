#pragma once
#include <d3d11.h>

class Canvas;

namespace UI {
struct UIState;

// Non-modal Project Setup (maps / labels / export).
void DrawProjectSetupPanel(UIState& state, Canvas& canvas, ID3D11Device* device = nullptr);

} // namespace UI
