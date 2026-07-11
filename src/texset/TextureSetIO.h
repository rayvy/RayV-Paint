#pragma once
// Import / export helpers for TextureSet map slots (no D3D).

#include "TextureSet.h"
#include "../core/TileCache.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace texset {

// Load image/DDS into a map slot composite. Enables the slot, sets size/sourcePath.
// Does not touch the paint Canvas (Diffuse canvas is managed separately).
// soloRole != None: extract that role's packed channel as grayscale RGB (for single-channel import).
bool ImportMapFromFile(TextureSet& set, MapKind kind, const std::string& filepath,
                       ChannelRole soloRole = ChannelRole::None);

// Export map composite (or provided cache) to PNG/TGA/BMP via ImageManager.
// Returns false if no pixels / write failed.
bool ExportMapToFile(const TextureSet& set, MapKind kind, const std::string& filepath);

// Export TileCache to standard image path.
bool ExportTileCacheToFile(const TileCache& cache, const std::string& filepath);

// Build default export path: {dir}/{setName}_{MapKind}.ext
std::string DefaultMapExportPath(const TextureSet& set, MapKind kind,
                                 const std::string& baseDir, const std::string& ext = "png");

// Quick-export all enabled maps of a set.
// Diffuse: optional external pixels (from Canvas composite) written if diffusePixels non-null.
// Other maps: from packed layer pixels (maps are layers; no hidden composites).
// Returns number of maps successfully written.
struct ExportAllResult {
    int written = 0;
    int failed = 0;
    std::string log;
};

// Optional per-map precomposed pixels (from Canvas::ComposePackedMapRGBA8).
// Key = (int)MapKind. Optional precomposed pixels from Canvas::ComposePackedMapRGBA8.
struct MapExportPixels {
    std::vector<uint8_t> rgba; // w*h*4
    int w = 0, h = 0;
};

ExportAllResult ExportAllMaps(
    TextureSet& set,
    const std::string& baseDir,
    const std::string& ext,
    const TileCache* diffuseOverride, // if non-null, use for Diffuse instead of set composite
    const std::vector<uint8_t>* diffuseRgba8, // alternative flat RGBA8 for Diffuse (w*h*4)
    int diffuseW, int diffuseH,
    const std::unordered_map<int, MapExportPixels>* packedByMap = nullptr);

// Resolve which MapKind packs a role (first match among enabled maps).
MapKind MapKindForRole(const TextureSet& set, ChannelRole role);

// Which channel index (0-3) packs role on map; -1 if none.
int ChannelIndexForRole(const MapSlot& slot, ChannelRole role);

// Extract single channel as grayscale RGBA8 into out cache (same size as source).
bool ExtractChannelAsGrayscale(const TileCache& src, int channelIndex, bool invert, TileCache& out);

} // namespace texset
