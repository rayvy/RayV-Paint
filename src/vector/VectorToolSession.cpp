#include "VectorToolSession.h"
#include "PathMath.h"
#include "PathTypes.h"
#include "VectorRasterizer.h"
#include "SvgIo.h"
#include "../Canvas.h"
#include "../core/Notifications.h"
#include "../ui/dialogs/Win32FileDialogs.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace vec {
namespace {

// Multi-select: ordered, last = primary
std::vector<uint32_t> g_SelIds;
uint32_t g_SelId = 0; // primary alias (kept in sync)
bool g_Dragging = false;
float g_DragLastX = 0.f, g_DragLastY = 0.f;
std::string g_UndoBefore;
bool g_Creating = false;
float g_CreateX0 = 0.f, g_CreateY0 = 0.f;
uint32_t g_CreateId = 0;
// Pen draft lives OUTSIDE the document until commit (polygonal-lasso style).
// Never mutates tile cache mid-draw — kills residual ghosts and stuck state.
bool g_PenActive = false;
std::vector<PathPoint> g_PenPts; // draft only
std::string g_PenBefore;
bool g_PenDragHandle = false; // mouse held after placing a point → curve handles
static ActiveTool g_PrevVectorTool = ActiveTool::Brush;
int g_EditSub = -1, g_EditPt = -1;
bool g_EditDrag = false;
// Handle drag (incoming/outgoing) when edit mode
int g_HandleKind = 0; // 0=node, 1=hin, 2=hout
float g_CursorX = 0.f, g_CursorY = 0.f;
bool g_NotifiedAutoLayer = false;
// Freehand stroke
bool g_FreehandActive = false;
std::vector<V2> g_FreehandPts;
std::string g_FreehandBefore;
// Clipboard (shape JSON)
std::string g_ClipboardShape;
// Select resize handle: -1 none, 0..3 = TL TR BR BL
int g_ResizeHandle = -1;
float g_ResizeStartX0 = 0, g_ResizeStartY0 = 0, g_ResizeStartX1 = 0, g_ResizeStartY1 = 0;
// Polygon click-build
bool g_PolyActive = false;
std::vector<V2> g_PolyPts;
std::string g_PolyBefore;

// Helper: mark dirty + re-raster full after structural edit
void CommitDoc(Canvas& canvas, int layerIdx, Document& doc, const std::string& before,
               const char* name) {
    doc.MarkAllDirty(canvas.GetWidth(), canvas.GetHeight());
    canvas.CommitVectorEdit(layerIdx, before, DocumentToJson(doc), name);
}

int ActiveVectorLayer(Canvas& canvas) {
    int i = canvas.GetActiveLayerIndex();
    if (i >= 0 && i < (int)canvas.GetLayers().size() && canvas.GetLayers()[i].IsVector())
        return i;
    return -1;
}

ShapeStyle StyleFromUi() {
    ShapeStyle st;
    st.fillEnabled = UI::g_VectorToolStyle.fillEnabled;
    st.strokeEnabled = UI::g_VectorToolStyle.strokeEnabled;
    st.strokeWidth = UI::g_VectorToolStyle.strokeWidth;
    for (int i = 0; i < 4; ++i) {
        st.fillRgba[i] = UI::g_VectorToolStyle.fillRgba[i];
        st.strokeRgba[i] = UI::g_VectorToolStyle.strokeRgba[i];
        st.gradRgba0[i] = UI::g_VectorToolStyle.fillRgba[i];
        st.gradRgba1[i] = UI::g_VectorToolStyle.gradRgba1[i];
    }
    st.fillPaint = UI::g_VectorToolStyle.fillLinearGrad ? FillPaint::LinearGrad : FillPaint::Solid;
    st.gradUseShapeBounds = true;
    st.dashLen = UI::g_VectorToolStyle.dashLen;
    st.gapLen = UI::g_VectorToolStyle.gapLen;
    return st;
}

void SyncPrimarySel() {
    g_SelId = g_SelIds.empty() ? 0 : g_SelIds.back();
}
void SetSoleSelection(uint32_t id) {
    g_SelIds.clear();
    if (id) g_SelIds.push_back(id);
    SyncPrimarySel();
}
void ToggleSelection(uint32_t id) {
    if (!id) return;
    auto it = std::find(g_SelIds.begin(), g_SelIds.end(), id);
    if (it != g_SelIds.end())
        g_SelIds.erase(it);
    else
        g_SelIds.push_back(id);
    SyncPrimarySel();
}
bool IsSelected(uint32_t id) {
    return std::find(g_SelIds.begin(), g_SelIds.end(), id) != g_SelIds.end();
}

// Hit-test resize grips in document space (returns 0..3 or -1)
int HitResizeHandle(const Shape& s, float x, float y, float tol) {
    float x0, y0, x1, y1;
    if (!ShapeLocalBounds(s, x0, y0, x1, y1)) return -1;
    V2 corners[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    for (int i = 0; i < 4; ++i) {
        if (std::hypot(x - corners[i].x, y - corners[i].y) <= tol)
            return i;
    }
    return -1;
}

void MarkDirty(Document& doc, const Shape& s, int w, int h) {
    float x0, y0, x1, y1;
    if (ShapeBounds(s, x0, y0, x1, y1, true))
        doc.MarkDirty((int)std::floor(x0) - 2, (int)std::floor(y0) - 2,
                      (int)std::ceil(x1) + 2, (int)std::ceil(y1) + 2);
    else
        doc.MarkAllDirty(w, h);
}

void ClearPenDraft() {
    g_PenActive = false;
    g_PenPts.clear();
    g_PenBefore.clear();
    g_PenDragHandle = false;
}

// Screen-space close radius (like polygonal lasso ~few px), converted to doc space via zoom.
float CloseTolDoc(float zoom) {
    const float screenPx = 10.f; // easy to hit first point
    return std::max(3.f, screenPx / std::max(0.001f, zoom));
}

bool NearDoc(float x0, float y0, float x1, float y1, float tol) {
    float dx = x0 - x1, dy = y0 - y1;
    return dx * dx + dy * dy <= tol * tol;
}

// Commit draft pen → real shape. closed=true when finishing via first-point / Enter.
bool CommitPenDraft(Canvas& canvas, int layerIdx, Document& doc, bool closed) {
    if (g_PenPts.size() < 2) {
        ClearPenDraft();
        return false;
    }
    std::string before = g_PenBefore;
    if (before.empty())
        before = DocumentToJson(doc);

    Shape sh = MakeEmptyPath(StyleFromUi(), doc.AllocId());
    sh.name = "Path";
    SubPath sp;
    sp.closed = closed;
    sp.points = g_PenPts;
    sh.path.subs.push_back(std::move(sp));
    doc.shapes.push_back(std::move(sh));
    uint32_t nid = doc.shapes.back().id;
    doc.MarkAllDirty(canvas.GetWidth(), canvas.GetHeight());
    canvas.CommitVectorEdit(layerIdx, before, DocumentToJson(doc),
                            closed ? "Pen path (closed)" : "Pen path");
    canvas.EnsureVectorRaster(layerIdx, false, /*forceFull=*/true);
    SetSoleSelection(nid);
    ClearPenDraft();
    return true;
}

void CancelPenDraft(Canvas& canvas, int layerIdx) {
    // Draft never touches document — just clear UI state.
    // Still force full raster if previous broken sessions left ghosts.
    ClearPenDraft();
    if (layerIdx >= 0)
        canvas.EnsureVectorRaster(layerIdx, false, /*forceFull=*/true);
}

ImVec2 DocToScreen(float dx, float dy, float zoom, float panX, float panY,
                   float viewportW, float viewportH, float imageMinX, float imageMinY) {
    float ox = std::floor(panX + viewportW * 0.5f);
    float oy = std::floor(panY + viewportH * 0.5f);
    return ImVec2(imageMinX + dx * zoom + ox, imageMinY + dy * zoom + oy);
}

void EnsureLayerNotify(Canvas& canvas, int layerIdx) {
    if (g_NotifiedAutoLayer) return;
    if (layerIdx < 0 || layerIdx >= (int)canvas.GetLayers().size()) return;
    // Only when we just created via Ensure — hard to know; show once per session if vector tool used
    g_NotifiedAutoLayer = true;
    core::Notifications::Get().Push(
        "Vector tools use a Vector layer (Layers → Vec, or auto-created).",
        core::NotifyLevel::Info);
}

void DrawHud(ActiveTool tool, Canvas& canvas, int layerIdx, Document* doc,
             float zoom, float panX, float panY, float vw, float vh, float imx, float imy) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    // Top-left of canvas image
    ImVec2 p0(imx + 10.f, imy + 10.f);
    char line1[160];
    const char* title = VectorToolTitle(tool);
    const char* layerName = "?";
    if (layerIdx >= 0 && layerIdx < (int)canvas.GetLayers().size())
        layerName = canvas.GetLayers()[layerIdx].name.c_str();

    std::snprintf(line1, sizeof(line1), "VECTOR  ·  %s  ·  layer: %s", title, layerName);

    char line2[192];
    if (tool == ActiveTool::VectorPen && g_PenActive) {
        int n = (int)g_PenPts.size();
        std::snprintf(line2, sizeof(line2),
            "Pen: %d pts  ·  Click = add  ·  Click first pt = close  ·  Drag = curve  ·  Esc = cancel", n);
    } else if (tool == ActiveTool::VectorSelect) {
        if (g_SelIds.size() > 1)
            std::snprintf(line2, sizeof(line2),
                "%d selected  ·  Drag = move all  ·  Shift+click toggle  ·  Align in Tool Settings",
                (int)g_SelIds.size());
        else if (g_SelId)
            std::snprintf(line2, sizeof(line2),
                "Selected  ·  Drag body = move  ·  Corners = resize  ·  Shift+click multi  ·  Dbl-click = Edit");
        else
            std::snprintf(line2, sizeof(line2),
                "Click shape  ·  Shift+click multi-select  ·  Draw with Rect/Pen/Polygon…");
    } else if (tool == ActiveTool::VectorEdit) {
        std::snprintf(line2, sizeof(line2),
            "Nodes + handles  ·  Dbl-click segment = insert  ·  N = type  ·  Del  ·  Esc = Select");
    } else if (tool == ActiveTool::VectorRect || tool == ActiveTool::VectorEllipse) {
        std::snprintf(line2, sizeof(line2),
            "Click-drag to create  ·  Fill/Stroke / corner radius in Tool Settings");
    } else if (tool == ActiveTool::VectorLine) {
        std::snprintf(line2, sizeof(line2), "Click-drag line  ·  Stroke width in Tool Settings");
    } else if (tool == ActiveTool::VectorFreehand) {
        if (g_FreehandActive)
            std::snprintf(line2, sizeof(line2), "Drawing… %d samples  ·  release to finish",
                (int)g_FreehandPts.size());
        else
            std::snprintf(line2, sizeof(line2),
                "Click-drag freehand  ·  auto-smooth  ·  Closed toggle in Tool Settings");
    } else if (tool == ActiveTool::VectorPolygon) {
        if (g_PolyActive)
            std::snprintf(line2, sizeof(line2),
                "Polygon: %d pts  ·  Click = add  ·  Enter/dbl-click = finish  ·  Esc = cancel",
                (int)g_PolyPts.size());
        else
            std::snprintf(line2, sizeof(line2),
                "Click to place vertices  ·  Closed checkbox = polygon vs polyline");
    } else {
        std::snprintf(line2, sizeof(line2), "%s", VectorToolHowTo(tool));
    }

    ImVec2 ts1 = ImGui::CalcTextSize(line1);
    ImVec2 ts2 = ImGui::CalcTextSize(line2);
    float boxW = std::max(ts1.x, ts2.x) + 20.f;
    float boxH = ts1.y + ts2.y + 18.f;
    dl->AddRectFilled(p0, ImVec2(p0.x + boxW, p0.y + boxH), IM_COL32(18, 20, 28, 210), 6.f);
    dl->AddRect(p0, ImVec2(p0.x + boxW, p0.y + boxH), IM_COL32(80, 140, 255, 160), 6.f, 0, 1.f);
    dl->AddText(ImVec2(p0.x + 10.f, p0.y + 6.f), IM_COL32(140, 190, 255, 255), line1);
    dl->AddText(ImVec2(p0.x + 10.f, p0.y + 8.f + ts1.y), IM_COL32(220, 220, 230, 255), line2);

    // Selection / nodes overlay
    if (!doc) return;
    auto toSc = [&](float x, float y) {
        return DocToScreen(x, y, zoom, panX, panY, vw, vh, imx, imy);
    };

    // Freehand preview
    if (tool == ActiveTool::VectorFreehand && g_FreehandActive && g_FreehandPts.size() >= 2) {
        for (size_t i = 1; i < g_FreehandPts.size(); ++i) {
            ImVec2 a = DocToScreen(g_FreehandPts[i - 1].x, g_FreehandPts[i - 1].y,
                                   zoom, panX, panY, vw, vh, imx, imy);
            ImVec2 b = DocToScreen(g_FreehandPts[i].x, g_FreehandPts[i].y,
                                   zoom, panX, panY, vw, vh, imx, imy);
            dl->AddLine(a, b, IM_COL32(255, 180, 80, 220), 2.f);
        }
    }
    // Polygon preview
    if (tool == ActiveTool::VectorPolygon && g_PolyActive && !g_PolyPts.empty()) {
        for (size_t i = 1; i < g_PolyPts.size(); ++i) {
            ImVec2 a = DocToScreen(g_PolyPts[i - 1].x, g_PolyPts[i - 1].y,
                                   zoom, panX, panY, vw, vh, imx, imy);
            ImVec2 b = DocToScreen(g_PolyPts[i].x, g_PolyPts[i].y,
                                   zoom, panX, panY, vw, vh, imx, imy);
            dl->AddLine(a, b, IM_COL32(120, 220, 160, 230), 2.f);
        }
        ImVec2 last = DocToScreen(g_PolyPts.back().x, g_PolyPts.back().y,
                                   zoom, panX, panY, vw, vh, imx, imy);
        ImVec2 cur = DocToScreen(g_CursorX, g_CursorY, zoom, panX, panY, vw, vh, imx, imy);
        dl->AddLine(last, cur, IM_COL32(120, 220, 160, 140), 1.5f);
        if (UI::g_VectorToolStyle.polygonClosed && g_PolyPts.size() >= 2) {
            ImVec2 first = DocToScreen(g_PolyPts[0].x, g_PolyPts[0].y,
                                        zoom, panX, panY, vw, vh, imx, imy);
            dl->AddLine(cur, first, IM_COL32(120, 220, 160, 80), 1.f);
        }
        for (const auto& p : g_PolyPts) {
            ImVec2 sp = DocToScreen(p.x, p.y, zoom, panX, panY, vw, vh, imx, imy);
            dl->AddCircleFilled(sp, 4.f, IM_COL32(120, 220, 160, 255));
        }
    }

    // Pen draft rubber-band (overlay only — not in tile cache yet)
    if (tool == ActiveTool::VectorPen && g_PenActive && !g_PenPts.empty()) {
        for (size_t i = 1; i < g_PenPts.size(); ++i) {
            ImVec2 a = toSc(g_PenPts[i - 1].p.x, g_PenPts[i - 1].p.y);
            ImVec2 b = toSc(g_PenPts[i].p.x, g_PenPts[i].p.y);
            dl->AddLine(a, b, IM_COL32(100, 200, 255, 230), 2.f);
            // handles
            if (g_PenPts[i - 1].hout.Length() > 0.5f) {
                V2 o = g_PenPts[i - 1].OutAbs();
                dl->AddLine(a, toSc(o.x, o.y), IM_COL32(160, 200, 255, 160), 1.f);
            }
        }
        ImVec2 last = toSc(g_PenPts.back().p.x, g_PenPts.back().p.y);
        ImVec2 cur = toSc(g_CursorX, g_CursorY);
        dl->AddLine(last, cur, IM_COL32(100, 200, 255, 150), 1.5f);
        // close preview to first point
        if (g_PenPts.size() >= 3) {
            ImVec2 first = toSc(g_PenPts[0].p.x, g_PenPts[0].p.y);
            float closeTol = CloseTolDoc(zoom);
            // avoid name "near" — Windows headers #define it
            bool closeHover = NearDoc(g_CursorX, g_CursorY, g_PenPts[0].p.x, g_PenPts[0].p.y, closeTol);
            dl->AddLine(cur, first,
                closeHover ? IM_COL32(80, 255, 120, 220) : IM_COL32(100, 200, 255, 70),
                closeHover ? 2.f : 1.f);
            // ring around first point (close target)
            float r = std::max(6.f, closeTol * zoom);
            dl->AddCircle(first, r,
                closeHover ? IM_COL32(80, 255, 120, 255) : IM_COL32(255, 220, 80, 200), 0, 2.f);
        }
        for (size_t i = 0; i < g_PenPts.size(); ++i) {
            ImVec2 sp = toSc(g_PenPts[i].p.x, g_PenPts[i].p.y);
            dl->AddCircleFilled(sp, i == 0 ? 5.5f : 4.f,
                i == 0 ? IM_COL32(80, 255, 120, 255) : IM_COL32(255, 220, 80, 255));
            dl->AddCircle(sp, i == 0 ? 5.5f : 4.f, IM_COL32(20, 20, 30, 255), 0, 1.5f);
        }
    }

    // Multi-selection boxes
    for (uint32_t id : g_SelIds) {
        if (auto* sh = doc->Find(id)) {
            float x0, y0, x1, y1;
            if (ShapeBounds(*sh, x0, y0, x1, y1, false)) {
                ImVec2 a = toSc(x0, y0), b = toSc(x1, y1);
                bool primary = (id == g_SelId);
                dl->AddRect(a, b, primary ? IM_COL32(80, 170, 255, 230) : IM_COL32(80, 170, 255, 140),
                            0.f, 0, primary ? 2.f : 1.f);
                if (primary && tool == ActiveTool::VectorSelect) {
                    float g = 5.f;
                    auto grip = [&](ImVec2 c) {
                        dl->AddRectFilled(ImVec2(c.x - g, c.y - g), ImVec2(c.x + g, c.y + g),
                                          IM_COL32(80, 170, 255, 255));
                    };
                    grip(a); grip(b); grip(ImVec2(b.x, a.y)); grip(ImVec2(a.x, b.y));
                }
            }
        }
    }
    // Nodes for primary in edit/select
    if (g_SelId) {
        if (auto* sh = doc->Find(g_SelId)) {
            bool showNodes = (tool == ActiveTool::VectorEdit) ||
                             (tool == ActiveTool::VectorSelect && g_SelIds.size() == 1);
            if (showNodes) {
                Shape tmp = *sh;
                if (tmp.kind != ShapeKind::Path) RebuildPathFromParams(tmp);
                for (const auto& sp : tmp.path.subs) {
                    for (size_t i = 0; i < sp.points.size(); ++i) {
                        const auto& pt = sp.points[i];
                        V2 p = tmp.xform.Map(pt.p);
                        ImVec2 spx = toSc(p.x, p.y);
                        bool active = (tool == ActiveTool::VectorEdit && (int)i == g_EditPt &&
                                       g_SelId == sh->id);
                        float r = active ? 6.f : (tool == ActiveTool::VectorEdit ? 5.f : 3.5f);
                        dl->AddCircleFilled(spx, r, active ? IM_COL32(255, 120, 60, 255)
                                                           : IM_COL32(255, 220, 80, 255));
                        dl->AddCircle(spx, r, IM_COL32(20, 20, 30, 255), 0, 1.5f);
                        if (tool == ActiveTool::VectorEdit) {
                            if (pt.hout.Length() > 0.5f) {
                                V2 o = tmp.xform.Map(pt.OutAbs());
                                ImVec2 ho = toSc(o.x, o.y);
                                dl->AddLine(spx, ho, IM_COL32(160, 200, 255, 200), 1.f);
                                dl->AddCircleFilled(ho, 3.5f, IM_COL32(140, 200, 255, 255));
                            }
                            if (pt.hin.Length() > 0.5f) {
                                V2 o = tmp.xform.Map(pt.InAbs());
                                ImVec2 ho = toSc(o.x, o.y);
                                dl->AddLine(spx, ho, IM_COL32(160, 200, 255, 200), 1.f);
                                dl->AddCircleFilled(ho, 3.5f, IM_COL32(140, 200, 255, 255));
                            }
                        }
                    }
                }
            }
        }
    }
}

} // namespace

const char* VectorToolTitle(ActiveTool t) {
    switch (t) {
    case ActiveTool::VectorSelect: return "Select shapes";
    case ActiveTool::VectorEdit: return "Edit nodes";
    case ActiveTool::VectorPen: return "Pen (path)";
    case ActiveTool::VectorRect: return "Rectangle";
    case ActiveTool::VectorEllipse: return "Ellipse";
    case ActiveTool::VectorLine: return "Line";
    case ActiveTool::VectorFreehand: return "Freehand";
    case ActiveTool::VectorPolygon: return "Polygon";
    default: return "Vector";
    }
}

const char* VectorToolHowTo(ActiveTool t) {
    switch (t) {
    case ActiveTool::VectorSelect:
        return "Click · Shift+click multi · drag move · corner resize (Shift uniform). "
               "Enter/dbl-click = Edit. Align/Join in Tool Settings. ]/[ z-order. Ctrl+C/V/D.";
    case ActiveTool::VectorEdit:
        return "Drag nodes/handles · double-click segment = insert · N = node type · "
               "Break splits path at selected node · Esc = Select.";
    case ActiveTool::VectorPen:
        return "Click points (like polygonal lasso). Click near the GREEN first point to close. "
               "Hold-drag for curve handles. Enter = close · Esc = cancel. Draft is overlay-only.";
    case ActiveTool::VectorRect:
        return "Click-drag rectangle. Corner radius in Tool Settings.";
    case ActiveTool::VectorEllipse:
        return "Click-drag ellipse. Style in Tool Settings.";
    case ActiveTool::VectorLine:
        return "Click-drag line (stroke-only). Width in Tool Settings.";
    case ActiveTool::VectorFreehand:
        return "Click-drag freehand path (auto-smoothed). Toggle Closed in Tool Settings.";
    case ActiveTool::VectorPolygon:
        return "Click to add vertices · Enter/double-click finishes · Esc cancels. "
               "Closed toggle in Tool Settings (Polygon vs Polyline).";
    default:
        return "Vector tools work on Vector layers (Layers panel → Vec).";
    }
}

std::string VectorToolLiveStatus(const Canvas& canvas, ActiveTool t) {
    char buf[128];
    if (t == ActiveTool::VectorPen && g_PenActive) {
        std::snprintf(buf, sizeof(buf), "drawing path…");
        return buf;
    }
    if (!g_SelIds.empty()) {
        if (g_SelIds.size() == 1)
            std::snprintf(buf, sizeof(buf), "shape #%u selected", g_SelId);
        else
            std::snprintf(buf, sizeof(buf), "%d shapes selected (primary #%u)",
                (int)g_SelIds.size(), g_SelId);
        return buf;
    }
    int n = 0;
    // count shapes on active vector layer
    int ai = canvas.GetActiveLayerIndex();
    if (ai >= 0 && ai < (int)canvas.GetLayers().size()) {
        const auto& L = canvas.GetLayers()[ai];
        if (L.vectorDoc) n = (int)L.vectorDoc->shapes.size();
    }
    if (n == 0) return "no shapes yet — draw Rect/Ellipse/Pen";
    std::snprintf(buf, sizeof(buf), "%d shape(s) on layer", n);
    return buf;
}

uint32_t VectorSelectedShapeId() { return g_SelId; }
void VectorSetSelectedShapeId(uint32_t id) { SetSoleSelection(id); }
std::vector<uint32_t> VectorSelectedShapeIds() { return g_SelIds; }
int VectorSelectionCount() { return (int)g_SelIds.size(); }

bool& VectorScaleStylesFlag() { return UI::g_VectorToolStyle.scaleStyles; }

bool VectorActionConvertToPath(Canvas& canvas) {
    int li = ActiveVectorLayer(canvas);
    Document* doc = canvas.GetVectorDocument(li);
    if (!doc || !g_SelId) return false;
    Shape* sh = doc->Find(g_SelId);
    if (!sh || sh->kind == ShapeKind::Path) return false;
    std::string before = DocumentToJson(*doc);
    ConvertShapeToPath(*sh);
    CommitDoc(canvas, li, *doc, before, "Convert to path");
    return true;
}

bool VectorActionBringFront(Canvas& canvas) {
    int li = ActiveVectorLayer(canvas);
    Document* doc = canvas.GetVectorDocument(li);
    if (!doc || !g_SelId) return false;
    std::string before = DocumentToJson(*doc);
    if (!BringShapeToFront(*doc, g_SelId)) return false;
    CommitDoc(canvas, li, *doc, before, "Bring to front");
    return true;
}
bool VectorActionSendBack(Canvas& canvas) {
    int li = ActiveVectorLayer(canvas);
    Document* doc = canvas.GetVectorDocument(li);
    if (!doc || !g_SelId) return false;
    std::string before = DocumentToJson(*doc);
    if (!SendShapeToBack(*doc, g_SelId)) return false;
    CommitDoc(canvas, li, *doc, before, "Send to back");
    return true;
}
bool VectorActionRaise(Canvas& canvas) {
    int li = ActiveVectorLayer(canvas);
    Document* doc = canvas.GetVectorDocument(li);
    if (!doc || !g_SelId) return false;
    std::string before = DocumentToJson(*doc);
    if (!RaiseShape(*doc, g_SelId)) return false;
    CommitDoc(canvas, li, *doc, before, "Raise shape");
    return true;
}
bool VectorActionLower(Canvas& canvas) {
    int li = ActiveVectorLayer(canvas);
    Document* doc = canvas.GetVectorDocument(li);
    if (!doc || !g_SelId) return false;
    std::string before = DocumentToJson(*doc);
    if (!LowerShape(*doc, g_SelId)) return false;
    CommitDoc(canvas, li, *doc, before, "Lower shape");
    return true;
}
bool VectorActionDuplicate(Canvas& canvas) {
    int li = ActiveVectorLayer(canvas);
    Document* doc = canvas.GetVectorDocument(li);
    if (!doc || !g_SelId) return false;
    std::string before = DocumentToJson(*doc);
    uint32_t nid = DuplicateShape(*doc, g_SelId);
    if (!nid) return false;
    SetSoleSelection(nid);
    CommitDoc(canvas, li, *doc, before, "Duplicate shape");
    return true;
}
bool VectorActionExportSvg(Canvas& canvas) {
    int li = ActiveVectorLayer(canvas);
    if (li < 0) {
        core::Notifications::Get().Push("Select a Vector layer first.", core::NotifyLevel::Warning);
        return false;
    }
    char path[1024] = {};
    if (!Ui::ShowSaveFile(path, sizeof(path), "SVG (*.svg)\0*.svg\0All Files (*.*)\0*.*\0"))
        return false;
    std::string p = path;
    if (p.size() < 4 || (p.substr(p.size() - 4) != ".svg" && p.substr(p.size() - 4) != ".SVG"))
        p += ".svg";
    if (!canvas.ExportVectorLayerSvg(li, p)) {
        core::Notifications::Get().Push("SVG export failed.", core::NotifyLevel::Error);
        return false;
    }
    core::Notifications::Get().Push("Exported SVG: " + p, core::NotifyLevel::Info);
    return true;
}

bool VectorActionBreakAtNode(Canvas& canvas) {
    int li = ActiveVectorLayer(canvas);
    Document* doc = canvas.GetVectorDocument(li);
    if (!doc || !g_SelId || g_EditSub < 0 || g_EditPt < 0) return false;
    Shape* sh = doc->Find(g_SelId);
    if (!sh) return false;
    std::string before = DocumentToJson(*doc);
    Shape other = BreakPathAtNode(*sh, g_EditSub, g_EditPt, doc->AllocId());
    if (other.path.subs.empty() || other.path.subs[0].points.size() < 2)
        return false;
    doc->shapes.push_back(std::move(other));
    SetSoleSelection(doc->shapes.back().id);
    CommitDoc(canvas, li, *doc, before, "Break path");
    return true;
}

bool VectorActionApplyStyle(Canvas& canvas) {
    int li = ActiveVectorLayer(canvas);
    Document* doc = canvas.GetVectorDocument(li);
    if (!doc || g_SelIds.empty()) return false;
    std::string before = DocumentToJson(*doc);
    ShapeStyle st = StyleFromUi();
    for (uint32_t id : g_SelIds) {
        if (Shape* sh = doc->Find(id)) {
            ApplyStyleToShape(*sh, st);
            MarkDirty(*doc, *sh, canvas.GetWidth(), canvas.GetHeight());
        }
    }
    canvas.CommitVectorEdit(li, before, DocumentToJson(*doc), "Apply style");
    return true;
}

bool VectorActionJoinPaths(Canvas& canvas) {
    int li = ActiveVectorLayer(canvas);
    Document* doc = canvas.GetVectorDocument(li);
    if (!doc || g_SelIds.size() < 2) return false;
    uint32_t idA = g_SelIds[g_SelIds.size() - 2];
    uint32_t idB = g_SelIds.back();
    Shape* a = doc->Find(idA);
    Shape* b = doc->Find(idB);
    if (!a || !b) return false;
    std::string before = DocumentToJson(*doc);
    float tol = 12.f;
    if (!JoinOpenPaths(*a, *b, tol)) {
        core::Notifications::Get().Push(
            "Join failed: need two open paths with nearby endpoints.",
            core::NotifyLevel::Warning);
        return false;
    }
    // remove b
    doc->shapes.erase(
        std::remove_if(doc->shapes.begin(), doc->shapes.end(),
                       [&](const Shape& s) { return s.id == idB; }),
        doc->shapes.end());
    SetSoleSelection(idA);
    CommitDoc(canvas, li, *doc, before, "Join paths");
    core::Notifications::Get().Push("Paths joined.", core::NotifyLevel::Info);
    return true;
}

bool VectorActionAlign(Canvas& canvas, int mode) {
    int li = ActiveVectorLayer(canvas);
    Document* doc = canvas.GetVectorDocument(li);
    if (!doc || g_SelIds.size() < 2) {
        core::Notifications::Get().Push("Align needs 2+ selected shapes (Shift+click).",
                                         core::NotifyLevel::Warning);
        return false;
    }
    std::string before = DocumentToJson(*doc);
    if (!AlignShapes(*doc, g_SelIds, mode)) return false;
    CommitDoc(canvas, li, *doc, before, "Align shapes");
    return true;
}

bool VectorGetSelectionBounds(Canvas& canvas, float& x, float& y, float& w, float& h) {
    int li = ActiveVectorLayer(canvas);
    Document* doc = canvas.GetVectorDocument(li);
    if (!doc || !g_SelId) return false;
    Shape* sh = doc->Find(g_SelId);
    if (!sh) return false;
    float x0, y0, x1, y1;
    if (!ShapeLocalBounds(*sh, x0, y0, x1, y1)) return false;
    x = x0; y = y0; w = x1 - x0; h = y1 - y0;
    return true;
}

bool VectorSetSelectionBounds(Canvas& canvas, float x, float y, float w, float h, bool scaleStyles) {
    int li = ActiveVectorLayer(canvas);
    Document* doc = canvas.GetVectorDocument(li);
    if (!doc || !g_SelId) return false;
    Shape* sh = doc->Find(g_SelId);
    if (!sh) return false;
    std::string before = DocumentToJson(*doc);
    SetShapeTopLeft(*sh, x, y);
    if (!SetShapeSize(*sh, w, h, /*aboutCenter=*/false, scaleStyles))
        return false;
    MarkDirty(*doc, *sh, canvas.GetWidth(), canvas.GetHeight());
    canvas.CommitVectorEdit(li, before, DocumentToJson(*doc), "Transform shape");
    return true;
}

bool VectorGetSelectionRound(Canvas& canvas, float& rx, float& ry) {
    int li = ActiveVectorLayer(canvas);
    Document* doc = canvas.GetVectorDocument(li);
    if (!doc || !g_SelId) return false;
    Shape* sh = doc->Find(g_SelId);
    if (!sh || sh->kind != ShapeKind::Rect) return false;
    rx = sh->param[4];
    ry = sh->param[5];
    return true;
}

bool VectorSetSelectionRound(Canvas& canvas, float rx, float ry) {
    int li = ActiveVectorLayer(canvas);
    Document* doc = canvas.GetVectorDocument(li);
    if (!doc || !g_SelId) return false;
    Shape* sh = doc->Find(g_SelId);
    if (!sh || sh->kind != ShapeKind::Rect) return false;
    std::string before = DocumentToJson(*doc);
    sh->param[4] = std::max(0.f, rx);
    sh->param[5] = std::max(0.f, ry);
    RebuildPathFromParams(*sh);
    MarkDirty(*doc, *sh, canvas.GetWidth(), canvas.GetHeight());
    canvas.CommitVectorEdit(li, before, DocumentToJson(*doc), "Round corners");
    return true;
}

bool UpdateVectorTools(
    Canvas& canvas,
    ID3D11Device* device,
    ActiveTool& activeTool,
    bool isHovered,
    bool canvasInputBlocked,
    float canvasX, float canvasY,
    float viewportW, float viewportH,
    float imageMinX, float imageMinY)
{
    // Leaving vector tools entirely — drop any in-progress draft
    if (!UI::IsVectorTool(activeTool)) {
        if (g_PenActive || g_PolyActive || g_FreehandActive || g_Creating) {
            int li = canvas.GetActiveLayerIndex();
            ClearPenDraft();
            g_PolyActive = false; g_PolyPts.clear();
            g_FreehandActive = false; g_FreehandPts.clear();
            g_Creating = false; g_CreateId = 0;
            if (li >= 0) canvas.EnsureVectorRaster(li, false, true);
        }
        g_PrevVectorTool = activeTool;
        return false;
    }

    g_CursorX = canvasX;
    g_CursorY = canvasY;

    const int docW = canvas.GetWidth();
    const int docH = canvas.GetHeight();
    const float zoom = canvas.GetZoom();
    const auto pan = canvas.GetPan();

    // Keys work even when not hovering (if not typing in UI)
    const bool keysOk = !canvasInputBlocked && !ImGui::GetIO().WantTextInput;
    const bool mouseOk = !canvasInputBlocked && isHovered;

    int vLayer = canvas.GetActiveLayerIndex();
    bool needEnsure = true;
    if (vLayer >= 0 && vLayer < (int)canvas.GetLayers().size() &&
        canvas.GetLayers()[vLayer].IsVector())
        needEnsure = false;

    if (mouseOk || (keysOk && (g_PenActive || g_Creating || g_Dragging || g_EditDrag || g_PolyActive))) {
        if (needEnsure) {
            vLayer = canvas.EnsureActiveVectorLayer(device);
            EnsureLayerNotify(canvas, vLayer);
        }
    } else if (needEnsure && keysOk &&
               (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                ImGui::IsKeyPressed(ImGuiKey_Delete))) {
        if (g_SelId || g_PenActive || g_PolyActive)
            vLayer = canvas.EnsureActiveVectorLayer(device);
    }

    Document* doc = canvas.GetVectorDocument(vLayer);

    // Tool switch hygiene: leaving Pen cancels draft (never leaves half-baked shape in doc)
    if (g_PrevVectorTool == ActiveTool::VectorPen && activeTool != ActiveTool::VectorPen) {
        if (g_PenActive)
            CancelPenDraft(canvas, vLayer);
        else
            ClearPenDraft();
    }
    if (g_PrevVectorTool == ActiveTool::VectorPolygon && activeTool != ActiveTool::VectorPolygon) {
        g_PolyActive = false;
        g_PolyPts.clear();
    }
    if (g_PrevVectorTool == ActiveTool::VectorFreehand && activeTool != ActiveTool::VectorFreehand) {
        g_FreehandActive = false;
        g_FreehandPts.clear();
    }
    g_PrevVectorTool = activeTool;

    // Always draw HUD when vector tool active (even if no hover)
    if (!canvasInputBlocked && docW > 0) {
        DrawHud(activeTool, canvas, vLayer, doc, zoom, pan.x, pan.y,
                viewportW, viewportH, imageMinX, imageMinY);
    }

    if (!doc) return false;

    bool consumed = false;
    float tol = std::max(4.f, 8.f / std::max(0.001f, zoom));
    const float penCloseTol = CloseTolDoc(zoom);

    // ---- Keys ----
    if (keysOk) {
        if (activeTool == ActiveTool::VectorEdit && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            activeTool = ActiveTool::VectorSelect;
            g_EditSub = g_EditPt = -1;
            g_EditDrag = false;
            consumed = true;
        }
        if (activeTool == ActiveTool::VectorPen && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            CancelPenDraft(canvas, vLayer);
            consumed = true;
        }
        if (activeTool == ActiveTool::VectorPen &&
            (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) &&
            g_PenActive) {
            if (g_PenPts.size() >= 3) {
                if (CommitPenDraft(canvas, vLayer, *doc, /*closed=*/true))
                    activeTool = ActiveTool::VectorSelect;
            } else if (g_PenPts.size() == 2) {
                if (CommitPenDraft(canvas, vLayer, *doc, /*closed=*/false))
                    activeTool = ActiveTool::VectorSelect;
            } else {
                CancelPenDraft(canvas, vLayer);
            }
            consumed = true;
        }
        // Enter on select with selection → edit mode
        if (activeTool == ActiveTool::VectorSelect && g_SelId &&
            ImGui::IsKeyPressed(ImGuiKey_Enter) && !g_PolyActive) {
            activeTool = ActiveTool::VectorEdit;
            consumed = true;
        }
        // Polygon finish / cancel
        if (activeTool == ActiveTool::VectorPolygon && g_PolyActive) {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                g_PolyActive = false;
                g_PolyPts.clear();
                consumed = true;
            }
            if ((ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) &&
                g_PolyPts.size() >= 2) {
                const char* nm = UI::g_VectorToolStyle.polygonClosed ? "Polygon" : "Polyline";
                Shape sh = MakeEmptyPath(StyleFromUi(), doc->AllocId());
                sh.path = PolylineToPath(g_PolyPts, 0.5f, UI::g_VectorToolStyle.polygonClosed);
                sh.name = nm;
                doc->shapes.push_back(std::move(sh));
                SetSoleSelection(doc->shapes.back().id);
                CommitDoc(canvas, vLayer, *doc, g_PolyBefore, nm);
                g_PolyActive = false;
                g_PolyPts.clear();
                activeTool = ActiveTool::VectorSelect;
                consumed = true;
            }
        }
        // N = cycle node type (edit)
        if (activeTool == ActiveTool::VectorEdit && g_SelId && g_EditSub >= 0 && g_EditPt >= 0 &&
            ImGui::IsKeyPressed(ImGuiKey_N) && !ImGui::GetIO().KeyCtrl) {
            if (auto* sh = doc->Find(g_SelId)) {
                std::string before = DocumentToJson(*doc);
                CycleNodeType(*sh, g_EditSub, g_EditPt);
                MarkDirty(*doc, *sh, docW, docH);
                canvas.CommitVectorEdit(vLayer, before, DocumentToJson(*doc), "Node type");
                consumed = true;
            }
        }
        // Z-order: ] raise / [ lower · Ctrl+] front · Ctrl+[ back
        if (g_SelId && !ImGui::GetIO().WantTextInput) {
            bool ctrl = ImGui::GetIO().KeyCtrl;
            if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) {
                std::string before = DocumentToJson(*doc);
                bool ok = ctrl ? BringShapeToFront(*doc, g_SelId) : RaiseShape(*doc, g_SelId);
                if (ok) {
                    CommitDoc(canvas, vLayer, *doc, before, ctrl ? "Bring to front" : "Raise");
                    consumed = true;
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) {
                std::string before = DocumentToJson(*doc);
                bool ok = ctrl ? SendShapeToBack(*doc, g_SelId) : LowerShape(*doc, g_SelId);
                if (ok) {
                    CommitDoc(canvas, vLayer, *doc, before, ctrl ? "Send to back" : "Lower");
                    consumed = true;
                }
            }
        }
        // Ctrl+C / Ctrl+V / Ctrl+D
        if (ImGui::GetIO().KeyCtrl && !ImGui::GetIO().WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_C) && g_SelId) {
                if (auto* sh = doc->Find(g_SelId)) {
                    g_ClipboardShape = ShapeToJson(*sh);
                    core::Notifications::Get().Push("Shape copied", core::NotifyLevel::Info);
                    consumed = true;
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_V) && !g_ClipboardShape.empty()) {
                Shape sh;
                if (ShapeFromJson(g_ClipboardShape, sh)) {
                    std::string before = DocumentToJson(*doc);
                    sh.id = doc->AllocId();
                    TranslateShape(sh, 16.f, 16.f);
                    doc->shapes.push_back(sh);
                    SetSoleSelection(sh.id);
                    CommitDoc(canvas, vLayer, *doc, before, "Paste shape");
                    activeTool = ActiveTool::VectorSelect;
                    consumed = true;
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_D) && g_SelId) {
                VectorActionDuplicate(canvas);
                consumed = true;
            }
        }
        if ((ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace))) {
            std::string before = DocumentToJson(*doc);
            if (activeTool == ActiveTool::VectorEdit && g_SelId && g_EditSub >= 0 && g_EditPt >= 0) {
                if (auto* sh = doc->Find(g_SelId)) {
                    ConvertShapeToPath(*sh);
                    if (g_EditSub < (int)sh->path.subs.size()) {
                        auto& pts = sh->path.subs[g_EditSub].points;
                        if (g_EditPt < (int)pts.size() && pts.size() > 1) {
                            MarkDirty(*doc, *sh, docW, docH);
                            pts.erase(pts.begin() + g_EditPt);
                            MarkDirty(*doc, *sh, docW, docH);
                            canvas.CommitVectorEdit(vLayer, before, DocumentToJson(*doc), "Delete node");
                            g_EditPt = -1;
                        }
                    }
                }
                consumed = true;
            } else if (!g_SelIds.empty()) {
                doc->shapes.erase(
                    std::remove_if(doc->shapes.begin(), doc->shapes.end(),
                                   [&](const Shape& s) { return IsSelected(s.id); }),
                    doc->shapes.end());
                doc->MarkAllDirty(docW, docH);
                canvas.CommitVectorEdit(vLayer, before, DocumentToJson(*doc), "Delete shape(s)");
                SetSoleSelection(0);
                consumed = true;
            }
        }
    }

    // Finish freehand if mouse released outside hover
    if (g_FreehandActive && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && doc) {
        g_FreehandActive = false;
        if (g_FreehandPts.size() >= 2) {
            Shape sh = MakeEmptyPath(StyleFromUi(), doc->AllocId());
            float ftol = std::max(1.5f, 3.f / std::max(0.001f, zoom));
            sh.path = PolylineToPath(g_FreehandPts, ftol, UI::g_VectorToolStyle.freehandClosed);
            sh.name = "Freehand";
            if (!sh.path.subs.empty() && sh.path.subs[0].points.size() >= 2) {
                doc->shapes.push_back(std::move(sh));
                SetSoleSelection(doc->shapes.back().id);
                CommitDoc(canvas, vLayer, *doc, g_FreehandBefore, "Freehand path");
                activeTool = ActiveTool::VectorSelect;
            }
        }
        g_FreehandPts.clear();
        consumed = true;
    }

    if (!mouseOk) return consumed;

    // ---- Mouse ----
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        if (activeTool == ActiveTool::VectorSelect) {
            uint32_t hit = HitTestShape(*doc, canvasX, canvasY, tol);
            if (hit) {
                SetSoleSelection(hit);
                activeTool = ActiveTool::VectorEdit;
                consumed = true;
                return consumed;
            }
        } else if (activeTool == ActiveTool::VectorEdit) {
            // Insert node on segment
            uint32_t sid = 0;
            int sub = -1, seg = -1;
            float t = 0.5f;
            if (HitTestSegment(*doc, canvasX, canvasY, tol * 1.5f, sid, sub, seg, t)) {
                if (auto* sh = doc->Find(sid)) {
                    std::string before = DocumentToJson(*doc);
                    ConvertShapeToPath(*sh);
                    int ni = InsertNodeOnSegment(*sh, sub, seg, t);
                    if (ni >= 0) {
                        SetSoleSelection(sid);
                        g_EditSub = sub;
                        g_EditPt = ni;
                        MarkDirty(*doc, *sh, docW, docH);
                        canvas.CommitVectorEdit(vLayer, before, DocumentToJson(*doc), "Insert node");
                        consumed = true;
                        return consumed;
                    }
                }
            }
        } else if (activeTool == ActiveTool::VectorPolygon && g_PolyActive && g_PolyPts.size() >= 2) {
            const char* nm = UI::g_VectorToolStyle.polygonClosed ? "Polygon" : "Polyline";
            Shape sh = MakeEmptyPath(StyleFromUi(), doc->AllocId());
            sh.path = PolylineToPath(g_PolyPts, 0.5f, UI::g_VectorToolStyle.polygonClosed);
            sh.name = nm;
            doc->shapes.push_back(std::move(sh));
            SetSoleSelection(doc->shapes.back().id);
            CommitDoc(canvas, vLayer, *doc, g_PolyBefore, nm);
            g_PolyActive = false;
            g_PolyPts.clear();
            activeTool = ActiveTool::VectorSelect;
            consumed = true;
            return consumed;
        }
    }

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (activeTool == ActiveTool::VectorSelect) {
            g_ResizeHandle = -1;
            // Prefer resize grips on current selection
            if (g_SelId) {
                if (auto* sh = doc->Find(g_SelId)) {
                    int h = HitResizeHandle(*sh, canvasX, canvasY, tol * 1.4f);
                    if (h >= 0) {
                        g_ResizeHandle = h;
                        float x0, y0, x1, y1;
                        ShapeLocalBounds(*sh, x0, y0, x1, y1);
                        g_ResizeStartX0 = x0; g_ResizeStartY0 = y0;
                        g_ResizeStartX1 = x1; g_ResizeStartY1 = y1;
                        g_UndoBefore = DocumentToJson(*doc);
                        consumed = true;
                    }
                }
            }
            if (!consumed) {
                uint32_t hit = HitTestShape(*doc, canvasX, canvasY, tol);
                if (ImGui::GetIO().KeyShift) {
                    if (hit) ToggleSelection(hit);
                } else {
                    // Click already-selected in multi → keep multi and drag all
                    if (!(hit && IsSelected(hit) && g_SelIds.size() > 1))
                        SetSoleSelection(hit);
                    if (hit) {
                        g_Dragging = true;
                        g_DragLastX = canvasX;
                        g_DragLastY = canvasY;
                        g_UndoBefore = DocumentToJson(*doc);
                    }
                }
                consumed = true;
            }
        } else if (activeTool == ActiveTool::VectorEdit) {
            // Prefer handles, then nodes
            uint32_t sid = 0;
            int sub = -1, pt = -1;
            g_HandleKind = 0;
            if (HitTestNode(*doc, canvasX, canvasY, tol, sid, sub, pt)) {
                SetSoleSelection(sid);
                g_EditSub = sub;
                g_EditPt = pt;
                g_EditDrag = true;
                g_UndoBefore = DocumentToJson(*doc);
            } else {
                // try handles of selected shape
                bool hitH = false;
                if (g_SelId) {
                    if (auto* sh = doc->Find(g_SelId)) {
                        Shape tmp = *sh;
                        if (tmp.kind != ShapeKind::Path) RebuildPathFromParams(tmp);
                        for (int si = 0; si < (int)tmp.path.subs.size() && !hitH; ++si) {
                            auto& pts = tmp.path.subs[si].points;
                            for (int pi = 0; pi < (int)pts.size(); ++pi) {
                                V2 p = tmp.xform.Map(pts[pi].p);
                                V2 hin = tmp.xform.Map(pts[pi].InAbs());
                                V2 hout = tmp.xform.Map(pts[pi].OutAbs());
                                // note: avoid name "near" — Windows headers #define near/far
                                auto isNear = [&](V2 q) {
                                    return std::hypot(q.x - canvasX, q.y - canvasY) <= tol;
                                };
                                if (pts[pi].hout.Length() > 0.5f && isNear(hout)) {
                                    g_EditSub = si; g_EditPt = pi; g_HandleKind = 2; hitH = true; break;
                                }
                                if (pts[pi].hin.Length() > 0.5f && isNear(hin)) {
                                    g_EditSub = si; g_EditPt = pi; g_HandleKind = 1; hitH = true; break;
                                }
                                (void)p;
                            }
                        }
                    }
                }
                if (hitH) {
                    g_EditDrag = true;
                    g_UndoBefore = DocumentToJson(*doc);
                } else {
                    g_SelId = HitTestShape(*doc, canvasX, canvasY, tol);
                    g_EditSub = g_EditPt = -1;
                }
            }
            consumed = true;
        } else if (activeTool == ActiveTool::VectorRect ||
                   activeTool == ActiveTool::VectorEllipse ||
                   activeTool == ActiveTool::VectorLine) {
            g_Creating = true;
            g_CreateX0 = canvasX;
            g_CreateY0 = canvasY;
            g_UndoBefore = DocumentToJson(*doc);
            Shape sh;
            if (activeTool == ActiveTool::VectorRect) {
                sh = MakeRect(canvasX, canvasY, 1.f, 1.f, StyleFromUi(), doc->AllocId());
                sh.param[4] = UI::g_VectorToolStyle.rectCornerRx;
                sh.param[5] = UI::g_VectorToolStyle.rectCornerRy;
                RebuildPathFromParams(sh);
            } else if (activeTool == ActiveTool::VectorEllipse)
                sh = MakeEllipse(canvasX, canvasY, 1.f, 1.f, StyleFromUi(), doc->AllocId());
            else
                sh = MakeLine(canvasX, canvasY, canvasX + 1.f, canvasY, StyleFromUi(), doc->AllocId());
            g_CreateId = sh.id;
            doc->shapes.push_back(std::move(sh));
            consumed = true;
        } else if (activeTool == ActiveTool::VectorPen) {
            // --- Draft-only pen (no document / tile mutation until commit) ---
            // Double-click first (same frame as Clicked) — like polygonal lasso.
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (g_PenActive && g_PenPts.size() >= 2) {
                    bool closed = g_PenPts.size() >= 3;
                    if (CommitPenDraft(canvas, vLayer, *doc, closed))
                        activeTool = ActiveTool::VectorSelect;
                } else {
                    CancelPenDraft(canvas, vLayer);
                }
            } else if (!g_PenActive) {
                g_PenActive = true;
                g_PenPts.clear();
                g_PenBefore = DocumentToJson(*doc);
                PathPoint p;
                p.p = {canvasX, canvasY};
                g_PenPts.push_back(p);
                g_PenDragHandle = true;
            } else if (g_PenPts.size() >= 3 &&
                       NearDoc(canvasX, canvasY, g_PenPts[0].p.x, g_PenPts[0].p.y, penCloseTol)) {
                // Click near first point → close (polygonal-lasso UX)
                if (CommitPenDraft(canvas, vLayer, *doc, /*closed=*/true))
                    activeTool = ActiveTool::VectorSelect;
            } else {
                PathPoint p;
                p.p = {canvasX, canvasY};
                g_PenPts.push_back(p);
                g_PenDragHandle = true;
            }
            consumed = true;
        } else if (activeTool == ActiveTool::VectorFreehand) {
            g_FreehandActive = true;
            g_FreehandPts.clear();
            g_FreehandPts.push_back({canvasX, canvasY});
            g_FreehandBefore = DocumentToJson(*doc);
            consumed = true;
        } else if (activeTool == ActiveTool::VectorPolygon) {
            if (!g_PolyActive) {
                g_PolyActive = true;
                g_PolyPts.clear();
                g_PolyBefore = DocumentToJson(*doc);
            }
            // Close if click near first point
            if (g_PolyPts.size() >= 3) {
                float d0 = std::hypot(canvasX - g_PolyPts[0].x, canvasY - g_PolyPts[0].y);
                if (d0 <= tol * 1.5f) {
                    Shape sh = MakeEmptyPath(StyleFromUi(), doc->AllocId());
                    sh.path = PolylineToPath(g_PolyPts, 0.5f, true);
                    sh.name = "Polygon";
                    doc->shapes.push_back(std::move(sh));
                    SetSoleSelection(doc->shapes.back().id);
                    CommitDoc(canvas, vLayer, *doc, g_PolyBefore, "Polygon");
                    g_PolyActive = false;
                    g_PolyPts.clear();
                    activeTool = ActiveTool::VectorSelect;
                    consumed = true;
                    return true;
                }
            }
            g_PolyPts.push_back({canvasX, canvasY});
            consumed = true;
        }
    }

    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        if (g_ResizeHandle >= 0 && g_SelId) {
            if (auto* sh = doc->Find(g_SelId)) {
                float x0 = g_ResizeStartX0, y0 = g_ResizeStartY0;
                float x1 = g_ResizeStartX1, y1 = g_ResizeStartY1;
                // Move the corner being dragged
                if (g_ResizeHandle == 0) { x0 = canvasX; y0 = canvasY; }
                else if (g_ResizeHandle == 1) { x1 = canvasX; y0 = canvasY; }
                else if (g_ResizeHandle == 2) { x1 = canvasX; y1 = canvasY; }
                else if (g_ResizeHandle == 3) { x0 = canvasX; y1 = canvasY; }
                // Normalize
                float nx0 = std::min(x0, x1), ny0 = std::min(y0, y1);
                float nx1 = std::max(x0, x1), ny1 = std::max(y0, y1);
                float nw = std::max(1.f, nx1 - nx0), nh = std::max(1.f, ny1 - ny0);
                if (ImGui::GetIO().KeyShift) {
                    // uniform from original aspect
                    float ow = std::max(1.f, g_ResizeStartX1 - g_ResizeStartX0);
                    float oh = std::max(1.f, g_ResizeStartY1 - g_ResizeStartY0);
                    float aspect = ow / oh;
                    if (nw / nh > aspect) nh = nw / aspect;
                    else nw = nh * aspect;
                    // keep opposite corner fixed
                    if (g_ResizeHandle == 0) { nx0 = nx1 - nw; ny0 = ny1 - nh; }
                    else if (g_ResizeHandle == 1) { nx1 = nx0 + nw; ny0 = ny1 - nh; }
                    else if (g_ResizeHandle == 2) { nx1 = nx0 + nw; ny1 = ny0 + nh; }
                    else { nx0 = nx1 - nw; ny1 = ny0 + nh; }
                }
                // Restore start geometry then apply
                *sh = Shape{}; // will re-parse from undo snapshot each frame is expensive
                // simpler: set from current undo snapshot once
                static std::string s_resizeSnap;
                if (s_resizeSnap != g_UndoBefore) {
                    s_resizeSnap = g_UndoBefore;
                }
                Document tmp;
                if (DocumentFromJson(g_UndoBefore, tmp)) {
                    if (Shape* base = tmp.Find(g_SelId)) {
                        *sh = *base;
                        SetShapeTopLeft(*sh, nx0, ny0);
                        SetShapeSize(*sh, std::max(1.f, nx1 - nx0), std::max(1.f, ny1 - ny0),
                                     false, UI::g_VectorToolStyle.scaleStyles);
                        MarkDirty(*doc, *sh, docW, docH);
                        canvas.EnsureVectorRaster(vLayer, true);
                    }
                }
            }
            consumed = true;
        } else if (g_Dragging && !g_SelIds.empty()) {
            float dx = canvasX - g_DragLastX;
            float dy = canvasY - g_DragLastY;
            for (uint32_t id : g_SelIds) {
                if (auto* sh = doc->Find(id)) {
                    MarkDirty(*doc, *sh, docW, docH);
                    TranslateShape(*sh, dx, dy);
                    MarkDirty(*doc, *sh, docW, docH);
                }
            }
            canvas.EnsureVectorRaster(vLayer, true);
            g_DragLastX = canvasX;
            g_DragLastY = canvasY;
            consumed = true;
        } else if (g_EditDrag && g_SelId && g_EditSub >= 0 && g_EditPt >= 0) {
            if (auto* sh = doc->Find(g_SelId)) {
                ConvertShapeToPath(*sh);
                if (g_EditSub < (int)sh->path.subs.size()) {
                    auto& pts = sh->path.subs[g_EditSub].points;
                    if (g_EditPt < (int)pts.size()) {
                        MarkDirty(*doc, *sh, docW, docH);
                        // local space (inverse translate-only xform)
                        float lx = canvasX - sh->xform.e;
                        float ly = canvasY - sh->xform.f;
                        if (g_HandleKind == 0) {
                            pts[g_EditPt].p = {lx, ly};
                        } else if (g_HandleKind == 1) {
                            pts[g_EditPt].hin = {lx - pts[g_EditPt].p.x, ly - pts[g_EditPt].p.y};
                            if (pts[g_EditPt].type == NodeType::Symmetric)
                                pts[g_EditPt].hout = pts[g_EditPt].hin * -1.f;
                            else if (pts[g_EditPt].type == NodeType::Smooth) {
                                float L = pts[g_EditPt].hout.Length();
                                V2 dir = (pts[g_EditPt].hin * -1.f).Normalized();
                                pts[g_EditPt].hout = dir * L;
                            }
                        } else if (g_HandleKind == 2) {
                            pts[g_EditPt].hout = {lx - pts[g_EditPt].p.x, ly - pts[g_EditPt].p.y};
                            if (pts[g_EditPt].type == NodeType::Symmetric)
                                pts[g_EditPt].hin = pts[g_EditPt].hout * -1.f;
                            else if (pts[g_EditPt].type == NodeType::Smooth) {
                                float L = pts[g_EditPt].hin.Length();
                                V2 dir = (pts[g_EditPt].hout * -1.f).Normalized();
                                pts[g_EditPt].hin = dir * L;
                            }
                        }
                        MarkDirty(*doc, *sh, docW, docH);
                        canvas.EnsureVectorRaster(vLayer, true);
                    }
                }
            }
            consumed = true;
        } else if (g_Creating && g_CreateId) {
            if (auto* sh = doc->Find(g_CreateId)) {
                MarkDirty(*doc, *sh, docW, docH);
                if (sh->kind == ShapeKind::Rect) {
                    sh->param[0] = std::min(g_CreateX0, canvasX);
                    sh->param[1] = std::min(g_CreateY0, canvasY);
                    sh->param[2] = std::max(1.f, std::abs(canvasX - g_CreateX0));
                    sh->param[3] = std::max(1.f, std::abs(canvasY - g_CreateY0));
                    RebuildPathFromParams(*sh);
                } else if (sh->kind == ShapeKind::Ellipse) {
                    sh->param[0] = (g_CreateX0 + canvasX) * 0.5f;
                    sh->param[1] = (g_CreateY0 + canvasY) * 0.5f;
                    sh->param[2] = std::max(0.5f, std::abs(canvasX - g_CreateX0) * 0.5f);
                    sh->param[3] = std::max(0.5f, std::abs(canvasY - g_CreateY0) * 0.5f);
                    RebuildPathFromParams(*sh);
                } else if (sh->kind == ShapeKind::Line) {
                    sh->param[0] = g_CreateX0;
                    sh->param[1] = g_CreateY0;
                    sh->param[2] = canvasX;
                    sh->param[3] = canvasY;
                    RebuildPathFromParams(*sh);
                }
                MarkDirty(*doc, *sh, docW, docH);
                canvas.EnsureVectorRaster(vLayer, true);
            }
            consumed = true;
        } else if (g_PenActive && g_PenDragHandle && !g_PenPts.empty()) {
            // Hold-drag after click: curve handles on last draft point (no raster)
            auto& last = g_PenPts.back();
            last.hout = {canvasX - last.p.x, canvasY - last.p.y};
            last.hin = last.hout * -1.f;
            last.type = NodeType::Symmetric;
            consumed = true;
        } else if (g_FreehandActive) {
            if (g_FreehandPts.empty() ||
                std::hypot(canvasX - g_FreehandPts.back().x, canvasY - g_FreehandPts.back().y) >= 2.f) {
                g_FreehandPts.push_back({canvasX, canvasY});
            }
            consumed = true;
        }
    }

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (g_ResizeHandle >= 0 && g_SelId) {
            canvas.CommitVectorEdit(vLayer, g_UndoBefore, DocumentToJson(*doc), "Resize shape");
            canvas.EnsureVectorRaster(vLayer, false, true);
            g_ResizeHandle = -1;
            consumed = true;
        }
        if (g_Dragging && g_SelId) {
            canvas.CommitVectorEdit(vLayer, g_UndoBefore, DocumentToJson(*doc), "Move shape");
            canvas.EnsureVectorRaster(vLayer, false, true);
            g_Dragging = false;
            consumed = true;
        }
        if (g_EditDrag) {
            canvas.CommitVectorEdit(vLayer, g_UndoBefore, DocumentToJson(*doc), "Edit node");
            canvas.EnsureVectorRaster(vLayer, false, true);
            g_EditDrag = false;
            g_HandleKind = 0;
            consumed = true;
        }
        if (g_PenDragHandle) {
            g_PenDragHandle = false;
            consumed = true;
        }
        if (g_Creating && g_CreateId) {
            // Reject tiny accidental clicks
            bool tiny = false;
            if (auto* sh = doc->Find(g_CreateId)) {
                if (sh->kind == ShapeKind::Rect || sh->kind == ShapeKind::Ellipse) {
                    if (sh->param[2] < 2.f && sh->param[3] < 2.f) tiny = true;
                }
            }
            if (tiny) {
                doc->shapes.erase(
                    std::remove_if(doc->shapes.begin(), doc->shapes.end(),
                                   [&](const Shape& s) { return s.id == g_CreateId; }),
                    doc->shapes.end());
                doc->MarkAllDirty(docW, docH);
                canvas.EnsureVectorRaster(vLayer, false);
            } else {
                canvas.CommitVectorEdit(vLayer, g_UndoBefore, DocumentToJson(*doc), "Create shape");
                SetSoleSelection(g_CreateId);
                // After create → Select so user can move it (discoverable)
                activeTool = ActiveTool::VectorSelect;
                core::Notifications::Get().Push(
                    "Shape created → Select mode: drag to move, double-click to edit nodes.",
                    core::NotifyLevel::Info);
            }
            g_Creating = false;
            g_CreateId = 0;
            consumed = true;
        }
        if (g_FreehandActive) {
            g_FreehandActive = false;
            if (g_FreehandPts.size() >= 2) {
                Shape sh = MakeEmptyPath(StyleFromUi(), doc->AllocId());
                float tol = std::max(1.5f, 3.f / std::max(0.001f, zoom));
                sh.path = PolylineToPath(g_FreehandPts, tol, UI::g_VectorToolStyle.freehandClosed);
                sh.name = "Freehand";
                if (!sh.path.subs.empty() && sh.path.subs[0].points.size() >= 2) {
                    doc->shapes.push_back(std::move(sh));
                    SetSoleSelection(doc->shapes.back().id);
                    CommitDoc(canvas, vLayer, *doc, g_FreehandBefore, "Freehand path");
                    activeTool = ActiveTool::VectorSelect;
                    core::Notifications::Get().Push(
                        "Freehand path created → Select mode.", core::NotifyLevel::Info);
                }
            }
            g_FreehandPts.clear();
            consumed = true;
        }
    }

    return consumed;
}

} // namespace vec
