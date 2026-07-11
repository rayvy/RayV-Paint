#include "TextureSetIO.h"
#include "../core/DdsHelper.h"
#include "../core/ImageManager.h"
#include "../core/Logger.h"
#include "../core/PathUtil.h"
#include "../core/TexconvHelper.h"
#include "../core/ConfigManager.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <vector>

namespace texset {
namespace fs = std::filesystem;

static std::string ExtLower(const std::string& path) {
    size_t d = path.find_last_of('.');
    if (d == std::string::npos) return {};
    std::string e = path.substr(d + 1);
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return e;
}

static std::string SanitizeFileToken(std::string s) {
    for (char& c : s) {
        if (c == ' ' || c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
    }
    return s;
}

int ChannelIndexForRole(const MapSlot& slot, ChannelRole role) {
    return ResolveChannelForRole(slot, role);
}

MapKind MapKindForRole(const TextureSet& set, ChannelRole role) {
    return ResolveMapForRole(set.maps, role);
}

bool ExtractChannelAsGrayscale(const TileCache& src, int channelIndex, bool invert, TileCache& out) {
    if (channelIndex < 0 || channelIndex > 3) return false;
    const int w = src.GetWidth();
    const int h = src.GetHeight();
    if (w <= 0 || h <= 0) return false;
    out.Init(w, h, CanvasPixelFormat::RGBA8);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float rgba[4];
            src.GetPixelF(x, y, rgba);
            float v = rgba[channelIndex];
            if (invert) v = 1.f - v;
            float o[4] = { v, v, v, 1.f };
            out.SetPixelF(x, y, o);
        }
    }
    return true;
}

// Deprecated path: only updates map *meta*. Pixels live on Canvas layers.
bool ImportMapFromFile(TextureSet& set, MapKind kind, const std::string& filepath,
                       ChannelRole /*soloRole*/) {
    if (filepath.empty()) return false;
    int w = 0, h = 0;
    // Peek size without storing composite
    std::string ext = ExtLower(filepath);
    if (ext == "dds") {
        TileCache tmp;
        DdsFormat fmt = DdsFormat::RGBA8_UNORM;
        if (!DdsHelper::LoadDDSToTileCache(filepath, tmp, w, h, fmt))
            return false;
    } else {
        std::vector<uint8_t> rgba;
        if (!ImageManager::LoadImageFromFile(filepath, rgba, w, h))
            return false;
    }
    return set.EnableMap(kind, w, h, filepath);
}

bool ExportTileCacheToFile(const TileCache& cache, const std::string& filepath) {
    const int w = cache.GetWidth();
    const int h = cache.GetHeight();
    if (w <= 0 || h <= 0) return false;
    std::vector<uint8_t> rgba((size_t)w * (size_t)h * 4u);
    cache.ExportRGBA8(rgba.data(), w, h);
    return ImageManager::SaveRGBA8ToFile(filepath, rgba.data(), w, h);
}

bool ExportMapToFile(const TextureSet& /*set*/, MapKind /*kind*/, const std::string& /*filepath*/) {
    // mapComposites removed — use ExportAllMaps with packed layer pixels
    return false;
}

std::string DefaultMapExportPath(const TextureSet& set, MapKind kind,
                                 const std::string& baseDir, const std::string& ext) {
    std::string name = SanitizeFileToken(set.name.empty() ? "Set" : set.name);
    const MapSlot* slot = FindMap(set.maps, kind);
    std::string map = (slot && !slot->nameSuffix.empty())
        ? slot->nameSuffix
        : (std::string("_") + MapKindName(kind));
    if (!map.empty() && map[0] != '_') map = "_" + map;
    std::string e = ext.empty() ? "png" : ext;
    if (!e.empty() && e[0] == '.') e.erase(e.begin());
    fs::path dir = baseDir.empty() ? fs::path(".") : fs::path(PathUtil::Utf8ToWide(baseDir));
    fs::path out = dir / (PathUtil::Utf8ToWide(name + map + "." + e));
    return PathUtil::WideToUtf8(out.wstring());
}

static bool SaveWithCodec(const std::string& path, const uint8_t* rgba, int w, int h,
                          MapExportCodec codec, bool mips) {
    std::string ext = ExtLower(path);
    // Force PNG if codec is PNG/RGBA8
    if (codec == MapExportCodec::PNG || codec == MapExportCodec::RGBA8_UNORM ||
        ext == "png" || ext == "tga" || ext == "bmp" || ext == "jpg" || ext == "jpeg") {
        // Ensure .png if path had .dds but codec is PNG
        std::string outPath = path;
        if (ext == "dds" && (codec == MapExportCodec::PNG || codec == MapExportCodec::RGBA8_UNORM)) {
            size_t d = outPath.find_last_of('.');
            if (d != std::string::npos) outPath = outPath.substr(0, d) + ".png";
        }
        return ImageManager::SaveRGBA8ToFile(outPath, rgba, w, h);
    }

    // DDS path via temp + texconv
    std::string tempDir = ConfigManager::GetUserSubdirectory("temp");
    std::string tempPng = tempDir + "/texset_export_tmp.png";
    if (!ImageManager::SaveRGBA8ToFile(tempPng, rgba, w, h))
        return false;

    std::string formatStr = "BC7_UNORM";
    switch (codec) {
    case MapExportCodec::BC7_UNORM_SRGB: formatStr = "BC7_UNORM_SRGB"; break;
    case MapExportCodec::BC7_UNORM: formatStr = "BC7_UNORM"; break;
    case MapExportCodec::BC5_UNORM: formatStr = "BC5_UNORM"; break;
    case MapExportCodec::R8G8_UNORM: formatStr = "R8G8_UNORM"; break;
    case MapExportCodec::R32_FLOAT: formatStr = "R32_FLOAT"; break;
    default: formatStr = "BC7_UNORM"; break;
    }

    std::string outPath = path;
    if (ext != "dds") {
        size_t d = outPath.find_last_of('.');
        if (d != std::string::npos) outPath = outPath.substr(0, d) + ".dds";
        else outPath += ".dds";
    }

    ExportSettings settings;
    settings.isDds = true;
    settings.ddsFormatStr = formatStr;
    settings.advancedMode = true;
    settings.compressionSpeed = "Medium";
    settings.generateMipMaps = mips;
    settings.mipFilter = "Bicubic";
    settings.exportPath = outPath;

    // Texconv expects input image/dds
    bool ok = TexconvHelper::CompressDDS(tempPng, outPath, settings);
    try { fs::remove(PathUtil::FromUtf8(tempPng)); } catch (...) {}
    return ok;
}

ExportAllResult ExportAllMaps(
    TextureSet& set,
    const std::string& baseDir,
    const std::string& ext,
    const TileCache* diffuseOverride,
    const std::vector<uint8_t>* diffuseRgba8,
    int diffuseW, int diffuseH,
    const std::unordered_map<int, MapExportPixels>* packedByMap) {

    ExportAllResult r;
    for (const auto& m : set.maps) {
        if (!m.enabled) continue;

        std::string path = m.exportPath;
        std::string useExt = ext;
        if (m.exportCodec != MapExportCodec::PNG && m.exportCodec != MapExportCodec::RGBA8_UNORM)
            useExt = "dds";
        else if (useExt.empty())
            useExt = "png";

        if (path.empty())
            path = DefaultMapExportPath(set, m.kind, baseDir, useExt);
        if (MapSlot* slot = set.GetMap(m.kind))
            slot->exportPath = path;

        bool ok = false;
        if (packedByMap) {
            auto it = packedByMap->find((int)m.kind);
            if (it != packedByMap->end() && it->second.w > 0 &&
                it->second.rgba.size() >= (size_t)it->second.w * (size_t)it->second.h * 4u) {
                ok = SaveWithCodec(path, it->second.rgba.data(), it->second.w, it->second.h,
                                   m.exportCodec, m.exportMips);
            }
        }
        if (!ok && m.kind == MapKind::Diffuse) {
            if (diffuseOverride && diffuseOverride->GetWidth() > 0) {
                std::vector<uint8_t> tmp((size_t)diffuseOverride->GetWidth() *
                                         (size_t)diffuseOverride->GetHeight() * 4u);
                diffuseOverride->ExportRGBA8(tmp.data(), diffuseOverride->GetWidth(),
                                             diffuseOverride->GetHeight());
                ok = SaveWithCodec(path, tmp.data(), diffuseOverride->GetWidth(),
                                   diffuseOverride->GetHeight(), m.exportCodec, m.exportMips);
            } else if (diffuseRgba8 && diffuseW > 0 && diffuseH > 0) {
                ok = SaveWithCodec(path, diffuseRgba8->data(), diffuseW, diffuseH,
                                   m.exportCodec, m.exportMips);
            }
        }

        if (ok) {
            ++r.written;
            r.log += "OK  " + path + "\n";
            Logger::Get().InfoTag("texset", "Exported " + path);
        } else {
            ++r.failed;
            r.log += "FAIL " + path + " (" + MapKindName(m.kind) + ")\n";
            Logger::Get().WarnTag("texset",
                std::string("Export failed for ") + MapKindName(m.kind) + " → " + path);
        }
    }
    return r;
}

} // namespace texset
