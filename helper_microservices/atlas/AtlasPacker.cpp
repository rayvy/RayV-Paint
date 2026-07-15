#include "AtlasPacker.h"
#include "../common/Utf8.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace helpers {
namespace {

int NextPow2(int v) {
    if (v <= 1) return 1;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

struct Shelf {
    int y = 0;
    int height = 0;
    int x = 0; // next free x
};

// Try pack into fixed width; returns height used or -1 on failure.
int TryShelfPack(std::vector<AtlasSprite>& sprites, int atlasW, int padding, int maxH) {
    std::vector<Shelf> shelves;
    int usedH = 0;

    for (auto& s : sprites) {
        const int sw = s.width + padding;
        const int sh = s.height + padding;
        if (sw > atlasW) return -1;

        int best = -1;
        for (int i = 0; i < (int)shelves.size(); ++i) {
            if (shelves[i].height >= sh && shelves[i].x + sw <= atlasW) {
                best = i;
                break;
            }
        }

        if (best < 0) {
            // new shelf
            int y = usedH;
            if (y + sh > maxH) return -1;
            Shelf shf;
            shf.y = y;
            shf.height = sh;
            shf.x = 0;
            shelves.push_back(shf);
            best = (int)shelves.size() - 1;
            usedH = y + sh;
        }

        Shelf& shf = shelves[best];
        s.x = shf.x;
        s.y = shf.y;
        shf.x += sw;
        if (shf.height < sh) {
            // shouldn't happen if we only place into taller/equal shelves
        }
    }
    return usedH;
}

std::string JsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\\' || c == '"') { o.push_back('\\'); o.push_back(c); }
        else if (c == '\n') o += "\\n";
        else o.push_back(c);
    }
    return o;
}

} // namespace

bool LoadSprites(const std::vector<std::string>& paths, std::vector<AtlasSprite>& out,
                 std::string* err) {
    out.clear();
    out.reserve(paths.size());
    for (const auto& p : paths) {
        AtlasSprite sp;
        sp.path = p;
        std::filesystem::path fp(Utf8ToWide(p));
        sp.name = WideToUtf8(fp.stem().wstring());
        std::string e;
        if (!LoadPng(p, sp.pixels, &e)) {
            if (err) *err = "Failed to load " + p + ": " + e;
            return false;
        }
        sp.width = sp.pixels.width;
        sp.height = sp.pixels.height;
        out.push_back(std::move(sp));
    }
    return true;
}

AtlasResult PackAtlas(std::vector<AtlasSprite> sprites, const AtlasOptions& opt) {
    AtlasResult r;
    if (sprites.empty()) {
        r.message = "No sprites";
        return r;
    }

    // Sort tallest first — better shelf packing
    std::sort(sprites.begin(), sprites.end(),
              [](const AtlasSprite& a, const AtlasSprite& b) {
                  if (a.height != b.height) return a.height > b.height;
                  return a.width > b.width;
              });

    int maxDim = std::max(1, opt.maxSize);
    int pad = std::max(0, opt.padding);

    // Minimum width: widest sprite + padding
    int minW = 0;
    long long area = 0;
    for (const auto& s : sprites) {
        minW = std::max(minW, s.width + pad);
        area += (long long)(s.width + pad) * (s.height + pad);
    }
    if (minW > maxDim) {
        r.message = "Sprite wider than max atlas size";
        return r;
    }

    int startW = (int)std::ceil(std::sqrt((double)area));
    startW = std::max(startW, minW);
    if (opt.powerOfTwo) startW = NextPow2(startW);
    startW = std::min(startW, maxDim);

    int bestW = 0, bestH = 0;
    std::vector<AtlasSprite> bestSprites;
    bool found = false;

    for (int w = startW; w <= maxDim; ) {
        auto trial = sprites;
        int h = TryShelfPack(trial, w, pad, maxDim);
        if (h > 0) {
            int outW = w;
            int outH = h;
            if (opt.powerOfTwo) {
                outW = NextPow2(outW);
                outH = NextPow2(outH);
            }
            if (outW <= maxDim && outH <= maxDim) {
                found = true;
                bestW = outW;
                bestH = outH;
                bestSprites = std::move(trial);
                break;
            }
        }

        if (w >= maxDim)
            break;
        if (opt.powerOfTwo) {
            int next = NextPow2(w + 1);
            w = (next > w && next <= maxDim) ? next : maxDim;
        } else {
            w = std::min(w + 64, maxDim);
        }
    }

    if (!found) {
        r.message = "Could not pack into max size " + std::to_string(maxDim);
        return r;
    }

    r.ok = true;
    r.atlasW = bestW;
    r.atlasH = bestH;
    r.sprites = std::move(bestSprites);
    r.message = "Packed " + std::to_string(r.sprites.size()) + " sprites into " +
                std::to_string(r.atlasW) + "x" + std::to_string(r.atlasH);
    return r;
}

bool RasterizeAtlas(AtlasResult& atlas) {
    if (!atlas.ok || atlas.atlasW <= 0 || atlas.atlasH <= 0) return false;
    atlas.image.width = atlas.atlasW;
    atlas.image.height = atlas.atlasH;
    atlas.image.rgba.assign((size_t)atlas.atlasW * atlas.atlasH * 4, 0);

    for (const auto& s : atlas.sprites) {
        if (s.pixels.rgba.empty()) continue;
        for (int y = 0; y < s.height; ++y) {
            int dy = s.y + y;
            if (dy < 0 || dy >= atlas.atlasH) continue;
            for (int x = 0; x < s.width; ++x) {
                int dx = s.x + x;
                if (dx < 0 || dx >= atlas.atlasW) continue;
                size_t si = ((size_t)y * s.width + x) * 4;
                size_t di = ((size_t)dy * atlas.atlasW + dx) * 4;
                atlas.image.rgba[di + 0] = s.pixels.rgba[si + 0];
                atlas.image.rgba[di + 1] = s.pixels.rgba[si + 1];
                atlas.image.rgba[di + 2] = s.pixels.rgba[si + 2];
                atlas.image.rgba[di + 3] = s.pixels.rgba[si + 3];
            }
        }
    }
    return true;
}

bool SaveAtlasJson(const std::string& utf8Path, const AtlasResult& atlas, std::string* err) {
    std::ofstream out(std::filesystem::path(Utf8ToWide(utf8Path)), std::ios::trunc);
    if (!out) {
        if (err) *err = "Cannot write JSON";
        return false;
    }
    out << "{\n";
    out << "  \"atlasWidth\": " << atlas.atlasW << ",\n";
    out << "  \"atlasHeight\": " << atlas.atlasH << ",\n";
    out << "  \"sprites\": [\n";
    for (size_t i = 0; i < atlas.sprites.size(); ++i) {
        const auto& s = atlas.sprites[i];
        out << "    {\"name\": \"" << JsonEscape(s.name) << "\", "
            << "\"x\": " << s.x << ", \"y\": " << s.y << ", "
            << "\"w\": " << s.width << ", \"h\": " << s.height << "}";
        if (i + 1 < atlas.sprites.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n}\n";
    return true;
}

} // namespace helpers
