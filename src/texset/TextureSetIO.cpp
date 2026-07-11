#include "TextureSetIO.h"
#include "../core/DdsHelper.h"
#include "../core/ImageManager.h"
#include "../core/Logger.h"
#include "../core/PathUtil.h"

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

bool ImportMapFromFile(TextureSet& set, MapKind kind, const std::string& filepath,
                       ChannelRole soloRole) {
    if (filepath.empty()) return false;

    int w = 0, h = 0;
    std::unique_ptr<TileCache> loaded = std::make_unique<TileCache>();
    const std::string ext = ExtLower(filepath);

    if (ext == "dds") {
        DdsFormat fmt = DdsFormat::RGBA8_UNORM;
        if (!DdsHelper::LoadDDSToTileCache(filepath, *loaded, w, h, fmt)) {
            Logger::Get().ErrorTag("texset", "ImportMap DDS failed: " + filepath);
            return false;
        }
    } else {
        std::vector<uint8_t> rgba;
        if (!ImageManager::LoadImageFromFile(filepath, rgba, w, h)) {
            Logger::Get().ErrorTag("texset", "ImportMap image failed: " + filepath);
            return false;
        }
        loaded->Init(w, h, CanvasPixelFormat::RGBA8);
        loaded->ImportRGBA8(rgba.data(), w, h);
    }

    // Optional: solo role → grayscale from packed channel of the *file as-if* using current pack
    // After enable, we use template pack on the slot to know which channel holds the role.
    if (!set.EnableMap(kind, w, h, filepath)) {
        Logger::Get().ErrorTag("texset", "EnableMap failed");
        return false;
    }

    MapSlot* slot = set.GetMap(kind);
    if (!slot) return false;

    if (soloRole != ChannelRole::None) {
        int ch = ChannelIndexForRole(*slot, soloRole);
        if (ch < 0) {
            // Fall back: use R for single-channel intent
            ch = 0;
            Logger::Get().WarnTag("texset",
                std::string("Role ") + ChannelRoleName(soloRole) +
                " not in pack for " + MapKindName(kind) + " — using R");
        }
        auto gray = std::make_unique<TileCache>();
        const bool inv = (ch >= 0 && ch < 4) ? slot->pack[ch].invert : false;
        if (!ExtractChannelAsGrayscale(*loaded, ch < 0 ? 0 : ch, inv, *gray)) {
            Logger::Get().ErrorTag("texset", "Channel extract failed");
            return false;
        }
        loaded = std::move(gray);
    }

    int key = (int)kind;
    set.mapComposites[key] = std::move(loaded);
    set.mapCompositeDirty[key] = false;
    set.activeMap = kind;

    Logger::Get().InfoTag("texset",
        std::string("Imported ") + MapKindName(kind) + " " +
        std::to_string(w) + "x" + std::to_string(h) + " → set '" + set.name + "'");
    return true;
}

bool ExportTileCacheToFile(const TileCache& cache, const std::string& filepath) {
    const int w = cache.GetWidth();
    const int h = cache.GetHeight();
    if (w <= 0 || h <= 0) return false;
    std::vector<uint8_t> rgba((size_t)w * (size_t)h * 4u);
    cache.ExportRGBA8(rgba.data(), w, h);
    return ImageManager::SaveRGBA8ToFile(filepath, rgba.data(), w, h);
}

bool ExportMapToFile(const TextureSet& set, MapKind kind, const std::string& filepath) {
    auto it = set.mapComposites.find((int)kind);
    if (it == set.mapComposites.end() || !it->second) {
        Logger::Get().WarnTag("texset",
            std::string("No composite for ") + MapKindName(kind));
        return false;
    }
    return ExportTileCacheToFile(*it->second, filepath);
}

std::string DefaultMapExportPath(const TextureSet& set, MapKind kind,
                                 const std::string& baseDir, const std::string& ext) {
    std::string name = SanitizeFileToken(set.name.empty() ? "Set" : set.name);
    std::string map = MapKindName(kind);
    std::string e = ext.empty() ? "png" : ext;
    if (!e.empty() && e[0] == '.') e.erase(e.begin());
    fs::path dir = baseDir.empty() ? fs::path(".") : fs::path(PathUtil::Utf8ToWide(baseDir));
    fs::path out = dir / (PathUtil::Utf8ToWide(name + "_" + map + "." + e));
    return PathUtil::WideToUtf8(out.wstring());
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

        // Prefer per-map exportPath; else suffix-aware default
        std::string path = m.exportPath;
        if (path.empty()) {
            std::string e = ext;
            // Codec may force dds extension
            if (m.exportCodec != MapExportCodec::PNG && m.exportCodec != MapExportCodec::RGBA8_UNORM)
                e = "png"; // still PNG until texconv hook; path stays writable
            path = DefaultMapExportPath(set, m.kind, baseDir, e);
            // Prefer nameSuffix in filename if set
            if (!m.nameSuffix.empty()) {
                std::string name = SanitizeFileToken(set.name.empty() ? "Set" : set.name);
                try {
                    fs::path dir = baseDir.empty() ? fs::path(".") : fs::path(PathUtil::Utf8ToWide(baseDir));
                    fs::path out = dir / PathUtil::Utf8ToWide(name + m.nameSuffix + "." + (e.empty() ? "png" : e));
                    path = PathUtil::WideToUtf8(out.wstring());
                } catch (...) {}
            }
        }
        if (MapSlot* slot = set.GetMap(m.kind))
            slot->exportPath = path;

        bool ok = false;
        // 1) Packed compose from Canvas (preferred — channel packing)
        if (packedByMap) {
            auto it = packedByMap->find((int)m.kind);
            if (it != packedByMap->end() && it->second.w > 0 && it->second.h > 0 &&
                it->second.rgba.size() >= (size_t)it->second.w * (size_t)it->second.h * 4u) {
                ok = ImageManager::SaveRGBA8ToFile(path, it->second.rgba.data(),
                                                  it->second.w, it->second.h);
            }
        }
        // 2) Diffuse fallbacks
        if (!ok && m.kind == MapKind::Diffuse) {
            if (diffuseOverride && diffuseOverride->GetWidth() > 0) {
                ok = ExportTileCacheToFile(*diffuseOverride, path);
            } else if (diffuseRgba8 && diffuseW > 0 && diffuseH > 0 &&
                       diffuseRgba8->size() >= (size_t)diffuseW * (size_t)diffuseH * 4u) {
                ok = ImageManager::SaveRGBA8ToFile(path, diffuseRgba8->data(), diffuseW, diffuseH);
            }
        }
        // 3) Imported composite
        if (!ok)
            ok = ExportMapToFile(set, m.kind, path);

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
