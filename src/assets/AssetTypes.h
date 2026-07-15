#pragma once
// Asset Browser — ownership categories, kinds, and shared texture identity.
// Raster layer pixels are NOT assets until explicitly imported/converted.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace assets {

// Ownership roots (not file types). Display name for BuiltIn is "Core".
enum class AssetCategory : uint8_t {
    BuiltIn  = 0, // Core — {exe}/assets/...
    User     = 1, // User library — Documents/RayVPaint/assets/...
    Project  = 2, // Session + packed into .rayp
    External = 3  // Absolute path outside libraries (migration / one-shot)
};

// Consumer type filter. Fill accepts Texture only.
enum class AssetKind : uint8_t {
    Texture = 0,
    SmartSource,
    BrushTip,
    ExportTemplate,      // .rayexpt — hook only
    Preview3dTemplate,   // .ray3dt — hook only
    Unknown
};

enum class AssetLoadState : uint8_t {
    Missing = 0,
    Pending,
    Ready,
    Failed
};

// Stable string key used on layers / serialization.
// Formats:
//   "core:<relative under exe assets>"
//   "user:<relative under user assets>"
//   "proj:<uuid>"
//   "ext:<normalized absolute path>"
struct AssetId {
    AssetCategory cat = AssetCategory::External;
    std::string key; // full store key (includes prefix)

    bool empty() const { return key.empty(); }
    bool operator==(const AssetId& o) const { return key == o.key; }
};

// Shared CPU payload — refcounted so sample paths stay safe across async commit.
struct TexturePayload {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> rgba; // RGBA8, size w*h*4
};

struct TextureAsset {
    AssetId id;
    AssetKind kind = AssetKind::Texture;
    std::string sourcePath; // absolute path if file-backed; empty for pure proj blob
    std::string displayName;
    int w = 0;
    int h = 0;
    std::shared_ptr<const TexturePayload> payload; // null until Ready
    // Legacy alias for callers that still read .rgba directly after Get()
    // Prefer payload / TryGetPayload.
    std::vector<uint8_t> rgba;
    int refCount = 0;
    AssetLoadState state = AssetLoadState::Missing;
    // Original file bytes for project packing (optional; empty → pack from rgba PNG).
    std::vector<uint8_t> fileBytes;
    std::string mime; // e.g. "image/png"
};

struct AssetFilter {
    AssetKind kind = AssetKind::Texture;
    bool includeCore = true;
    bool includeUser = true;
    bool includeProject = true;
    bool includeExternal = false;
    std::string search; // case-insensitive substring of displayName/key
};

struct AssetInfo {
    std::string key;
    AssetCategory category = AssetCategory::External;
    AssetKind kind = AssetKind::Unknown;
    std::string displayName;
    std::string sourcePath;
    int w = 0;
    int h = 0;
    AssetLoadState loadState = AssetLoadState::Missing;
};

inline const char* CategoryDisplayName(AssetCategory c) {
    switch (c) {
    case AssetCategory::BuiltIn:  return "Core";
    case AssetCategory::User:     return "User";
    case AssetCategory::Project:  return "Project";
    case AssetCategory::External: return "External";
    }
    return "Unknown";
}

inline const char* KindName(AssetKind k) {
    switch (k) {
    case AssetKind::Texture:           return "texture";
    case AssetKind::SmartSource:       return "smart_source";
    case AssetKind::BrushTip:          return "brush_tip";
    case AssetKind::ExportTemplate:    return "export_template";
    case AssetKind::Preview3dTemplate: return "preview3d_template";
    default: return "unknown";
    }
}

inline AssetKind KindFromName(const std::string& s) {
    if (s == "texture") return AssetKind::Texture;
    if (s == "smart_source") return AssetKind::SmartSource;
    if (s == "brush_tip") return AssetKind::BrushTip;
    if (s == "export_template") return AssetKind::ExportTemplate;
    if (s == "preview3d_template") return AssetKind::Preview3dTemplate;
    return AssetKind::Unknown;
}

// Key helpers
bool ParseKey(const std::string& key, AssetCategory& outCat, std::string& outRest);
std::string MakeKey(AssetCategory cat, const std::string& rest);
bool IsTextureExtension(const std::string& extLower);
AssetKind GuessKindFromPath(const std::string& pathOrName);

// Sidecar thumb paths next to asset file:
//   asset.ext  →  asset.thumbnail.png (32) / asset.thumbnail_h.png (128)
std::string ThumbPathFor(const std::string& assetPath, bool highQuality);

} // namespace assets
