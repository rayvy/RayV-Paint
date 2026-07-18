#include "SvgIo.h"
#include "PathMath.h"
#include "../core/PathUtil.h"
#include <fstream>
#include <sstream>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <algorithm>

// nanosvg — single header, define implementation in this TU only
#define NANOSVG_IMPLEMENTATION
#define NANOSVG_ALL_COLOR_KEYWORDS
#include "../../third_party/nanosvg/nanosvg.h"

namespace vec {
namespace {

ShapeStyle StyleFromNs(NSVGshape* sh) {
    ShapeStyle st;
    st.fillEnabled = (sh->fill.type != NSVG_PAINT_NONE);
    st.strokeEnabled = (sh->stroke.type != NSVG_PAINT_NONE) && sh->strokeWidth > 0.f;
    st.strokeWidth = sh->strokeWidth > 0.f ? sh->strokeWidth : 1.f;
    auto unpack = [](unsigned int c, float out[4]) {
        out[0] = ((c >> 0) & 255) / 255.f;
        out[1] = ((c >> 8) & 255) / 255.f;
        out[2] = ((c >> 16) & 255) / 255.f;
        out[3] = ((c >> 24) & 255) / 255.f;
        if (out[3] <= 0.f) out[3] = 1.f;
    };
    if (sh->fill.type == NSVG_PAINT_COLOR)
        unpack(sh->fill.color, st.fillRgba);
    else if (!st.fillEnabled) {
        st.fillRgba[0] = st.fillRgba[1] = st.fillRgba[2] = 0.f;
        st.fillRgba[3] = 0.f;
    }
    st.fillRgba[3] *= sh->opacity;
    if (sh->stroke.type == NSVG_PAINT_COLOR)
        unpack(sh->stroke.color, st.strokeRgba);
    st.strokeRgba[3] *= sh->opacity;
    st.fillRule = (sh->fillRule == NSVG_FILLRULE_EVENODD) ? 1 : 0;
    return st;
}

void ImportNsShape(NSVGshape* sh, Document& doc) {
    if (!(sh->flags & NSVG_FLAGS_VISIBLE)) return;
    Shape shape;
    shape.id = doc.AllocId();
    shape.name = sh->id[0] ? sh->id : "svg";
    shape.kind = ShapeKind::Path;
    shape.style = StyleFromNs(sh);
    shape.visible = true;

    // nanosvg path pts: cubic chain p0,c1,c2,p1,c1,c2,p2,... (npts = 3*nseg+1)
    for (NSVGpath* p = sh->paths; p; p = p->next) {
        SubPath sp;
        sp.closed = p->closed != 0;
        const int npts = p->npts;
        if (npts < 1) continue;
        for (int i = 0; i + 3 < npts; i += 3) {
            float x0 = p->pts[i * 2], y0 = p->pts[i * 2 + 1];
            float x1 = p->pts[(i + 1) * 2], y1 = p->pts[(i + 1) * 2 + 1];
            float x2 = p->pts[(i + 2) * 2], y2 = p->pts[(i + 2) * 2 + 1];
            float x3 = p->pts[(i + 3) * 2], y3 = p->pts[(i + 3) * 2 + 1];
            if (sp.points.empty()) {
                PathPoint a;
                a.p = {x0, y0};
                a.hout = {x1 - x0, y1 - y0};
                a.type = NodeType::Corner;
                sp.points.push_back(a);
            } else {
                sp.points.back().hout = {x1 - sp.points.back().p.x, y1 - sp.points.back().p.y};
            }
            PathPoint b;
            b.p = {x3, y3};
            b.hin = {x2 - x3, y2 - y3};
            b.type = NodeType::Corner;
            sp.points.push_back(b);
        }
        if (sp.points.empty() && npts >= 1) {
            PathPoint a;
            a.p = {p->pts[0], p->pts[1]};
            sp.points.push_back(a);
        }
        if (!sp.points.empty())
            shape.path.subs.push_back(std::move(sp));
    }
    if (!shape.path.subs.empty())
        doc.shapes.push_back(std::move(shape));
}

bool LoadFromNsImage(NSVGimage* image, Document& out, const std::string& sourceCopy) {
    if (!image) return false;
    out = Document{};
    out.sourceSvg = sourceCopy;
    out.antialias = true;
    for (NSVGshape* sh = image->shapes; sh; sh = sh->next)
        ImportNsShape(sh, out);
    nsvgDelete(image);
    float x0, y0, x1, y1;
    if (DocumentBounds(out, x0, y0, x1, y1))
        out.MarkDirty((int)std::floor(x0), (int)std::floor(y0),
                      (int)std::ceil(x1), (int)std::ceil(y1));
    else
        out.MarkAllDirty(1, 1);
    return !out.shapes.empty() || true; // empty SVG still ok
}

} // namespace

bool LoadSvgMemory(const char* data, size_t len, Document& out, std::string* err) {
    if (!data || !len) {
        if (err) *err = "empty SVG";
        return false;
    }
    std::string copy(data, data + len);
    // nsvgParse mutates? takes non-const char* — needs mutable buffer
    std::vector<char> buf(copy.begin(), copy.end());
    buf.push_back('\0');
    NSVGimage* image = nsvgParse(buf.data(), "px", 96.f);
    if (!image) {
        if (err) *err = "nanosvg parse failed";
        return false;
    }
    return LoadFromNsImage(image, out, copy);
}

bool LoadSvgFile(const std::string& path, Document& out, std::string* err) {
#ifdef _WIN32
    std::ifstream in(PathUtil::Utf8ToWide(path), std::ios::binary);
#else
    std::ifstream in(path, std::ios::binary);
#endif
    if (!in) {
        if (err) *err = "cannot open: " + path;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    return LoadSvgMemory(s.data(), s.size(), out, err);
}

std::string SaveSvgString(const Document& doc, int width, int height) {
    std::ostringstream o;
    o << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    o << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << width
      << "\" height=\"" << height << "\" viewBox=\"0 0 " << width << " " << height << "\">\n";
    auto rgba = [](const float c[4]) {
        char buf[64];
        int r = (int)std::clamp(c[0] * 255.f, 0.f, 255.f);
        int g = (int)std::clamp(c[1] * 255.f, 0.f, 255.f);
        int b = (int)std::clamp(c[2] * 255.f, 0.f, 255.f);
        std::snprintf(buf, sizeof(buf), "rgb(%d,%d,%d)", r, g, b);
        return std::string(buf);
    };
    for (const auto& s : doc.shapes) {
        if (!s.visible) continue;
        Shape tmp = s;
        if (tmp.kind != ShapeKind::Path)
            RebuildPathFromParams(tmp);
        std::ostringstream d;
        for (const auto& sp : tmp.path.subs) {
            if (sp.points.empty()) continue;
            for (size_t i = 0; i < sp.points.size(); ++i) {
                V2 p = tmp.xform.Map(sp.points[i].p);
                if (i == 0) d << "M " << p.x << " " << p.y << " ";
                else {
                    V2 c1 = tmp.xform.Map(sp.points[i - 1].OutAbs());
                    V2 c2 = tmp.xform.Map(sp.points[i].InAbs());
                    bool straight = (sp.points[i - 1].hout.Length() < 1e-4f &&
                                     sp.points[i].hin.Length() < 1e-4f);
                    if (straight)
                        d << "L " << p.x << " " << p.y << " ";
                    else
                        d << "C " << c1.x << " " << c1.y << " " << c2.x << " " << c2.y
                          << " " << p.x << " " << p.y << " ";
                }
            }
            if (sp.closed) d << "Z ";
        }
        o << "  <path d=\"" << d.str() << "\" fill=\"";
        if (tmp.style.fillEnabled)
            o << rgba(tmp.style.fillRgba) << "\" fill-opacity=\"" << tmp.style.fillRgba[3];
        else
            o << "none";
        o << "\" stroke=\"";
        if (tmp.style.strokeEnabled)
            o << rgba(tmp.style.strokeRgba) << "\" stroke-opacity=\"" << tmp.style.strokeRgba[3]
              << "\" stroke-width=\"" << tmp.style.strokeWidth;
        else
            o << "none";
        o << "\"/>\n";
    }
    o << "</svg>\n";
    return o.str();
}

bool SaveSvgFile(const std::string& path, const Document& doc, int width, int height,
                 std::string* err) {
    std::string s = SaveSvgString(doc, width, height);
#ifdef _WIN32
    std::ofstream out(PathUtil::Utf8ToWide(path), std::ios::binary);
#else
    std::ofstream out(path, std::ios::binary);
#endif
    if (!out) {
        if (err) *err = "cannot write: " + path;
        return false;
    }
    out.write(s.data(), (std::streamsize)s.size());
    return true;
}

} // namespace vec
