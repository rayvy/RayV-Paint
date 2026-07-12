#include "TextureSetIO.h"
#include "../core/DdsHelper.h"
#include "../core/ImageManager.h"
#include "../core/Logger.h"
#include "../core/PathUtil.h"
#include "../core/TexconvHelper.h"
#include "../core/ConfigManager.h"

#include <algorithm>
#include <atomic>
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

bool IsLosslessImageCodec(MapExportCodec codec) {
    return codec == MapExportCodec::PNG || codec == MapExportCodec::RGBA8_UNORM;
}

std::string CodecToFormatString(MapExportCodec codec) {
    switch (codec) {
    case MapExportCodec::BC7_UNORM_SRGB: return "BC7_UNORM_SRGB";
    case MapExportCodec::BC7_UNORM:      return "BC7_UNORM";
    case MapExportCodec::BC5_UNORM:      return "BC5_UNORM";
    case MapExportCodec::R8G8_UNORM:     return "R8G8_UNORM";
    case MapExportCodec::R32_FLOAT:      return "R32_FLOAT";
    case MapExportCodec::RGBA8_UNORM:    return "RGBA8_UNORM";
    case MapExportCodec::PNG:
    default:                            return "BC7_UNORM";
    }
}

std::string ForcePathExtension(const std::string& path, const char* extNoDot) {
    if (!extNoDot || !*extNoDot) return path;
    std::string e = extNoDot;
    if (!e.empty() && e[0] == '.') e.erase(e.begin());
    size_t slash = path.find_last_of("/\\");
    size_t dot = path.find_last_of('.');
    std::string base = path;
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        base = path.substr(0, dot);
    return base + "." + e;
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
    // Use ExportAllMaps with packed layer pixels (ComposePackedMapRGBA8)
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

// Resolve actual write format for one map given optional global batch options.
static void ResolveExportTarget(const MapSlot& m, const BatchExportOptions* options,
                                bool& outAsPng, std::string& outFormatStr,
                                bool& outMips, std::string& outMipFilter,
                                std::string& outSpeed) {
    outAsPng = true;
    outFormatStr.clear();
    outMips = true;
    outMipFilter = "Bicubic";
    outSpeed = "Medium";

    if (options) {
        outMips = options->generateMipMaps;
        outMipFilter = options->mipFilter;
        outSpeed = options->compressionSpeed;

        if (options->container == BatchExportOptions::Container::PNG) {
            outAsPng = true;
            return;
        }

        // DDS container
        outAsPng = false;
        if (options->usePerMapCodec && !IsLosslessImageCodec(m.exportCodec)) {
            outFormatStr = CodecToFormatString(m.exportCodec);
        } else {
            outFormatStr = options->ddsFormat.empty()
                ? "BC7 (sRGB, DX 11+)"
                : options->ddsFormat;
        }
        return;
    }

    // No global options: decide from per-map codec only
    if (IsLosslessImageCodec(m.exportCodec)) {
        outAsPng = true;
    } else {
        outAsPng = false;
        outFormatStr = CodecToFormatString(m.exportCodec);
        outMips = m.exportMips;
    }
}

static bool SavePngWithOptionalIcc(const std::string& path, const uint8_t* rgba, int w, int h,
                                   const BatchExportOptions* options) {
    std::string outPath = ForcePathExtension(path, "png");
    if (options && options->iccBytes && options->iccSize > 0) {
        return ImageManager::SaveRGBA8ToFile(
            outPath, rgba, w, h, w * 4,
            options->iccBytes, options->iccSize,
            options->iccProfileName ? options->iccProfileName : "sRGB");
    }
    return ImageManager::SaveRGBA8ToFile(outPath, rgba, w, h);
}

static bool SaveDdsWithFormat(const std::string& path, const uint8_t* rgba, int w, int h,
                              const std::string& formatStr, bool mips,
                              const std::string& mipFilter, const std::string& speed) {
    std::string outPath = ForcePathExtension(path, "dds");

    std::string tempDir = ConfigManager::GetUserSubdirectory("temp");
    // Unique temp name avoids collisions when exporting multiple maps in one batch
    static std::atomic<uint32_t> s_tmpSeq{0};
    std::string tempPng = tempDir + "/texset_export_tmp_" +
        std::to_string(s_tmpSeq.fetch_add(1)) + ".png";

    if (!ImageManager::SaveRGBA8ToFile(tempPng, rgba, w, h))
        return false;

    ExportSettings settings;
    settings.isDds = true;
    settings.ddsFormatStr = formatStr.empty() ? "BC7_UNORM" : formatStr;
    settings.advancedMode = true;
    settings.compressionSpeed = speed.empty() ? "Medium" : speed;
    settings.generateMipMaps = mips;
    settings.mipFilter = mipFilter.empty() ? "Bicubic" : mipFilter;
    settings.exportPath = outPath;

    bool ok = TexconvHelper::CompressDDS(tempPng, outPath, settings);
    try { fs::remove(PathUtil::FromUtf8(tempPng)); } catch (...) {}
    return ok;
}

static bool SaveWithResolved(const std::string& path, const uint8_t* rgba, int w, int h,
                             bool asPng, const std::string& formatStr, bool mips,
                             const std::string& mipFilter, const std::string& speed,
                             const BatchExportOptions* options) {
    if (asPng)
        return SavePngWithOptionalIcc(path, rgba, w, h, options);
    return SaveDdsWithFormat(path, rgba, w, h, formatStr, mips, mipFilter, speed);
}

ExportAllResult ExportAllMaps(
    TextureSet& set,
    const std::string& baseDir,
    const std::string& ext,
    const TileCache* diffuseOverride,
    const std::vector<uint8_t>* diffuseRgba8,
    int diffuseW, int diffuseH,
    const std::unordered_map<int, MapExportPixels>* packedByMap,
    const BatchExportOptions* options) {

    ExportAllResult r;
    for (const auto& m : set.maps) {
        if (!m.enabled) continue;

        bool asPng = true;
        std::string formatStr;
        bool mips = m.exportMips;
        std::string mipFilter = "Bicubic";
        std::string speed = "Medium";
        ResolveExportTarget(m, options, asPng, formatStr, mips, mipFilter, speed);

        // Prefer slot mips flag when no global options
        if (!options)
            mips = m.exportMips;

        std::string useExt = asPng ? "png" : "dds";
        if (!options && !ext.empty()) {
            // Legacy: caller-supplied default only for empty paths when codec is lossless
            if (asPng) useExt = ext;
        }

        std::string path = m.exportPath;
        if (path.empty())
            path = DefaultMapExportPath(set, m.kind, baseDir, useExt);
        // Always align extension with resolved container (fixes stale .png paths with DDS codec)
        path = ForcePathExtension(path, useExt.c_str());

        if (MapSlot* slot = set.GetMap(m.kind))
            slot->exportPath = path;

        bool ok = false;
        if (packedByMap) {
            auto it = packedByMap->find((int)m.kind);
            if (it != packedByMap->end() && it->second.w > 0 &&
                it->second.rgba.size() >= (size_t)it->second.w * (size_t)it->second.h * 4u) {
                ok = SaveWithResolved(path, it->second.rgba.data(), it->second.w, it->second.h,
                                      asPng, formatStr, mips, mipFilter, speed, options);
            }
        }
        if (!ok && m.kind == MapKind::Diffuse) {
            if (diffuseOverride && diffuseOverride->GetWidth() > 0) {
                std::vector<uint8_t> tmp((size_t)diffuseOverride->GetWidth() *
                                         (size_t)diffuseOverride->GetHeight() * 4u);
                diffuseOverride->ExportRGBA8(tmp.data(), diffuseOverride->GetWidth(),
                                             diffuseOverride->GetHeight());
                ok = SaveWithResolved(path, tmp.data(), diffuseOverride->GetWidth(),
                                      diffuseOverride->GetHeight(), asPng, formatStr,
                                      mips, mipFilter, speed, options);
            } else if (diffuseRgba8 && diffuseW > 0 && diffuseH > 0) {
                ok = SaveWithResolved(path, diffuseRgba8->data(), diffuseW, diffuseH,
                                      asPng, formatStr, mips, mipFilter, speed, options);
            }
        }

        if (ok) {
            ++r.written;
            r.log += "OK  " + path + (asPng ? " [PNG]" : (" [DDS " + formatStr + "]")) + "\n";
            Logger::Get().InfoTag("texset", "Exported " + path +
                (asPng ? " (PNG)" : (" (DDS " + formatStr + ")")));
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
