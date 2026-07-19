#pragma once

class Canvas;

namespace UI {
struct UIState;

// Zoom / pan readout + viewport flip / rotation.
void DrawViewportNavPanel(UIState& state, Canvas& canvas);

} // namespace UI
