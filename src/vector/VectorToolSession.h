#pragma once
// Interactive vector tool state (select / edit / pen / shapes).
// Call from main viewport loop; draws canvas HUD + handles.

#include "../ui/EditorPanels.h"
#include <d3d11.h>
#include <string>
#include <cstdint>
#include <vector>

struct ID3D11Device;
class Canvas;

namespace vec {

// One-frame interaction for ActiveTool::Vector*
// Returns true if mouse/key was consumed (skip brush/selection).
bool UpdateVectorTools(
    Canvas& canvas,
    ID3D11Device* device,
    ActiveTool& activeTool,
    bool isHovered,
    bool canvasInputBlocked,
    float canvasX, float canvasY,
    float viewportW, float viewportH,
    float imageMinX, float imageMinY);

// Human-readable status for Tool Settings / footer.
const char* VectorToolTitle(ActiveTool t);
const char* VectorToolHowTo(ActiveTool t);
// Extra live status (pen points, selection, layer name). Empty if N/A.
std::string VectorToolLiveStatus(const Canvas& canvas, ActiveTool t);

// Selection (primary = last selected). 0 = none.
uint32_t VectorSelectedShapeId();
void VectorSetSelectedShapeId(uint32_t id);
std::vector<uint32_t> VectorSelectedShapeIds();
int VectorSelectionCount();

// UI actions (Tool Settings / menu). Operate on active vector layer + selection.
bool VectorActionConvertToPath(Canvas& canvas);
bool VectorActionBringFront(Canvas& canvas);
bool VectorActionSendBack(Canvas& canvas);
bool VectorActionRaise(Canvas& canvas);
bool VectorActionLower(Canvas& canvas);
bool VectorActionDuplicate(Canvas& canvas);
bool VectorActionExportSvg(Canvas& canvas); // opens save dialog
bool VectorActionBreakAtNode(Canvas& canvas);
bool VectorActionApplyStyle(Canvas& canvas); // apply g_VectorToolStyle to selection
bool VectorActionJoinPaths(Canvas& canvas);  // join two open paths in selection
// mode: 0=L 1=HC 2=R 3=T 4=VC 5=B (needs 2+ selected)
bool VectorActionAlign(Canvas& canvas, int mode);

// Numeric transform of primary selection (document px).
bool VectorGetSelectionBounds(Canvas& canvas, float& x, float& y, float& w, float& h);
bool VectorSetSelectionBounds(Canvas& canvas, float x, float y, float w, float h,
                              bool scaleStyles);
bool VectorGetSelectionRound(Canvas& canvas, float& rx, float& ry); // rect only
bool VectorSetSelectionRound(Canvas& canvas, float rx, float ry);

// Global: whether resize scales stroke width
bool& VectorScaleStylesFlag();

} // namespace vec
