#pragma once

class Canvas;

namespace UI {
struct UIState;

// Layer Effects modal (styles + filters for active layer).
void DrawLayerEffectsPanel(UIState& state, Canvas& canvas);

} // namespace UI
