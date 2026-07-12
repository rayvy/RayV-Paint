#pragma once

class Canvas;
struct BrushSettings;

namespace UI {
struct UIState;

// Colors dock: SV square, hue/alpha, primary/secondary, HEX/RGB.
void DrawColorsPanel(UIState& state, Canvas& canvas, BrushSettings& brush);

} // namespace UI
