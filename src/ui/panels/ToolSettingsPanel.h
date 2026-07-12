#pragma once
#include <d3d11.h>

class Canvas;
struct BrushSettings;
enum class ActiveTool;

namespace UI {
struct UIState;

// Adaptive tool settings strip (brush, selection, fill, transform, …).
void DrawToolSettingsPanel(UIState& state, Canvas& canvas, BrushSettings& brush,
                           ActiveTool activeTool, ID3D11Device* device);

} // namespace UI
