#pragma once
// First-class vector geometry (Krita-like shape layer data model).
// Document pixels; truth is geometry — tileCache is a disposable raster projection.

#include <cstdint>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

namespace vec {

struct V2 {
    float x = 0.f, y = 0.f;
    V2() = default;
    V2(float x_, float y_) : x(x_), y(y_) {}
    V2 operator+(const V2& o) const { return {x + o.x, y + o.y}; }
    V2 operator-(const V2& o) const { return {x - o.x, y - o.y}; }
    V2 operator*(float s) const { return {x * s, y * s}; }
    float Length() const { return std::sqrt(x * x + y * y); }
    V2 Normalized() const {
        float L = Length();
        return L > 1e-8f ? V2(x / L, y / L) : V2(1.f, 0.f);
    }
};

enum class NodeType : uint8_t { Corner = 0, Smooth = 1, Symmetric = 2 };

// Cubic Bezier node: incoming handle = p+hin, outgoing = p+hout (relative).
struct PathPoint {
    V2 p;
    V2 hin;  // relative to p (incoming)
    V2 hout; // relative to p (outgoing)
    NodeType type = NodeType::Corner;

    V2 InAbs() const { return p + hin; }
    V2 OutAbs() const { return p + hout; }
};

struct SubPath {
    std::vector<PathPoint> points;
    bool closed = false;
};

struct Path {
    std::vector<SubPath> subs;
};

enum class ShapeKind : uint8_t {
    Path = 0,
    Rect = 1,
    Ellipse = 2,
    Line = 3
};

enum class FillPaint : uint8_t { Solid = 0, LinearGrad = 1 };

struct ShapeStyle {
    bool fillEnabled = true;
    float fillRgba[4] = {0.2f, 0.45f, 0.95f, 1.f};
    bool strokeEnabled = true;
    float strokeRgba[4] = {0.05f, 0.05f, 0.08f, 1.f};
    float strokeWidth = 2.f; // document px
    // 0 = nonzero, 1 = even-odd
    uint8_t fillRule = 0;

    // Linear gradient (document px endpoints; if both ends ~0 use shape AABB L→R)
    FillPaint fillPaint = FillPaint::Solid;
    float gradRgba0[4] = {0.2f, 0.45f, 0.95f, 1.f};
    float gradRgba1[4] = {0.95f, 0.35f, 0.55f, 1.f};
    float gradX0 = 0.f, gradY0 = 0.f, gradX1 = 0.f, gradY1 = 0.f;
    bool gradUseShapeBounds = true; // ignore grad endpoints, use AABB left→right

    // Stroke dash (0 = solid). Lengths in document px along path.
    float dashLen = 0.f;
    float gapLen = 0.f;
};

// Affine 2×3: [a c e; b d f] maps (x,y) → (a*x + c*y + e, b*x + d*y + f)
struct Affine {
    float a = 1.f, b = 0.f, c = 0.f, d = 1.f, e = 0.f, f = 0.f;
    V2 Map(V2 p) const { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }
    static Affine Identity() { return {}; }
    static Affine Translate(float x, float y) {
        Affine t; t.e = x; t.f = y; return t;
    }
};

struct Shape {
    uint32_t id = 0;
    std::string name;
    ShapeKind kind = ShapeKind::Path;
    Affine xform = Affine::Identity();
    ShapeStyle style;
    // Path geometry (always used after convert; for Rect/Ellipse/Line also parametric)
    Path path;
    // Parametric (Rect / Ellipse / Line) before convert-to-path
    float param[8] = {}; // Rect: x,y,w,h,rx,ry; Ellipse: cx,cy,rx,ry; Line: x1,y1,x2,y2
    bool visible = true;
};

struct Document {
    std::vector<Shape> shapes;
    bool antialias = true;
    uint32_t nextId = 1;
    // Optional original SVG for re-export fidelity
    std::string sourceSvg;
    // Dirty rect in document space (x1 < x0 ⇒ clean / empty)
    int dirtyX0 = 0, dirtyY0 = 0, dirtyX1 = -1, dirtyY1 = -1;
    uint64_t generation = 0;
    // Last successful raster fingerprint
    uint64_t rasterGen = 0;

    uint32_t AllocId() { return nextId++; }

    void MarkDirty(int x0, int y0, int x1, int y1) {
        if (x1 < x0 || y1 < y0) return;
        if (dirtyX1 < dirtyX0) {
            dirtyX0 = x0; dirtyY0 = y0; dirtyX1 = x1; dirtyY1 = y1;
        } else {
            dirtyX0 = std::min(dirtyX0, x0);
            dirtyY0 = std::min(dirtyY0, y0);
            dirtyX1 = std::max(dirtyX1, x1);
            dirtyY1 = std::max(dirtyY1, y1);
        }
        ++generation;
    }
    void MarkAllDirty(int docW, int docH) {
        dirtyX0 = 0; dirtyY0 = 0;
        dirtyX1 = std::max(0, docW); dirtyY1 = std::max(0, docH);
        ++generation;
    }
    void ClearDirty() { dirtyX1 = -1; dirtyX0 = 0; dirtyY0 = 0; dirtyY1 = -1; }
    bool HasDirty() const { return dirtyX1 >= dirtyX0 && dirtyY1 >= dirtyY0; }

    Shape* Find(uint32_t id) {
        for (auto& s : shapes)
            if (s.id == id) return &s;
        return nullptr;
    }
    const Shape* Find(uint32_t id) const {
        for (const auto& s : shapes)
            if (s.id == id) return &s;
        return nullptr;
    }
};

// Default drawing style from UI
struct ToolStyle {
    float fillRgba[4] = {0.25f, 0.55f, 0.95f, 1.f};
    float strokeRgba[4] = {0.08f, 0.08f, 0.1f, 1.f};
    float strokeWidth = 2.f;
    bool fillEnabled = true;
    bool strokeEnabled = true;
};

} // namespace vec
