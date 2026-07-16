#pragma once
// Pointer bag filled once at startup. Operators are one-liners into these targets.
// Do not put business logic in this header.

#include "../../ui/EditorPanels.h"
#include "../../core/PaintEngine.h"
#include <d3d11.h>

class Canvas;

namespace core::ops {

struct OperatorHost {
    Canvas* canvas = nullptr;
    UI::UIState* ui = nullptr;
    ID3D11Device* device = nullptr;
    BrushSettings* brush = nullptr;
    float* secondaryColor = nullptr; // rgba[4]
    ActiveTool* activeTool = nullptr;
    ActiveTool* lastSelectTool = nullptr;
    ActiveTool* lastLassoTool = nullptr;
    ActiveTool* lastWandTool = nullptr;
    bool* freeTransformMode = nullptr;
    ActiveTool* toolBeforeFreeTransform = nullptr;
    ActiveTool* toolBeforeWarp = nullptr;
    int* warpDragIndex = nullptr;
    bool* colorSwapPending = nullptr; // UI cross-fade when SwapColors runs
};

// Register all catalog-backed execute handlers. Call once after host fields are valid.
void RegisterEditorOperators(OperatorHost host);

// Call every frame before DispatchKeymapFrame — project tabs change the active Canvas*.
void BindOperatorHostFrame(Canvas* canvas, ID3D11Device* device);

} // namespace core::ops
