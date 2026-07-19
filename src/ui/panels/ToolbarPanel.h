#pragma once

class Canvas;
struct BrushSettings;
enum class ActiveTool;

namespace UI {
struct UIState;

// Left/top tool strip (grouped hotkeys, color swap chip, rebind popup).
void DrawToolbarPanel(UIState& state, Canvas& canvas, BrushSettings& brush, ActiveTool& activeTool);

} // namespace UI
