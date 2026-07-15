#include "AssetTypes.h"
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace fs = std::filesystem;

namespace assets {

bool ParseKey(const std::string& key, AssetCategory& outCat, std::string& outRest) {
    outRest.clear();
    if (key.size() >= 5 && key.compare(0, 5, "core:") == 0) {
        outCat = AssetCategory::BuiltIn;
        outRest = key.substr(5);
        return true;
    }
    if (key.size() >= 5 && key.compare(0, 5, "user:") == 0) {
        outCat = AssetCategory::User;
        outRest = key.substr(5);
        return true;
    }
    if (key.size() >= 5 && key.compare(0, 5, "proj:") == 0) {
        outCat = AssetCategory::Project;
        outRest = key.substr(5);
        return true;
    }
    if (key.size() >= 4 && key.compare(0, 4, "ext:") == 0) {
        outCat = AssetCategory::External;
        outRest = key.substr(4);
        return true;
    }
    // Legacy: "builtin:" accepted as core
    if (key.size() >= 8 && key.compare(0, 8, "builtin:") == 0) {
        outCat = AssetCategory::BuiltIn;
        outRest = key.substr(8);
        return true;
    }
    return false;
}

std::string MakeKey(AssetCategory cat, const std::string& rest) {
    switch (cat) {
    case AssetCategory::BuiltIn:  return "core:" + rest;
    case AssetCategory::User:     return "user:" + rest;
    case AssetCategory::Project:  return "proj:" + rest;
    case AssetCategory::External: return "ext:" + rest;
    }
    return rest;
}

bool IsTextureExtension(const std::string& extLower) {
    return extLower == ".png" || extLower == ".jpg" || extLower == ".jpeg" ||
           extLower == ".tga" || extLower == ".bmp" || extLower == ".dds";
}

AssetKind GuessKindFromPath(const std::string& pathOrName) {
    std::string ext;
    try {
        ext = fs::path(pathOrName).extension().string();
    } catch (...) {
        return AssetKind::Unknown;
    }
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    if (IsTextureExtension(ext)) return AssetKind::Texture;
    if (ext == ".rayexpt") return AssetKind::ExportTemplate;
    if (ext == ".ray3dt") return AssetKind::Preview3dTemplate;
    if (ext == ".rvbrush") return AssetKind::BrushTip;
    if (ext == ".svg") return AssetKind::SmartSource;
    return AssetKind::Unknown;
}

std::string ThumbPathFor(const std::string& assetPath, bool highQuality) {
    try {
        fs::path p(assetPath);
        std::string stem = p.string();
        // Replace: file.ext → file.thumbnail.png / file.thumbnail_h.png
        // Use full path without changing directory.
        std::string s = p.string();
        auto dot = s.find_last_of('.');
        auto slash = s.find_last_of("/\\");
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
            s = s.substr(0, dot);
        return s + (highQuality ? ".thumbnail_h.png" : ".thumbnail.png");
    } catch (...) {
        return assetPath + (highQuality ? ".thumbnail_h.png" : ".thumbnail.png");
    }
}

} // namespace assets
