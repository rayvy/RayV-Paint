#pragma once

class Canvas;

namespace UI {
struct UIState;

// Project type / bit depth / paths / export container (no Mod Setup chrome).
void DrawPropertiesPanel(UIState& state, Canvas& canvas);

} // namespace UI
