#pragma once
// Asset Browser — ownership categories, kinds, and shared texture identity.
// Raster layer pixels are NOT assets until explicitly imported/converted.
// Package formats: see Documentation.MD (RVPAF / RVPCF / RVPBF).
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
    External = 3  // Absolute path outside libraries (one-shot import)
};

// Consumer type filter. Fill accepts Texture only.
enum class AssetKind : uint8_t {
    Texture = 0,       // .rvpaf (texture) or raw image during import
    SmartSource,
    Brush,             // .rvpbf
    ExportTemplate,    // .rvpcf kind=export_template
    ShaderPreset,      // .rvpcf kind=shader_preset
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
    std::vector<uint8_t> rgba; // legacy alias
    int refCount = 0;
    AssetLoadState state = AssetLoadState::Missing;
    std::vector<uint8_t> fileBytes; // original package or image bytes for packing
    std::string mime; // e.g. "image/png" or "application/rvpaf"
};

struct AssetFilter {
    AssetKind kind = AssetKind::Texture;
    bool includeCore = true;
    bool includeUser = true;
    bool includeProject = true;
    bool includeExternal = false;
    std::string search;
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
    case AssetKind::Texture:        return "texture";
    case AssetKind::SmartSource:    return "smart_source";
    case AssetKind::Brush:          return "brush";
    case AssetKind::ExportTemplate: return "export_template";
    case AssetKind::ShaderPreset:   return "shader_preset";
    default: return "unknown";
    }
}

inline AssetKind KindFromName(const std::string& s) {
    if (s == "texture") return AssetKind::Texture;
    if (s == "smart_source") return AssetKind::SmartSource;
    if (s == "brush" || s == "brush_tip") return AssetKind::Brush;
    if (s == "export_template") return AssetKind::ExportTemplate;
    if (s == "shader_preset" || s == "preview3d_template") return AssetKind::ShaderPreset;
    return AssetKind::Unknown;
}

bool ParseKey(const std::string& key, AssetCategory& outCat, std::string& outRest);
std::string MakeKey(AssetCategory cat, const std::string& rest);
bool IsTextureExtension(const std::string& extLower);
bool IsPackageExtension(const std::string& extLower);
AssetKind GuessKindFromPath(const std::string& pathOrName);

// Sidecar thumbs for raw images (legacy import only).
// RVPAF packages embed thumbnail.png / thumbnail_h.png inside the package.
std::string ThumbPathFor(const std::string& assetPath, bool highQuality);

} // namespace assets
