#pragma once
#include "PathTypes.h"
#include "../core/TileCache.h"

namespace vec {

// Rasterize dirty region of vector document into layer tile cache.
// Clears dirty rect area first (transparent), then paints shapes bottom→top.
// Never allocates full document buffers — only dirty AABB (+ pad).
// coarse=true: cheaper flatten while interacting.
// Returns true if any pixels written.
bool RasterizeDocument(const Document& doc, TileCache& tiles, int docW, int docH,
                       bool coarse = false);

// Full re-raster (marks entire doc as working region).
bool RasterizeDocumentFull(const Document& doc, TileCache& tiles, int docW, int docH,
                           bool coarse = false);

} // namespace vec
