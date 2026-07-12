#pragma once
// Asset Browser / shared texture identity (Build 16).
// Categories = ownership roots, not file types. B16 scope: textures only.
#include <cstdint>
#include <string>
#include <vector>

namespace assets {

enum class AssetCategory : uint8_t {
    BuiltIn = 0, // {exe}/assets/...
    User    = 1, // {AppData|Documents}/RayVPaint/assets/...
    Project = 2, // packed into .rayp (later)
    External = 3 // absolute path outside libraries (Fill import via FE/Win32)
};

// Stable string key used on layers / serialization.
// Formats:
//   "ext:<normalized absolute path>"
//   "user:<relative under user assets>"
//   "builtin:<relative under exe assets>"
//   "proj:<uuid or name>"  (S7)
struct AssetId {
    AssetCategory cat = AssetCategory::External;
    std::string key; // full store key (includes prefix)

    bool empty() const { return key.empty(); }
    bool operator==(const AssetId& o) const { return key == o.key; }
};

struct TextureAsset {
    AssetId id;
    std::string sourcePath; // absolute path if known (for UI / re-export)
    int w = 0;
    int h = 0;
    std::vector<uint8_t> rgba; // RGBA8, size w*h*4; shared by refcount
    int refCount = 0;
};

} // namespace assets
