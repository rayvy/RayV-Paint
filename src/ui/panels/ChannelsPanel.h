#pragma once
#include <d3d11.h>

class Canvas;

namespace UI {
struct UIState;

// Channels dock: map switchers + RGBA solo previews.
void DrawChannelsPanel(UIState& state, Canvas& canvas, ID3D11Device* device);

} // namespace UI
