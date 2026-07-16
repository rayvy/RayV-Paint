#pragma once
// RayV Painter package formats (RVPCF / RVPAF / RVPBF).
// All three share the same on-disk container ("RVPK"); format is declared in the
// embedded manifest JSON. No legacy .rvbrush / .rayexpt / bare preset JSON.
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace rvp {

// Container magic "RVPK"
static constexpr uint32_t kPackageMagic = 0x4B505652u; // 'RVPK' LE
static constexpr uint32_t kPackageVersion = 1;

enum class PackageFormat : uint8_t {
    Unknown = 0,
    RVPCF = 1, // RayV Painter Config File  — export templates, shader presets
    RVPAF = 2, // RayV Painter Asset File   — texture / media assets
    RVPBF = 3  // RayV Painter Brush File   — brush preset + tip resources
};

// Sub-kinds for RVPCF (config packages)
enum class ConfigKind : uint8_t {
    Unknown = 0,
    ExportTemplate = 1, // map export naming / container defaults
    ShaderPreset = 2,   // 3D preview material / channel graph
};

// Sub-kinds for RVPAF
enum class AssetKind : uint8_t {
    Unknown = 0,
    Texture = 1,
    SmartSource = 2
};

inline const char* PackageFormatName(PackageFormat f) {
    switch (f) {
    case PackageFormat::RVPCF: return "RVPCF";
    case PackageFormat::RVPAF: return "RVPAF";
    case PackageFormat::RVPBF: return "RVPBF";
    default: return "Unknown";
    }
}

inline PackageFormat PackageFormatFromName(const std::string& s) {
    if (s == "RVPCF" || s == "rvpcf") return PackageFormat::RVPCF;
    if (s == "RVPAF" || s == "rvpaf") return PackageFormat::RVPAF;
    if (s == "RVPBF" || s == "rvpbf") return PackageFormat::RVPBF;
    return PackageFormat::Unknown;
}

inline const char* ExtensionFor(PackageFormat f) {
    switch (f) {
    case PackageFormat::RVPCF: return ".rvpcf";
    case PackageFormat::RVPAF: return ".rvpaf";
    case PackageFormat::RVPBF: return ".rvpbf";
    default: return "";
    }
}

// In-memory package: manifest JSON string + named resource blobs.
struct Package {
    PackageFormat format = PackageFormat::Unknown;
    std::string manifestJson; // full JSON document (must include "format")
    // Relative paths → raw bytes (e.g. "config.json", "image.png", "tip.raw8", "shaders/x.hlsl")
    std::unordered_map<std::string, std::vector<uint8_t>> resources;

    bool empty() const { return manifestJson.empty() && resources.empty(); }
    bool Has(const std::string& path) const { return resources.find(path) != resources.end(); }
    const std::vector<uint8_t>* Get(const std::string& path) const {
        auto it = resources.find(path);
        return it == resources.end() ? nullptr : &it->second;
    }
    void Set(const std::string& path, std::vector<uint8_t> data) {
        resources[path] = std::move(data);
    }
    void SetText(const std::string& path, const std::string& text) {
        resources[path] = std::vector<uint8_t>(text.begin(), text.end());
    }
    std::string GetText(const std::string& path) const {
        const auto* d = Get(path);
        if (!d) return {};
        return std::string(d->begin(), d->end());
    }
};

// Well-known resource paths inside packages
namespace paths {
inline constexpr const char* kConfigJson = "config.json";
inline constexpr const char* kImagePng = "image.png";
inline constexpr const char* kImageDds = "image.dds";
inline constexpr const char* kThumbPng = "thumbnail.png";
inline constexpr const char* kThumbHPng = "thumbnail_h.png";
inline constexpr const char* kTipRaw8 = "tip.raw8";
inline constexpr const char* kShadersDir = "shaders/";
} // namespace paths

} // namespace rvp
