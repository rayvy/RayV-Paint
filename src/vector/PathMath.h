#pragma once
#include "PathTypes.h"
#include <vector>

namespace vec {

// Build path geometry from parametric shapes (mutates shape.path).
void RebuildPathFromParams(Shape& s);

// Convert any shape to editable Path kind (keeps path, clears parametric meaning).
void ConvertShapeToPath(Shape& s);

// Apply affine to all path points (absolute); sets xform to identity.
void BakeTransformIntoPath(Shape& s);

// Flatten subpath to polyline (document space after xform).
// tol: max deviation in px; minSteps/maxSteps clamp Bezier subdivision.
void FlattenSubPath(const SubPath& sp, const Affine& xform, float tol,
                    std::vector<V2>& out, bool coarse = false);

void FlattenShape(const Shape& s, float tol, std::vector<std::vector<V2>>& outLoops,
                  bool coarse = false);

// Axis-aligned bounds of shape in document space (includes stroke pad if strokeOn).
bool ShapeBounds(const Shape& s, float& outX0, float& outY0, float& outX1, float& outY1,
                 bool includeStroke = true);

bool DocumentBounds(const Document& doc, float& outX0, float& outY0, float& outX1, float& outY1);

// Hit-test: nearest shape under point (top-most first). Returns shape id or 0.
uint32_t HitTestShape(const Document& doc, float x, float y, float tol);

// Hit-test path node. Returns true + shapeId, subIdx, ptIdx.
bool HitTestNode(const Document& doc, float x, float y, float tol,
                 uint32_t& outShapeId, int& outSub, int& outPt);

// Hit-test closest path segment. Returns true + shapeId, subIdx, segment start pt index, t in [0,1].
bool HitTestSegment(const Document& doc, float x, float y, float tol,
                    uint32_t& outShapeId, int& outSub, int& outSegStart, float& outT);

// Insert a node on segment (after segStart). Returns new point index or -1.
int InsertNodeOnSegment(Shape& s, int subIdx, int segStart, float t);

// Cycle Corner → Smooth → Symmetric → Corner for a node.
void CycleNodeType(Shape& s, int subIdx, int ptIdx);

// Z-order within document (returns false if shape not found).
bool BringShapeToFront(Document& doc, uint32_t id);
bool SendShapeToBack(Document& doc, uint32_t id);
bool RaiseShape(Document& doc, uint32_t id);
bool LowerShape(Document& doc, uint32_t id);

// Duplicate shape with new id (appended). Returns new id or 0.
uint32_t DuplicateShape(Document& doc, uint32_t id);

// Helpers to build common shapes
Shape MakeRect(float x, float y, float w, float h, const ShapeStyle& style, uint32_t id);
Shape MakeEllipse(float cx, float cy, float rx, float ry, const ShapeStyle& style, uint32_t id);
Shape MakeLine(float x1, float y1, float x2, float y2, const ShapeStyle& style, uint32_t id);
Shape MakeEmptyPath(const ShapeStyle& style, uint32_t id);

// Freehand: polyline samples → cubic path (Douglas-Peucker + handle estimate).
// closed=true closes the path. Returns empty path if < 2 points.
Path PolylineToPath(const std::vector<V2>& samples, float simplifyTol, bool closed);

// Translate shape (updates param + path + xform e,f)
void TranslateShape(Shape& s, float dx, float dy);

// Axis-aligned bounds in document space (geometry only, no stroke pad).
bool ShapeLocalBounds(const Shape& s, float& x0, float& y0, float& x1, float& y1);

// Set top-left of bounds (translate so min = x,y).
void SetShapeTopLeft(Shape& s, float x, float y);

// Set size of AABB (scale about top-left or center). scaleStyles multiplies strokeWidth.
// Returns false if degenerate.
bool SetShapeSize(Shape& s, float newW, float newH, bool aboutCenter, bool scaleStyles);

// Uniform/non-uniform scale about center.
void ScaleShapeAboutCenter(Shape& s, float sx, float sy, bool scaleStyles);

// Break open path at node (split into two shapes). Returns new shape id or 0.
// Caller must insert returned shape into document.
Shape BreakPathAtNode(Shape& s, int subIdx, int ptIdx, uint32_t newId);

// Apply fill/stroke style to shape (does not touch geometry).
void ApplyStyleToShape(Shape& s, const ShapeStyle& style);

// Join two open path shapes if any endpoints are within tol. Merges `b` into `a`, returns true.
// On success caller should remove `b` from document.
bool JoinOpenPaths(Shape& a, Shape& b, float tol);

// Align shapes (ids) within document. mode: 0=L 1=HCenter 2=R 3=T 4=VCenter 5=B
// relativeTo: union of selection bounds.
bool AlignShapes(Document& doc, const std::vector<uint32_t>& ids, int mode);

// JSON (nlohmann-free string form for undo / .rayp) — implemented in PathMath.cpp using nlohmann
std::string DocumentToJson(const Document& doc);
bool DocumentFromJson(const std::string& json, Document& out);

// Single shape JSON (clipboard)
std::string ShapeToJson(const Shape& s);
bool ShapeFromJson(const std::string& json, Shape& out);

} // namespace vec
