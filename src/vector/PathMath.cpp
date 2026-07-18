#include "PathMath.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <cfloat>
#include <algorithm>

namespace vec {
namespace {

void CubicPoints(const V2& p0, const V2& p1, const V2& p2, const V2& p3, float tol,
                 std::vector<V2>& out, int depth) {
    // de Casteljau flatness
    float dx = p3.x - p0.x, dy = p3.y - p0.y;
    float d1 = std::abs((p1.x - p3.x) * dy - (p1.y - p3.y) * dx);
    float d2 = std::abs((p2.x - p3.x) * dy - (p2.y - p3.y) * dx);
    if ((d1 + d2) * (d1 + d2) <= tol * tol * (dx * dx + dy * dy) || depth > 10) {
        out.push_back(p3);
        return;
    }
    V2 p01 = (p0 + p1) * 0.5f, p12 = (p1 + p2) * 0.5f, p23 = (p2 + p3) * 0.5f;
    V2 p012 = (p01 + p12) * 0.5f, p123 = (p12 + p23) * 0.5f;
    V2 p0123 = (p012 + p123) * 0.5f;
    CubicPoints(p0, p01, p012, p0123, tol, out, depth + 1);
    CubicPoints(p0123, p123, p23, p3, tol, out, depth + 1);
}

Path RectToPath(float x, float y, float w, float h, float rx, float ry) {
    Path path;
    SubPath sp;
    sp.closed = true;
    rx = std::clamp(rx, 0.f, std::abs(w) * 0.5f);
    ry = std::clamp(ry, 0.f, std::abs(h) * 0.5f);
    if (w < 0) { x += w; w = -w; }
    if (h < 0) { y += h; h = -h; }
    if (rx < 0.5f && ry < 0.5f) {
        sp.points.push_back({V2(x, y), {}, {}, NodeType::Corner});
        sp.points.push_back({V2(x + w, y), {}, {}, NodeType::Corner});
        sp.points.push_back({V2(x + w, y + h), {}, {}, NodeType::Corner});
        sp.points.push_back({V2(x, y + h), {}, {}, NodeType::Corner});
    } else {
        // Rounded rect as cubic corners (kappa ≈ 0.552 for quarter-circle)
        const float k = 0.5522847498f;
        auto corner = [&](float px, float py, float hix, float hiy, float hox, float hoy) {
            PathPoint n;
            n.p = {px, py};
            n.hin = {hix, hiy};
            n.hout = {hox, hoy};
            n.type = NodeType::Symmetric;
            sp.points.push_back(n);
        };
        // Top edge mid-ish: start after top-left radius, end before top-right
        // Clockwise from top-left arc end
        corner(x + rx, y,         -k * rx, 0,  k * rx, 0);          // top-left of top edge
        corner(x + w - rx, y,     -k * rx, 0,  k * rx, 0);          // top-right of top edge
        corner(x + w, y + ry,     0, -k * ry,  0, k * ry);          // top-right of right edge
        corner(x + w, y + h - ry, 0, -k * ry,  0, k * ry);
        corner(x + w - rx, y + h, k * rx, 0,  -k * rx, 0);
        corner(x + rx, y + h,     k * rx, 0,  -k * rx, 0);
        corner(x, y + h - ry,     0, k * ry,  0, -k * ry);
        corner(x, y + ry,         0, k * ry,  0, -k * ry);
    }
    path.subs.push_back(std::move(sp));
    return path;
}

Path EllipseToPath(float cx, float cy, float rx, float ry) {
    // 4 cubic Beziers approximating ellipse
    const float k = 0.5522847498f;
    Path path;
    SubPath sp;
    sp.closed = true;
    auto pt = [&](float x, float y, float hix, float hiy, float hox, float hoy) {
        PathPoint n;
        n.p = {x, y};
        n.hin = {hix, hiy};
        n.hout = {hox, hoy};
        n.type = NodeType::Symmetric;
        sp.points.push_back(n);
    };
    // right, bottom, left, top
    pt(cx + rx, cy, 0, -k * ry, 0, k * ry);
    pt(cx, cy + ry, k * rx, 0, -k * rx, 0);
    pt(cx - rx, cy, 0, k * ry, 0, -k * ry);
    pt(cx, cy - ry, -k * rx, 0, k * rx, 0);
    path.subs.push_back(std::move(sp));
    return path;
}

Path LineToPath(float x1, float y1, float x2, float y2) {
    Path path;
    SubPath sp;
    sp.closed = false;
    sp.points.push_back({V2(x1, y1), {}, {}, NodeType::Corner});
    sp.points.push_back({V2(x2, y2), {}, {}, NodeType::Corner});
    path.subs.push_back(std::move(sp));
    return path;
}

} // namespace

void RebuildPathFromParams(Shape& s) {
    switch (s.kind) {
    case ShapeKind::Rect:
        s.path = RectToPath(s.param[0], s.param[1], s.param[2], s.param[3], s.param[4], s.param[5]);
        break;
    case ShapeKind::Ellipse:
        s.path = EllipseToPath(s.param[0], s.param[1], s.param[2], s.param[3]);
        break;
    case ShapeKind::Line:
        s.path = LineToPath(s.param[0], s.param[1], s.param[2], s.param[3]);
        break;
    default:
        break;
    }
}

void ConvertShapeToPath(Shape& s) {
    if (s.kind != ShapeKind::Path)
        RebuildPathFromParams(s);
    s.kind = ShapeKind::Path;
}

void BakeTransformIntoPath(Shape& s) {
    if (s.kind != ShapeKind::Path)
        RebuildPathFromParams(s);
    for (auto& sub : s.path.subs) {
        for (auto& pt : sub.points) {
            V2 p = s.xform.Map(pt.p);
            V2 inA = s.xform.Map(pt.InAbs());
            V2 outA = s.xform.Map(pt.OutAbs());
            pt.p = p;
            pt.hin = inA - p;
            pt.hout = outA - p;
        }
    }
    s.xform = Affine::Identity();
    if (s.kind == ShapeKind::Rect || s.kind == ShapeKind::Ellipse || s.kind == ShapeKind::Line)
        s.kind = ShapeKind::Path;
}

void FlattenSubPath(const SubPath& sp, const Affine& xform, float tol,
                    std::vector<V2>& out, bool coarse) {
    out.clear();
    if (sp.points.empty()) return;
    float t = coarse ? std::max(tol * 4.f, 2.f) : tol;
    const int n = (int)sp.points.size();
    auto mapP = [&](const PathPoint& pt) { return xform.Map(pt.p); };
    out.push_back(mapP(sp.points[0]));
    int segs = sp.closed ? n : n - 1;
    for (int i = 0; i < segs; ++i) {
        const PathPoint& a = sp.points[i];
        const PathPoint& b = sp.points[(i + 1) % n];
        V2 p0 = xform.Map(a.p);
        V2 p3 = xform.Map(b.p);
        V2 c1 = xform.Map(a.OutAbs());
        V2 c2 = xform.Map(b.InAbs());
        bool straight = (a.hout.x * a.hout.x + a.hout.y * a.hout.y < 1e-8f) &&
                        (b.hin.x * b.hin.x + b.hin.y * b.hin.y < 1e-8f);
        if (straight) {
            out.push_back(p3);
        } else {
            CubicPoints(p0, c1, c2, p3, t, out, 0);
        }
    }
}

void FlattenShape(const Shape& s, float tol, std::vector<std::vector<V2>>& outLoops, bool coarse) {
    outLoops.clear();
    Shape tmp = s;
    if (tmp.kind != ShapeKind::Path)
        RebuildPathFromParams(tmp);
    for (const auto& sub : tmp.path.subs) {
        std::vector<V2> loop;
        FlattenSubPath(sub, tmp.xform, tol, loop, coarse);
        if (loop.size() >= 2)
            outLoops.push_back(std::move(loop));
    }
}

bool ShapeBounds(const Shape& s, float& outX0, float& outY0, float& outX1, float& outY1,
                 bool includeStroke) {
    std::vector<std::vector<V2>> loops;
    FlattenShape(s, 1.f, loops, true);
    if (loops.empty()) return false;
    float x0 = FLT_MAX, y0 = FLT_MAX, x1 = -FLT_MAX, y1 = -FLT_MAX;
    for (const auto& loop : loops) {
        for (const auto& p : loop) {
            x0 = std::min(x0, p.x); y0 = std::min(y0, p.y);
            x1 = std::max(x1, p.x); y1 = std::max(y1, p.y);
        }
    }
    float pad = 1.f;
    if (includeStroke && s.style.strokeEnabled)
        pad += s.style.strokeWidth * 0.5f + 1.f;
    outX0 = x0 - pad; outY0 = y0 - pad;
    outX1 = x1 + pad; outY1 = y1 + pad;
    return true;
}

bool DocumentBounds(const Document& doc, float& outX0, float& outY0, float& outX1, float& outY1) {
    bool any = false;
    for (const auto& s : doc.shapes) {
        if (!s.visible) continue;
        float a, b, c, d;
        if (!ShapeBounds(s, a, b, c, d, true)) continue;
        if (!any) { outX0 = a; outY0 = b; outX1 = c; outY1 = d; any = true; }
        else {
            outX0 = std::min(outX0, a); outY0 = std::min(outY0, b);
            outX1 = std::max(outX1, c); outY1 = std::max(outY1, d);
        }
    }
    return any;
}

static float DistPtSeg(float px, float py, float x0, float y0, float x1, float y1) {
    float dx = x1 - x0, dy = y1 - y0;
    float len2 = dx * dx + dy * dy;
    float t = len2 > 1e-12f ? ((px - x0) * dx + (py - y0) * dy) / len2 : 0.f;
    t = std::clamp(t, 0.f, 1.f);
    float qx = x0 + t * dx, qy = y0 + t * dy;
    float ex = px - qx, ey = py - qy;
    return std::sqrt(ex * ex + ey * ey);
}

uint32_t HitTestShape(const Document& doc, float x, float y, float tol) {
    for (int i = (int)doc.shapes.size() - 1; i >= 0; --i) {
        const auto& s = doc.shapes[i];
        if (!s.visible) continue;
        float x0, y0, x1, y1;
        if (!ShapeBounds(s, x0, y0, x1, y1, true)) continue;
        if (x < x0 - tol || y < y0 - tol || x > x1 + tol || y > y1 + tol) continue;
        std::vector<std::vector<V2>> loops;
        FlattenShape(s, 1.f, loops, true);
        // Fill hit (nonzero winding simplified: point in poly)
        if (s.style.fillEnabled) {
            for (const auto& loop : loops) {
                if (loop.size() < 3) continue;
                bool inside = false;
                for (size_t a = 0, b = loop.size() - 1; a < loop.size(); b = a++) {
                    const V2& A = loop[a], &B = loop[b];
                    if (((A.y > y) != (B.y > y)) &&
                        (x < (B.x - A.x) * (y - A.y) / (B.y - A.y + 1e-12f) + A.x))
                        inside = !inside;
                }
                if (inside) return s.id;
            }
        }
        if (s.style.strokeEnabled) {
            float tw = s.style.strokeWidth * 0.5f + tol;
            for (const auto& loop : loops) {
                for (size_t k = 1; k < loop.size(); ++k) {
                    if (DistPtSeg(x, y, loop[k - 1].x, loop[k - 1].y, loop[k].x, loop[k].y) <= tw)
                        return s.id;
                }
            }
        }
    }
    return 0;
}

bool HitTestNode(const Document& doc, float x, float y, float tol,
                 uint32_t& outShapeId, int& outSub, int& outPt) {
    float best = tol;
    bool hit = false;
    for (int si = (int)doc.shapes.size() - 1; si >= 0; --si) {
        const auto& s = doc.shapes[si];
        if (!s.visible) continue;
        Shape tmp = s;
        if (tmp.kind != ShapeKind::Path) RebuildPathFromParams(tmp);
        for (int sub = 0; sub < (int)tmp.path.subs.size(); ++sub) {
            const auto& sp = tmp.path.subs[sub];
            for (int pi = 0; pi < (int)sp.points.size(); ++pi) {
                V2 p = tmp.xform.Map(sp.points[pi].p);
                float d = std::hypot(p.x - x, p.y - y);
                if (d <= best) {
                    best = d;
                    outShapeId = s.id;
                    outSub = sub;
                    outPt = pi;
                    hit = true;
                }
            }
        }
    }
    return hit;
}

bool HitTestSegment(const Document& doc, float x, float y, float tol,
                    uint32_t& outShapeId, int& outSub, int& outSegStart, float& outT) {
    float best = tol;
    bool hit = false;
    for (int si = (int)doc.shapes.size() - 1; si >= 0; --si) {
        const auto& s = doc.shapes[si];
        if (!s.visible) continue;
        Shape tmp = s;
        if (tmp.kind != ShapeKind::Path) RebuildPathFromParams(tmp);
        for (int sub = 0; sub < (int)tmp.path.subs.size(); ++sub) {
            const auto& sp = tmp.path.subs[sub];
            int n = (int)sp.points.size();
            if (n < 2) continue;
            int segs = sp.closed ? n : n - 1;
            for (int i = 0; i < segs; ++i) {
                V2 a = tmp.xform.Map(sp.points[i].p);
                V2 b = tmp.xform.Map(sp.points[(i + 1) % n].p);
                float dx = b.x - a.x, dy = b.y - a.y;
                float len2 = dx * dx + dy * dy;
                float t = len2 > 1e-12f ? ((x - a.x) * dx + (y - a.y) * dy) / len2 : 0.f;
                t = std::clamp(t, 0.f, 1.f);
                float qx = a.x + t * dx, qy = a.y + t * dy;
                float d = std::hypot(x - qx, y - qy);
                if (d <= best) {
                    best = d;
                    outShapeId = s.id;
                    outSub = sub;
                    outSegStart = i;
                    outT = t;
                    hit = true;
                }
            }
        }
    }
    return hit;
}

int InsertNodeOnSegment(Shape& s, int subIdx, int segStart, float t) {
    ConvertShapeToPath(s);
    if (subIdx < 0 || subIdx >= (int)s.path.subs.size()) return -1;
    auto& sp = s.path.subs[subIdx];
    int n = (int)sp.points.size();
    if (n < 2 || segStart < 0 || segStart >= n) return -1;
    int next = sp.closed ? (segStart + 1) % n : segStart + 1;
    if (next >= n && !sp.closed) return -1;
    t = std::clamp(t, 0.05f, 0.95f); // avoid stacking on endpoints
    PathPoint& A = sp.points[segStart];
    PathPoint& B = sp.points[next];
    // Cubic de Casteljau split for better continuity
    V2 p0 = A.p, p1 = A.OutAbs(), p2 = B.InAbs(), p3 = B.p;
    bool straight = (A.hout.Length() < 1e-4f && B.hin.Length() < 1e-4f);
    PathPoint mid;
    mid.type = NodeType::Smooth;
    if (straight) {
        mid.p = p0 + (p3 - p0) * t;
        mid.hin = {};
        mid.hout = {};
        mid.type = NodeType::Corner;
    } else {
        V2 p01 = p0 + (p1 - p0) * t;
        V2 p12 = p1 + (p2 - p1) * t;
        V2 p23 = p2 + (p3 - p2) * t;
        V2 p012 = p01 + (p12 - p01) * t;
        V2 p123 = p12 + (p23 - p12) * t;
        V2 p0123 = p012 + (p123 - p012) * t;
        mid.p = p0123;
        mid.hin = p012 - mid.p;
        mid.hout = p123 - mid.p;
        // Update neighbor handles
        A.hout = p01 - A.p;
        B.hin = p23 - B.p;
    }
    int insertAt = segStart + 1;
    if (sp.closed && next == 0) {
        // insert at end before wrap
        sp.points.push_back(mid);
        return (int)sp.points.size() - 1;
    }
    sp.points.insert(sp.points.begin() + insertAt, mid);
    return insertAt;
}

void CycleNodeType(Shape& s, int subIdx, int ptIdx) {
    ConvertShapeToPath(s);
    if (subIdx < 0 || subIdx >= (int)s.path.subs.size()) return;
    auto& pts = s.path.subs[subIdx].points;
    if (ptIdx < 0 || ptIdx >= (int)pts.size()) return;
    auto& pt = pts[ptIdx];
    if (pt.type == NodeType::Corner) {
        pt.type = NodeType::Smooth;
        // invent handles if none
        if (pt.hin.Length() < 1e-4f && pt.hout.Length() < 1e-4f) {
            // look at neighbors
            int n = (int)pts.size();
            int prev = (ptIdx - 1 + n) % n;
            int next = (ptIdx + 1) % n;
            V2 dir = pts[next].p - pts[prev].p;
            float L = dir.Length() * 0.2f;
            if (L < 4.f) L = 4.f;
            dir = dir.Normalized() * L;
            pt.hout = dir;
            pt.hin = dir * -1.f;
        } else {
            float L = std::max(pt.hin.Length(), pt.hout.Length());
            V2 d = pt.hout.Length() > 1e-4f ? pt.hout.Normalized() : (pt.hin * -1.f).Normalized();
            pt.hout = d * L;
            pt.hin = d * -L;
        }
    } else if (pt.type == NodeType::Smooth) {
        pt.type = NodeType::Symmetric;
        float L = (pt.hin.Length() + pt.hout.Length()) * 0.5f;
        if (L < 1.f) L = 8.f;
        V2 d = pt.hout.Length() > 1e-4f ? pt.hout.Normalized() : V2(1.f, 0.f);
        pt.hout = d * L;
        pt.hin = d * -L;
    } else {
        pt.type = NodeType::Corner;
        // keep handles (cusp) or zero them
        // leave as independent
    }
}

static int FindShapeIndex(Document& doc, uint32_t id) {
    for (int i = 0; i < (int)doc.shapes.size(); ++i)
        if (doc.shapes[i].id == id) return i;
    return -1;
}

bool BringShapeToFront(Document& doc, uint32_t id) {
    int i = FindShapeIndex(doc, id);
    if (i < 0 || i == (int)doc.shapes.size() - 1) return false;
    Shape s = std::move(doc.shapes[i]);
    doc.shapes.erase(doc.shapes.begin() + i);
    doc.shapes.push_back(std::move(s));
    return true;
}

bool SendShapeToBack(Document& doc, uint32_t id) {
    int i = FindShapeIndex(doc, id);
    if (i <= 0) return false;
    Shape s = std::move(doc.shapes[i]);
    doc.shapes.erase(doc.shapes.begin() + i);
    doc.shapes.insert(doc.shapes.begin(), std::move(s));
    return true;
}

bool RaiseShape(Document& doc, uint32_t id) {
    int i = FindShapeIndex(doc, id);
    if (i < 0 || i >= (int)doc.shapes.size() - 1) return false;
    std::swap(doc.shapes[i], doc.shapes[i + 1]);
    return true;
}

bool LowerShape(Document& doc, uint32_t id) {
    int i = FindShapeIndex(doc, id);
    if (i <= 0) return false;
    std::swap(doc.shapes[i], doc.shapes[i - 1]);
    return true;
}

uint32_t DuplicateShape(Document& doc, uint32_t id) {
    int i = FindShapeIndex(doc, id);
    if (i < 0) return 0;
    Shape s = doc.shapes[i];
    s.id = doc.AllocId();
    s.name += " copy";
    TranslateShape(s, 12.f, 12.f);
    doc.shapes.push_back(std::move(s));
    return doc.shapes.back().id;
}

// Douglas-Peucker simplify
static void DouglasPeucker(const std::vector<V2>& pts, int i0, int i1, float tol,
                           std::vector<char>& keep) {
    if (i1 <= i0 + 1) return;
    float maxD = 0.f;
    int maxI = i0;
    V2 a = pts[i0], b = pts[i1];
    for (int i = i0 + 1; i < i1; ++i) {
        float d = DistPtSeg(pts[i].x, pts[i].y, a.x, a.y, b.x, b.y);
        if (d > maxD) { maxD = d; maxI = i; }
    }
    if (maxD > tol) {
        keep[maxI] = 1;
        DouglasPeucker(pts, i0, maxI, tol, keep);
        DouglasPeucker(pts, maxI, i1, tol, keep);
    }
}

Path PolylineToPath(const std::vector<V2>& samples, float simplifyTol, bool closed) {
    Path path;
    if (samples.size() < 2) return path;
    std::vector<V2> pts = samples;
    // drop near-duplicate consecutive
    std::vector<V2> cleaned;
    cleaned.push_back(pts[0]);
    for (size_t i = 1; i < pts.size(); ++i) {
        if (std::hypot(pts[i].x - cleaned.back().x, pts[i].y - cleaned.back().y) >= 1.f)
            cleaned.push_back(pts[i]);
    }
    if (cleaned.size() < 2) return path;
    std::vector<char> keep(cleaned.size(), 0);
    keep.front() = keep.back() = 1;
    DouglasPeucker(cleaned, 0, (int)cleaned.size() - 1, std::max(0.5f, simplifyTol), keep);
    std::vector<V2> simp;
    for (size_t i = 0; i < cleaned.size(); ++i)
        if (keep[i]) simp.push_back(cleaned[i]);
    if (simp.size() < 2) simp = cleaned;

    SubPath sp;
    sp.closed = closed;
    for (size_t i = 0; i < simp.size(); ++i) {
        PathPoint p;
        p.p = simp[i];
        p.type = NodeType::Smooth;
        // Smooth handles along chord neighbors
        V2 prev = simp[i > 0 ? i - 1 : (closed ? simp.size() - 1 : i)];
        V2 next = simp[i + 1 < simp.size() ? i + 1 : (closed ? 0 : i)];
        V2 dir = next - prev;
        float L = dir.Length() * 0.2f;
        if (L < 1.f) L = 1.f;
        if (dir.Length() > 1e-4f) {
            dir = dir.Normalized() * L;
            p.hout = dir;
            p.hin = dir * -1.f;
        }
        if (i == 0 && !closed) { p.hin = {}; p.type = NodeType::Corner; }
        if (i + 1 == simp.size() && !closed) { p.hout = {}; p.type = NodeType::Corner; }
        sp.points.push_back(p);
    }
    path.subs.push_back(std::move(sp));
    return path;
}

Shape MakeRect(float x, float y, float w, float h, const ShapeStyle& style, uint32_t id) {
    Shape s;
    s.id = id;
    s.kind = ShapeKind::Rect;
    s.style = style;
    s.param[0] = x; s.param[1] = y; s.param[2] = w; s.param[3] = h;
    s.param[4] = s.param[5] = 0.f;
    s.name = "Rect";
    RebuildPathFromParams(s);
    return s;
}

Shape MakeEllipse(float cx, float cy, float rx, float ry, const ShapeStyle& style, uint32_t id) {
    Shape s;
    s.id = id;
    s.kind = ShapeKind::Ellipse;
    s.style = style;
    s.param[0] = cx; s.param[1] = cy; s.param[2] = std::abs(rx); s.param[3] = std::abs(ry);
    s.name = "Ellipse";
    RebuildPathFromParams(s);
    return s;
}

Shape MakeLine(float x1, float y1, float x2, float y2, const ShapeStyle& style, uint32_t id) {
    Shape s;
    s.id = id;
    s.kind = ShapeKind::Line;
    s.style = style;
    s.style.fillEnabled = false; // lines are stroke-only by default
    s.param[0] = x1; s.param[1] = y1; s.param[2] = x2; s.param[3] = y2;
    s.name = "Line";
    RebuildPathFromParams(s);
    return s;
}

Shape MakeEmptyPath(const ShapeStyle& style, uint32_t id) {
    Shape s;
    s.id = id;
    s.kind = ShapeKind::Path;
    s.style = style;
    s.name = "Path";
    return s;
}

void TranslateShape(Shape& s, float dx, float dy) {
    s.xform.e += dx;
    s.xform.f += dy;
    if (s.kind == ShapeKind::Rect || s.kind == ShapeKind::Ellipse || s.kind == ShapeKind::Line) {
        s.param[0] += dx;
        s.param[1] += dy;
        if (s.kind == ShapeKind::Line) {
            s.param[2] += dx;
            s.param[3] += dy;
        }
        RebuildPathFromParams(s);
        s.xform = Affine::Identity();
    }
}

bool ShapeLocalBounds(const Shape& s, float& x0, float& y0, float& x1, float& y1) {
    return ShapeBounds(s, x0, y0, x1, y1, /*includeStroke=*/false);
}

void SetShapeTopLeft(Shape& s, float x, float y) {
    float x0, y0, x1, y1;
    if (!ShapeLocalBounds(s, x0, y0, x1, y1)) return;
    TranslateShape(s, x - x0, y - y0);
}

void ScaleShapeAboutCenter(Shape& s, float sx, float sy, bool scaleStyles) {
    if (std::abs(sx) < 1e-6f) sx = (sx < 0 ? -1e-6f : 1e-6f);
    if (std::abs(sy) < 1e-6f) sy = (sy < 0 ? -1e-6f : 1e-6f);
    float x0, y0, x1, y1;
    if (!ShapeLocalBounds(s, x0, y0, x1, y1)) return;
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;

    if (s.kind == ShapeKind::Rect) {
        float w = s.param[2] * sx, h = s.param[3] * sy;
        s.param[0] = cx - w * 0.5f;
        s.param[1] = cy - h * 0.5f;
        s.param[2] = std::abs(w);
        s.param[3] = std::abs(h);
        s.param[4] *= std::abs(sx); // rx
        s.param[5] *= std::abs(sy); // ry
        RebuildPathFromParams(s);
        s.xform = Affine::Identity();
    } else if (s.kind == ShapeKind::Ellipse) {
        s.param[2] = std::abs(s.param[2] * sx);
        s.param[3] = std::abs(s.param[3] * sy);
        s.param[0] = cx; // keep center
        s.param[1] = cy;
        RebuildPathFromParams(s);
        s.xform = Affine::Identity();
    } else if (s.kind == ShapeKind::Line) {
        auto mapP = [&](float& px, float& py) {
            px = cx + (px - cx) * sx;
            py = cy + (py - cy) * sy;
        };
        mapP(s.param[0], s.param[1]);
        mapP(s.param[2], s.param[3]);
        RebuildPathFromParams(s);
        s.xform = Affine::Identity();
    } else {
        ConvertShapeToPath(s);
        BakeTransformIntoPath(s);
        for (auto& sub : s.path.subs) {
            for (auto& pt : sub.points) {
                pt.p.x = cx + (pt.p.x - cx) * sx;
                pt.p.y = cy + (pt.p.y - cy) * sy;
                pt.hin.x *= sx; pt.hin.y *= sy;
                pt.hout.x *= sx; pt.hout.y *= sy;
            }
        }
    }
    if (scaleStyles && s.style.strokeEnabled) {
        float avg = (std::abs(sx) + std::abs(sy)) * 0.5f;
        s.style.strokeWidth = std::max(0.25f, s.style.strokeWidth * avg);
    }
}

bool SetShapeSize(Shape& s, float newW, float newH, bool aboutCenter, bool scaleStyles) {
    float x0, y0, x1, y1;
    if (!ShapeLocalBounds(s, x0, y0, x1, y1)) return false;
    float ow = x1 - x0, oh = y1 - y0;
    if (ow < 1e-4f || oh < 1e-4f) return false;
    if (newW < 0.5f) newW = 0.5f;
    if (newH < 0.5f) newH = 0.5f;
    float sx = newW / ow, sy = newH / oh;
    if (aboutCenter) {
        ScaleShapeAboutCenter(s, sx, sy, scaleStyles);
    } else {
        // scale about top-left
        if (s.kind == ShapeKind::Rect) {
            s.param[2] = newW;
            s.param[3] = newH;
            RebuildPathFromParams(s);
            s.xform = Affine::Identity();
            if (scaleStyles)
                s.style.strokeWidth = std::max(0.25f, s.style.strokeWidth * (sx + sy) * 0.5f);
        } else {
            ScaleShapeAboutCenter(s, sx, sy, scaleStyles);
            SetShapeTopLeft(s, x0, y0);
        }
    }
    return true;
}

Shape BreakPathAtNode(Shape& s, int subIdx, int ptIdx, uint32_t newId) {
    ConvertShapeToPath(s);
    Shape other = MakeEmptyPath(s.style, newId);
    other.name = s.name + " b";
    if (subIdx < 0 || subIdx >= (int)s.path.subs.size()) return other;
    auto& sp = s.path.subs[subIdx];
    int n = (int)sp.points.size();
    if (n < 3 || ptIdx <= 0 || ptIdx >= n - 1) return other; // need middle node
    // second shape gets [ptIdx .. end]
    SubPath b;
    b.closed = false;
    for (int i = ptIdx; i < n; ++i)
        b.points.push_back(sp.points[i]);
    // first shape keeps [0 .. ptIdx]
    SubPath a;
    a.closed = false;
    for (int i = 0; i <= ptIdx; ++i)
        a.points.push_back(sp.points[i]);
    // open original
    sp = std::move(a);
    sp.closed = false;
    s.kind = ShapeKind::Path;
    other.path.subs.push_back(std::move(b));
    return other;
}

void ApplyStyleToShape(Shape& s, const ShapeStyle& style) {
    s.style = style;
}

static void GetOpenEndpoints(const Shape& s, V2& start, V2& end, bool& ok) {
    ok = false;
    Shape tmp = s;
    if (tmp.kind != ShapeKind::Path) RebuildPathFromParams(tmp);
    BakeTransformIntoPath(tmp);
    // first open subpath
    for (const auto& sp : tmp.path.subs) {
        if (sp.closed || sp.points.size() < 2) continue;
        start = sp.points.front().p;
        end = sp.points.back().p;
        ok = true;
        return;
    }
}

bool JoinOpenPaths(Shape& a, Shape& b, float tol) {
    ConvertShapeToPath(a);
    ConvertShapeToPath(b);
    BakeTransformIntoPath(a);
    BakeTransformIntoPath(b);
    // Find open subpaths
    int ai = -1, bi = -1;
    for (int i = 0; i < (int)a.path.subs.size(); ++i)
        if (!a.path.subs[i].closed && a.path.subs[i].points.size() >= 2) { ai = i; break; }
    for (int i = 0; i < (int)b.path.subs.size(); ++i)
        if (!b.path.subs[i].closed && b.path.subs[i].points.size() >= 2) { bi = i; break; }
    if (ai < 0 || bi < 0) return false;
    auto& A = a.path.subs[ai];
    auto& B = b.path.subs[bi];
    V2 a0 = A.points.front().p, a1 = A.points.back().p;
    V2 b0 = B.points.front().p, b1 = B.points.back().p;
    float t2 = tol * tol;
    auto dist2 = [](V2 u, V2 v) {
        float dx = u.x - v.x, dy = u.y - v.y; return dx * dx + dy * dy;
    };
    // 4 cases: a1-b0, a1-b1, a0-b0, a0-b1
    enum { A1B0, A1B1, A0B0, A0B1 } best = A1B0;
    float bd = dist2(a1, b0);
    float d;
    d = dist2(a1, b1); if (d < bd) { bd = d; best = A1B1; }
    d = dist2(a0, b0); if (d < bd) { bd = d; best = A0B0; }
    d = dist2(a0, b1); if (d < bd) { bd = d; best = A0B1; }
    if (bd > t2) return false;

    std::vector<PathPoint> merged;
    auto rev = [](std::vector<PathPoint> pts) {
        std::reverse(pts.begin(), pts.end());
        for (auto& p : pts) std::swap(p.hin, p.hout);
        return pts;
    };
    auto appendSkipFirst = [](std::vector<PathPoint>& out, const std::vector<PathPoint>& in) {
        for (size_t i = 1; i < in.size(); ++i) out.push_back(in[i]);
    };

    switch (best) {
    case A1B0:
        merged = A.points;
        appendSkipFirst(merged, B.points);
        break;
    case A1B1: {
        merged = A.points;
        auto br = rev(B.points);
        appendSkipFirst(merged, br);
        break;
    }
    case A0B0: {
        merged = rev(A.points);
        appendSkipFirst(merged, B.points);
        break;
    }
    case A0B1: {
        merged = rev(A.points);
        auto br = rev(B.points);
        appendSkipFirst(merged, br);
        break;
    }
    }
    A.points = std::move(merged);
    A.closed = false;
    a.kind = ShapeKind::Path;
    // drop other open sub from a if only one; append remaining closed subs from b
    for (int i = 0; i < (int)b.path.subs.size(); ++i) {
        if (i == bi) continue;
        a.path.subs.push_back(b.path.subs[i]);
    }
    return true;
}

bool AlignShapes(Document& doc, const std::vector<uint32_t>& ids, int mode) {
    if (ids.size() < 2) return false;
    struct Item { uint32_t id; float x0, y0, x1, y1; };
    std::vector<Item> items;
    float ux0 = 1e30f, uy0 = 1e30f, ux1 = -1e30f, uy1 = -1e30f;
    for (uint32_t id : ids) {
        Shape* s = doc.Find(id);
        if (!s) continue;
        float x0, y0, x1, y1;
        if (!ShapeLocalBounds(*s, x0, y0, x1, y1)) continue;
        items.push_back({id, x0, y0, x1, y1});
        ux0 = std::min(ux0, x0); uy0 = std::min(uy0, y0);
        ux1 = std::max(ux1, x1); uy1 = std::max(uy1, y1);
    }
    if (items.size() < 2) return false;
    float ucx = (ux0 + ux1) * 0.5f, ucy = (uy0 + uy1) * 0.5f;
    for (auto& it : items) {
        Shape* s = doc.Find(it.id);
        if (!s) continue;
        float cx = (it.x0 + it.x1) * 0.5f, cy = (it.y0 + it.y1) * 0.5f;
        float dx = 0, dy = 0;
        switch (mode) {
        case 0: dx = ux0 - it.x0; break;                 // left
        case 1: dx = ucx - cx; break;                    // hcenter
        case 2: dx = ux1 - it.x1; break;                 // right
        case 3: dy = uy0 - it.y0; break;                 // top
        case 4: dy = ucy - cy; break;                    // vcenter
        case 5: dy = uy1 - it.y1; break;                 // bottom
        default: break;
        }
        if (dx != 0.f || dy != 0.f)
            TranslateShape(*s, dx, dy);
    }
    return true;
}

// ---- JSON ----
using json = nlohmann::json;

static json PointToJ(const PathPoint& p) {
    return json{
        {"x", p.p.x}, {"y", p.p.y},
        {"ix", p.hin.x}, {"iy", p.hin.y},
        {"ox", p.hout.x}, {"oy", p.hout.y},
        {"t", (int)p.type}
    };
}

static PathPoint PointFromJ(const json& j) {
    PathPoint p;
    p.p.x = j.value("x", 0.f); p.p.y = j.value("y", 0.f);
    p.hin.x = j.value("ix", 0.f); p.hin.y = j.value("iy", 0.f);
    p.hout.x = j.value("ox", 0.f); p.hout.y = j.value("oy", 0.f);
    p.type = (NodeType)j.value("t", 0);
    return p;
}

std::string DocumentToJson(const Document& doc) {
    json root;
    root["aa"] = doc.antialias;
    root["next"] = doc.nextId;
    root["gen"] = doc.generation;
    if (!doc.sourceSvg.empty())
        root["src"] = doc.sourceSvg;
    json arr = json::array();
    for (const auto& s : doc.shapes) {
        json js;
        js["id"] = s.id;
        js["name"] = s.name;
        js["kind"] = (int)s.kind;
        js["vis"] = s.visible;
        js["xf"] = {s.xform.a, s.xform.b, s.xform.c, s.xform.d, s.xform.e, s.xform.f};
        js["param"] = std::vector<float>(s.param, s.param + 8);
        js["style"] = {
            {"fe", s.style.fillEnabled},
            {"fr", s.style.fillRgba[0]}, {"fg", s.style.fillRgba[1]},
            {"fb", s.style.fillRgba[2]}, {"fa", s.style.fillRgba[3]},
            {"se", s.style.strokeEnabled},
            {"sr", s.style.strokeRgba[0]}, {"sg", s.style.strokeRgba[1]},
            {"sb", s.style.strokeRgba[2]}, {"sa", s.style.strokeRgba[3]},
            {"sw", s.style.strokeWidth},
            {"rule", s.style.fillRule},
            {"fp", (int)s.style.fillPaint},
            {"g0r", s.style.gradRgba0[0]}, {"g0g", s.style.gradRgba0[1]},
            {"g0b", s.style.gradRgba0[2]}, {"g0a", s.style.gradRgba0[3]},
            {"g1r", s.style.gradRgba1[0]}, {"g1g", s.style.gradRgba1[1]},
            {"g1b", s.style.gradRgba1[2]}, {"g1a", s.style.gradRgba1[3]},
            {"gx0", s.style.gradX0}, {"gy0", s.style.gradY0},
            {"gx1", s.style.gradX1}, {"gy1", s.style.gradY1},
            {"gub", s.style.gradUseShapeBounds},
            {"dash", s.style.dashLen}, {"gap", s.style.gapLen}
        };
        json subs = json::array();
        for (const auto& sp : s.path.subs) {
            json jsp;
            jsp["c"] = sp.closed;
            json pts = json::array();
            for (const auto& pt : sp.points)
                pts.push_back(PointToJ(pt));
            jsp["pts"] = pts;
            subs.push_back(jsp);
        }
        js["path"] = subs;
        arr.push_back(js);
    }
    root["shapes"] = arr;
    return root.dump();
}

bool DocumentFromJson(const std::string& str, Document& out) {
    try {
        json root = json::parse(str);
        out = Document{};
        out.antialias = root.value("aa", true);
        out.nextId = root.value("next", 1u);
        out.generation = root.value("gen", (uint64_t)0);
        if (root.contains("src") && root["src"].is_string())
            out.sourceSvg = root["src"].get<std::string>();
        if (!root.contains("shapes")) return true;
        for (const auto& js : root["shapes"]) {
            Shape s;
            s.id = js.value("id", 0u);
            s.name = js.value("name", std::string("Shape"));
            s.kind = (ShapeKind)js.value("kind", 0);
            s.visible = js.value("vis", true);
            if (js.contains("xf") && js["xf"].is_array() && js["xf"].size() >= 6) {
                s.xform.a = js["xf"][0]; s.xform.b = js["xf"][1];
                s.xform.c = js["xf"][2]; s.xform.d = js["xf"][3];
                s.xform.e = js["xf"][4]; s.xform.f = js["xf"][5];
            }
            if (js.contains("param") && js["param"].is_array()) {
                int i = 0;
                for (auto& v : js["param"]) {
                    if (i < 8) s.param[i++] = v.get<float>();
                }
            }
            if (js.contains("style")) {
                auto& st = js["style"];
                s.style.fillEnabled = st.value("fe", true);
                s.style.fillRgba[0] = st.value("fr", 0.2f);
                s.style.fillRgba[1] = st.value("fg", 0.45f);
                s.style.fillRgba[2] = st.value("fb", 0.95f);
                s.style.fillRgba[3] = st.value("fa", 1.f);
                s.style.strokeEnabled = st.value("se", true);
                s.style.strokeRgba[0] = st.value("sr", 0.05f);
                s.style.strokeRgba[1] = st.value("sg", 0.05f);
                s.style.strokeRgba[2] = st.value("sb", 0.08f);
                s.style.strokeRgba[3] = st.value("sa", 1.f);
                s.style.strokeWidth = st.value("sw", 2.f);
                s.style.fillRule = (uint8_t)st.value("rule", 0);
                s.style.fillPaint = (FillPaint)st.value("fp", 0);
                s.style.gradRgba0[0] = st.value("g0r", s.style.fillRgba[0]);
                s.style.gradRgba0[1] = st.value("g0g", s.style.fillRgba[1]);
                s.style.gradRgba0[2] = st.value("g0b", s.style.fillRgba[2]);
                s.style.gradRgba0[3] = st.value("g0a", s.style.fillRgba[3]);
                s.style.gradRgba1[0] = st.value("g1r", 0.95f);
                s.style.gradRgba1[1] = st.value("g1g", 0.35f);
                s.style.gradRgba1[2] = st.value("g1b", 0.55f);
                s.style.gradRgba1[3] = st.value("g1a", 1.f);
                s.style.gradX0 = st.value("gx0", 0.f);
                s.style.gradY0 = st.value("gy0", 0.f);
                s.style.gradX1 = st.value("gx1", 0.f);
                s.style.gradY1 = st.value("gy1", 0.f);
                s.style.gradUseShapeBounds = st.value("gub", true);
                s.style.dashLen = st.value("dash", 0.f);
                s.style.gapLen = st.value("gap", 0.f);
            }
            if (js.contains("path") && js["path"].is_array()) {
                for (const auto& jsp : js["path"]) {
                    SubPath sp;
                    sp.closed = jsp.value("c", false);
                    if (jsp.contains("pts")) {
                        for (const auto& jp : jsp["pts"])
                            sp.points.push_back(PointFromJ(jp));
                    }
                    s.path.subs.push_back(std::move(sp));
                }
            }
            if (s.kind != ShapeKind::Path && s.path.subs.empty())
                RebuildPathFromParams(s);
            out.shapes.push_back(std::move(s));
            if (s.id >= out.nextId) out.nextId = s.id + 1;
        }
        out.MarkAllDirty(1, 1); // caller expands to doc size
        return true;
    } catch (...) {
        return false;
    }
}

std::string ShapeToJson(const Shape& s) {
    // Reuse document serializer with a single-shape document
    Document d;
    d.shapes.push_back(s);
    d.nextId = s.id + 1;
    try {
        json root = json::parse(DocumentToJson(d));
        if (root.contains("shapes") && root["shapes"].is_array() && !root["shapes"].empty())
            return root["shapes"][0].dump();
    } catch (...) {}
    return "{}";
}

bool ShapeFromJson(const std::string& str, Shape& out) {
    try {
        // Wrap as document
        json one = json::parse(str);
        json root;
        root["shapes"] = json::array({one});
        root["next"] = one.value("id", 1u) + 1;
        Document d;
        if (!DocumentFromJson(root.dump(), d) || d.shapes.empty())
            return false;
        out = std::move(d.shapes[0]);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace vec
