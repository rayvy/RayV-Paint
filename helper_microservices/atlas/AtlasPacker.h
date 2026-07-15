#pragma once
// Simple shelf bin-packer for texture atlases (no external libs).

#include "PngIo.h"
#include <string>
#include <vector>

namespace helpers {

struct AtlasSprite {
    std::string path;
    std::string name; // stem for JSON
    int width = 0;
    int height = 0;
    int x = 0; // packed origin
    int y = 0;
    RgbaImage pixels; // optional: filled when loaded
};

struct AtlasOptions {
    int padding = 2;
    int maxSize = 4096;
    bool powerOfTwo = true;
    // If true, fail when sprites don't fit; if false, grow until maxSize then fail.
    bool strict = true;
};

struct AtlasResult {
    bool ok = false;
    std::string message;
    int atlasW = 0;
    int atlasH = 0;
    RgbaImage image;
    std::vector<AtlasSprite> sprites; // with x,y set
};

// Load PNGs into sprites (fills pixels + size).
bool LoadSprites(const std::vector<std::string>& paths, std::vector<AtlasSprite>& out,
                 std::string* err = nullptr);

// Pack loaded sprites (uses width/height; may sort by height).
AtlasResult PackAtlas(std::vector<AtlasSprite> sprites, const AtlasOptions& opt);

// Blit packed sprites into atlas image (requires pixels in sprites).
bool RasterizeAtlas(AtlasResult& atlas);

// Write simple JSON sidecar (UV rects in pixels).
bool SaveAtlasJson(const std::string& utf8Path, const AtlasResult& atlas, std::string* err = nullptr);

} // namespace helpers
