#include "Canvas.h"
#include "core/TileCache.h"
#include "core/Logger.h"
#include "core/ImageManager.h"
#include "core/ConfigManager.h"
#include "core/TexconvHelper.h"
#include <iostream>
#include <algorithm>
#include <fstream>
#include <cstring>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <thread>
#include <stb_image.h>
#include <stb_image_write.h>

#ifdef _WIN32
#include <windows.h>
static std::wstring UTF8ToWString(const std::string& str) {
    if (str.empty()) return L"";
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}
#endif

// Explicitly declare stbi_zlib_compress which is defined in ImageManager.cpp (via stb_image_write implementation)
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);
extern "C" char* stbi_zlib_decode_malloc(const char* buffer, int len, int* outlen);

using json = nlohmann::json;

// --- Helpers ---
static std::vector<float> ExportLayerF(const Layer& layer, int w, int h) {
    std::vector<float> out((size_t)w * h * 4, 0.f);
    if (layer.tileCache) layer.tileCache->ExportRGBA32F(out.data(), w, h);
    return out;
}

static void SetLayerPixelsF(Layer& layer, const std::vector<float>& pixels, int w, int h, CanvasPixelFormat fmt = CanvasPixelFormat::RGBA8) {
    if (!layer.tileCache) {
        layer.tileCache = std::make_unique<TileCache>();
        layer.tileCache->Init(w, h, fmt);
    }
    layer.tileCache->ImportRGBA32F(pixels.data(), w, h);
    layer.tileCache->MarkAllDirty();
    layer.needsUpload = true;
    layer.filtersDirty = true;
}

static bool LayerHasPixels(const Layer& layer) {
    return layer.tileCache && !layer.tileCache->IsEmpty();
}

static std::vector<float> ComposeVisibleLayers(const std::vector<Layer>& layers, int w, int h) {
    std::vector<float> composite((size_t)w * h * 4, 0.f);
    int firstVisibleIdx = -1;
    for (int l = 0; l < (int)layers.size(); ++l) {
        if (layers[l].visible) { firstVisibleIdx = l; break; }
    }
    if (firstVisibleIdx == -1) return composite;

    const auto& baseLayer = layers[firstVisibleIdx];
    if (LayerHasPixels(baseLayer)) {
        auto basePx = ExportLayerF(baseLayer, w, h);
        std::memcpy(composite.data(), basePx.data(), basePx.size() * sizeof(float));
        if (baseLayer.opacity < 1.f) {
            for (size_t i = 0; i < (size_t)w * h; ++i) {
                composite[i * 4 + 3] *= baseLayer.opacity;
            }
        }
    }

    for (int l = firstVisibleIdx + 1; l < (int)layers.size(); ++l) {
        const auto& layer = layers[l];
        if (!layer.visible || !LayerHasPixels(layer)) continue;
        auto layerPx = ExportLayerF(layer, w, h);
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            size_t base = i * 4;
            float srcR = layerPx[base + 0];
            float srcG = layerPx[base + 1];
            float srcB = layerPx[base + 2];
            float srcA = layerPx[base + 3] * layer.opacity;
            if (srcA <= 0.f) continue;
            float destR = composite[base + 0];
            float destG = composite[base + 1];
            float destB = composite[base + 2];
            float destA = composite[base + 3];
            float outA = srcA + destA * (1.f - srcA);
            if (outA > 0.f) {
                composite[base + 0] = (srcR * srcA + destR * destA * (1.f - srcA)) / outA;
                composite[base + 1] = (srcG * srcA + destG * destA * (1.f - srcA)) / outA;
                composite[base + 2] = (srcB * srcA + destB * destA * (1.f - srcA)) / outA;
                composite[base + 3] = outA;
            }
        }
    }
    return composite;
}

static bool ExtractICCFromPNG(const std::string& pngPath, std::vector<uint8_t>& outIccData, std::string& outProfileName) {
#ifdef _WIN32
    std::ifstream file(UTF8ToWString(pngPath), std::ios::binary);
#else
    std::ifstream file(pngPath, std::ios::binary);
#endif
    if (!file.is_open()) return false;

    // Check PNG signature
    uint8_t sig[8];
    if (!file.read(reinterpret_cast<char*>(sig), 8)) return false;
    if (sig[0] != 0x89 || sig[1] != 0x50 || sig[2] != 0x4E || sig[3] != 0x47) return false;

    while (true) {
        uint8_t lenBytes[4];
        if (!file.read(reinterpret_cast<char*>(lenBytes), 4)) break;
        uint32_t len = (lenBytes[0] << 24) | (lenBytes[1] << 16) | (lenBytes[2] << 8) | lenBytes[3];

        char type[4];
        if (!file.read(type, 4)) break;

        if (std::memcmp(type, "iCCP", 4) == 0) {
            std::vector<uint8_t> chunkData(len);
            if (!file.read(reinterpret_cast<char*>(chunkData.data()), len)) break;

            size_t nameLen = 0;
            while (nameLen < len && chunkData[nameLen] != 0) {
                nameLen++;
            }
            if (nameLen >= len || nameLen > 79) return false;

            outProfileName = std::string(reinterpret_cast<char*>(chunkData.data()), nameLen);

            if (nameLen + 2 >= len) return false;
            uint8_t compMethod = chunkData[nameLen + 1];
            if (compMethod != 0) return false;

            size_t compSize = len - (nameLen + 2);
            const uint8_t* compPtr = chunkData.data() + nameLen + 2;

            int decompSize = 0;
            char* decomp = stbi_zlib_decode_malloc(reinterpret_cast<const char*>(compPtr), static_cast<int>(compSize), &decompSize);
            if (decomp && decompSize > 0) {
                outIccData.assign(decomp, decomp + decompSize);
                free(decomp);
                return true;
            }
            return false;
        } else {
            file.seekg(len + 4, std::ios::cur);
        }
    }
    return false;
}

// ============================================================
// File Import / Export
// ============================================================

bool Canvas::ExtractAndSetICCProfile(const std::string& pngPath) {
    std::vector<uint8_t> iccData;
    std::string profileName;
    if (ExtractICCFromPNG(pngPath, iccData, profileName)) {
        std::string iccPath = pngPath;
        size_t dotPos = iccPath.find_last_of('.');
        if (dotPos != std::string::npos) {
            iccPath = iccPath.substr(0, dotPos) + ".icc";
        } else {
            iccPath += ".icc";
        }
#ifdef _WIN32
        std::ofstream outFile(UTF8ToWString(iccPath), std::ios::binary);
#else
        std::ofstream outFile(iccPath, std::ios::binary);
#endif
        if (outFile.is_open()) {
            outFile.write(reinterpret_cast<const char*>(iccData.data()), iccData.size());
            outFile.close();
            m_ExportPngColorSpace = iccPath;
            Logger::Get().Info("Extracted embedded ICC profile '" + profileName + "' to: " + iccPath);
            return true;
        }
    }

    // Fallback: check for next-to-image .icc or .icm files
    std::string base = pngPath;
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    if (std::filesystem::exists(base + ".icc")) {
        m_ExportPngColorSpace = base + ".icc";
        Logger::Get().Info("Found external ICC profile next to image: " + m_ExportPngColorSpace);
        return true;
    } else if (std::filesystem::exists(base + ".icm")) {
        m_ExportPngColorSpace = base + ".icm";
        Logger::Get().Info("Found external ICM profile next to image: " + m_ExportPngColorSpace);
        return true;
    }

    m_ExportPngColorSpace = "sRGB";
    return false;
}

bool Canvas::LoadImageToLayer(const std::string& filepath) {
    std::string ext;
    size_t dotPos = filepath.find_last_of('.');
    if (dotPos != std::string::npos) {
        ext = filepath.substr(dotPos + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    }

    int imgWidth = 0, imgHeight = 0;
    std::vector<uint8_t> loadedU8;
    DdsFormat loadedDdsFormat = DdsFormat::RGBA8_UNORM;
    std::unique_ptr<TileCache> loadedTileCache;

    if (ext == "dds") {
        loadedTileCache = std::make_unique<TileCache>();
        if (!DdsHelper::LoadDDSToTileCache(filepath, *loadedTileCache, imgWidth, imgHeight, loadedDdsFormat)) {
            return false;
        }

        if (loadedDdsFormat == DdsFormat::R8_UNORM || loadedDdsFormat == DdsFormat::R16_FLOAT || loadedDdsFormat == DdsFormat::R32_FLOAT) {
            m_ChannelR = true; m_ChannelG = false; m_ChannelB = false; m_ChannelA = false;
            Logger::Get().Info("Single-channel DDS detected. Auto-configured R-only channels.");
        }
    } else {
        if (!ImageManager::LoadImageFromFile(filepath, loadedU8, imgWidth, imgHeight)) return false;
    }

    std::string lowerPath = filepath;
    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower);
    if (lowerPath.find("normal") != std::string::npos ||
        lowerPath.find("nrm")    != std::string::npos ||
        lowerPath.find("bc5")    != std::string::npos) {
        m_ChannelR = true; m_ChannelG = true; m_ChannelB = false; m_ChannelA = false;
        Logger::Get().Info("Normal map detected. Auto-configured RG channels.");
    }

    // --- Detect if this is the first real image load ---
    bool isFirst = m_Layers.empty() ||
        (m_Layers.size() == 1 &&
         m_Layers[0].name == "Background" &&
         (!m_Layers[0].tileCache || m_Layers[0].tileCache->IsEmpty()));

    if (isFirst) {
        m_Width  = imgWidth;
        m_Height = imgHeight;
        m_CanvasFormat = CanvasPixelFormat::RGBA8;
        Logger::Get().Info("Canvas format: RGBA8");

        MarkCompositeResourcesDirty();
        m_Layers.clear();
        m_ProjectType        = ProjectType::Simple;
        m_CurrentProjectFilePath = filepath;
        m_ExportPath         = filepath;

        if (ext == "dds") {
            if (loadedDdsFormat == DdsFormat::RGBA32_FLOAT) m_ExportFormat = "RGBA32_FLOAT";
            else if (loadedDdsFormat == DdsFormat::RGBA16_UNORM) m_ExportFormat = "RGBA16_UNORM";
            else if (loadedDdsFormat == DdsFormat::RGBA16_FLOAT) m_ExportFormat = "RGBA16_FLOAT";
            else if (loadedDdsFormat == DdsFormat::R8_UNORM)     m_ExportFormat = "R8_UNORM";
            else if (loadedDdsFormat == DdsFormat::R16_FLOAT)    m_ExportFormat = "R16_FLOAT";
            else if (loadedDdsFormat == DdsFormat::R32_FLOAT)    m_ExportFormat = "R32_FLOAT";
            else m_ExportFormat = "RGBA8_UNORM";
            Logger::Get().Info("Auto-configured DDS export format: " + m_ExportFormat);
        } else if (ext == "png") {
            ExtractAndSetICCProfile(filepath);
        }
    }

    // --- Build Layer with TileCache ---
    Layer imported;
    imported.name    = filepath.substr(filepath.find_last_of("\\/") + 1);
    imported.visible = true;
    imported.opacity = 1.0f;
    if (loadedTileCache) {
        if (loadedTileCache->GetWidth() == m_Width && loadedTileCache->GetHeight() == m_Height) {
            imported.tileCache = std::move(loadedTileCache);
        } else {
            imported.tileCache = std::make_unique<TileCache>();
            imported.tileCache->Init(m_Width, m_Height, m_CanvasFormat);
            imported.tileCache->CopyFrom(*loadedTileCache, 0, 0, 0, 0, imgWidth, imgHeight);
        }
    } else {
        imported.tileCache = std::make_unique<TileCache>();
        imported.tileCache->Init(m_Width, m_Height, m_CanvasFormat);
        if (!loadedU8.empty()) {
            imported.tileCache->ImportRGBA8(loadedU8.data(), imgWidth, imgHeight);
        }
    }
    imported.tileCache->MarkAllDirty();

    imported.needsUpload = true;
    m_Layers.push_back(std::move(imported));
    m_ActiveLayerIdx = (int)m_Layers.size() - 1;
    m_CompositeDirty = true;

    ResetView();
    Logger::Get().Info("Successfully imported layer from: " + filepath);
    return true;
}

bool Canvas::SaveCanvas(const std::string& filepath, DdsFormat ddsFormat) {
    if (m_Layers.empty()) {
        Logger::Get().Error("No layers to save.");
        return false;
    }

    std::vector<float> composite = ComposeVisibleLayers(m_Layers, m_Width, m_Height);

    DdsImage dds;
    dds.width = m_Width;
    dds.height = m_Height;
    dds.format = ddsFormat;
    dds.pixels = std::move(composite);

    return DdsHelper::SaveDDS(filepath, dds);
}

bool Canvas::SaveCanvasStandard(const std::string& filepath, const std::string& iccProfilePath) {
    if (m_Layers.empty()) {
        Logger::Get().Error("No layers to save.");
        return false;
    }

    std::vector<float> composite = ComposeVisibleLayers(m_Layers, m_Width, m_Height);
    return ImageManager::SaveImageToFile(filepath, composite, m_Width, m_Height, iccProfilePath);
}

bool Canvas::SaveCanvasCompressed(const std::string& filepath, const std::string& formatStr, bool generateMips, const std::string& mipFilter, const std::string& speed) {
    DdsFormat ddsFmt;
    bool isNative = false;
    if (formatStr == "R8G8B8A8_UNORM" || formatStr == "RGBA8_UNORM") { ddsFmt = DdsFormat::RGBA8_UNORM; isNative = true; }
    else if (formatStr == "R16G16B16A16_UNORM" || formatStr == "RGBA16_UNORM") { ddsFmt = DdsFormat::RGBA16_UNORM; isNative = true; }
    else if (formatStr == "R16G16B16A16_FLOAT" || formatStr == "RGBA16_FLOAT") { ddsFmt = DdsFormat::RGBA16_FLOAT; isNative = true; }
    else if (formatStr == "R32G32B32A32_FLOAT" || formatStr == "RGBA32_FLOAT") { ddsFmt = DdsFormat::RGBA32_FLOAT; isNative = true; }
    else if (formatStr == "R8_UNORM") { ddsFmt = DdsFormat::R8_UNORM; isNative = true; }
    else if (formatStr == "R16_FLOAT") { ddsFmt = DdsFormat::R16_FLOAT; isNative = true; }
    else if (formatStr == "R32_FLOAT") { ddsFmt = DdsFormat::R32_FLOAT; isNative = true; }

    if (isNative) {
        return SaveCanvas(filepath, ddsFmt);
    }

    std::string tempDir = ConfigManager::GetUserSubdirectory("temp");
    std::string tempFile = tempDir + "/temp_export_uncompressed.dds";

    struct FileCleanupGuard {
        std::wstring path;
        ~FileCleanupGuard() {
            if (!path.empty()) {
                std::error_code ec;
                std::filesystem::remove(path, ec);
            }
        }
    } guard;
#ifdef _WIN32
    guard.path = UTF8ToWString(tempFile);
#else
    guard.path = std::wstring(tempFile.begin(), tempFile.end());
#endif

    if (!SaveCanvas(tempFile, DdsFormat::RGBA8_UNORM)) {
        Logger::Get().Error("Failed to save temporary uncompressed DDS for texconv.");
        return false;
    }

    ExportSettings settings;
    settings.isDds = true;
    settings.ddsFormatStr = formatStr;
    settings.advancedMode = true;
    settings.compressionSpeed = speed;
    settings.generateMipMaps = generateMips;
    settings.mipFilter = mipFilter;
    settings.exportPath = filepath;

    return TexconvHelper::CompressDDS(tempFile, filepath, settings);
}

std::vector<float> Canvas::GetCompositePixels() const {
    if (m_Layers.empty()) {
        return std::vector<float>((size_t)m_Width * m_Height * 4, 0.0f);
    }
    return ComposeVisibleLayers(m_Layers, m_Width, m_Height);
}

bool Canvas::SaveCanvasRayp(const std::string& filepath) {
    if (m_Layers.empty()) {
        Logger::Get().Error("No layers to save in RAYP.");
        return false;
    }

    try {
        // 1. Create JSON metadata
        json metadata;
        metadata["width"] = m_Width;
        metadata["height"] = m_Height;
        metadata["active_layer"] = m_ActiveLayerIdx;
        metadata["project_type"] = (m_ProjectType == ProjectType::Simple) ? "simple" : "advanced";
        
        metadata["export_path"] = m_ExportPath;
        metadata["export_format"] = m_ExportFormat;
        metadata["export_advanced_mode"] = m_ExportAdvancedMode;
        metadata["export_compression_speed"] = m_ExportCompressionSpeed;
        metadata["export_generate_mip_maps"] = m_ExportGenerateMipMaps;
        metadata["export_mip_filter"] = m_ExportMipFilter;
        metadata["export_png_color_space"] = m_ExportPngColorSpace;

        json layersArray = json::array();
        for (const auto& layer : m_Layers) {
            json layerJson;
            layerJson["name"] = layer.name;
            layerJson["visible"] = layer.visible;
            layerJson["opacity"] = layer.opacity;
            layersArray.push_back(layerJson);
        }
        metadata["layers"] = layersArray;

        std::string metadataStr = metadata.dump();

        // 2. Open binary file for writing
#ifdef _WIN32
        std::ofstream out(UTF8ToWString(filepath), std::ios::binary);
#else
        std::ofstream out(filepath, std::ios::binary);
#endif
        if (!out.is_open()) {
            Logger::Get().Error("Could not open file for saving RAYP: " + filepath);
            return false;
        }

        // Write Magic header
        out.write("RAYP", 4);
        
        // Write format version
        uint32_t version = 1;
        out.write(reinterpret_cast<const char*>(&version), sizeof(version));

        // Write Metadata size and content
        uint64_t metadataSize = metadataStr.size();
        out.write(reinterpret_cast<const char*>(&metadataSize), sizeof(metadataSize));
        out.write(metadataStr.data(), metadataStr.size());

        // 3. Compress and write pixel data for each layer
        for (auto& layer : m_Layers) {
            std::vector<float> layerPixels = ExportLayerF(layer, m_Width, m_Height);
            uint64_t uncompressedSize = layerPixels.size() * sizeof(float);
            
            int compSize = 0;
            unsigned char* compData = stbi_zlib_compress(
                reinterpret_cast<unsigned char*>(layerPixels.data()),
                static_cast<int>(uncompressedSize),
                &compSize,
                8 // good compression level
            );

            if (!compData) {
                Logger::Get().Error("Failed to compress layer data for " + layer.name);
                return false;
            }

            uint64_t compressedSize = compSize;
            out.write(reinterpret_cast<const char*>(&uncompressedSize), sizeof(uncompressedSize));
            out.write(reinterpret_cast<const char*>(&compressedSize), sizeof(compressedSize));
            out.write(reinterpret_cast<const char*>(compData), compressedSize);

            free(compData);
        }

        m_IsDocumentModified = false;
        m_CurrentProjectFilePath = filepath;
        Logger::Get().Info("Successfully saved project to " + filepath);
        return true;
    }
    catch (const std::exception& e) {
        Logger::Get().Error("Exception saving RAYP: " + std::string(e.what()));
        return false;
    }
}

bool Canvas::LoadCanvasRayp(const std::string& filepath) {
    try {
        // 1. Open binary file for reading
#ifdef _WIN32
        std::ifstream in(UTF8ToWString(filepath), std::ios::binary);
#else
        std::ifstream in(filepath, std::ios::binary);
#endif
        if (!in.is_open()) {
            Logger::Get().Error("Could not open file for loading RAYP: " + filepath);
            return false;
        }

        // Read Magic header
        char magic[4];
        in.read(magic, 4);
        if (std::strncmp(magic, "RAYP", 4) != 0) {
            Logger::Get().Error("Invalid RAYP magic signature.");
            return false;
        }

        // Read format version
        uint32_t version = 0;
        in.read(reinterpret_cast<char*>(&version), sizeof(version));
        if (version != 1) {
            Logger::Get().Error("Unsupported RAYP version: " + std::to_string(version));
            return false;
        }

        // Read Metadata size and content
        uint64_t metadataSize = 0;
        in.read(reinterpret_cast<char*>(&metadataSize), sizeof(metadataSize));
        
        std::string metadataStr;
        metadataStr.resize(metadataSize);
        in.read(&metadataStr[0], metadataSize);

        // Parse JSON metadata
        json metadata = json::parse(metadataStr);

        m_Layers.clear();

        m_Width = metadata["width"].get<int>();
        m_Height = metadata["height"].get<int>();
        m_ActiveLayerIdx = metadata["active_layer"].get<int>();
        m_CurrentProjectFilePath = filepath;
        m_CanvasFormat = CanvasPixelFormat::RGBA8;

        if (metadata.contains("project_type")) {
            std::string pt = metadata["project_type"].get<std::string>();
            m_ProjectType = (pt == "simple") ? ProjectType::Simple : ProjectType::Advanced;
        } else {
            m_ProjectType = ProjectType::Advanced;
        }

        if (metadata.contains("export_path")) m_ExportPath = metadata["export_path"].get<std::string>();
        if (metadata.contains("export_format")) m_ExportFormat = metadata["export_format"].get<std::string>();
        if (metadata.contains("export_advanced_mode")) m_ExportAdvancedMode = metadata["export_advanced_mode"].get<bool>();
        if (metadata.contains("export_compression_speed")) m_ExportCompressionSpeed = metadata["export_compression_speed"].get<std::string>();
        if (metadata.contains("export_generate_mip_maps")) m_ExportGenerateMipMaps = metadata["export_generate_mip_maps"].get<bool>();
        if (metadata.contains("export_mip_filter")) m_ExportMipFilter = metadata["export_mip_filter"].get<std::string>();
        if (metadata.contains("export_png_color_space")) m_ExportPngColorSpace = metadata["export_png_color_space"].get<std::string>();

        MarkCompositeResourcesDirty();

        auto layersArray = metadata["layers"];
        for (size_t idx = 0; idx < layersArray.size(); ++idx) {
            auto layerJson = layersArray[idx];
            Layer layer;
            layer.name = layerJson["name"].get<std::string>();
            layer.visible = layerJson["visible"].get<bool>();
            layer.opacity = layerJson["opacity"].get<float>();
            
            // Read uncompressed and compressed size
            uint64_t uncompressedSize = 0;
            uint64_t compressedSize = 0;
            in.read(reinterpret_cast<char*>(&uncompressedSize), sizeof(uncompressedSize));
            in.read(reinterpret_cast<char*>(&compressedSize), sizeof(compressedSize));

            std::vector<uint8_t> compressedBytes(compressedSize);
            in.read(reinterpret_cast<char*>(compressedBytes.data()), compressedSize);

            // Decompress using stbi_zlib_decode_malloc
            int decompSize = 0;
            char* decompData = stbi_zlib_decode_malloc(
                reinterpret_cast<const char*>(compressedBytes.data()),
                static_cast<int>(compressedSize),
                &decompSize
            );

            if (!decompData || static_cast<size_t>(decompSize) != uncompressedSize) {
                Logger::Get().Error("Failed to decompress layer data for " + layer.name);
                if (decompData) free(decompData);
                return false;
            }

            std::vector<float> layerPixels(uncompressedSize / sizeof(float));
            std::memcpy(layerPixels.data(), decompData, uncompressedSize);
            free(decompData);

            layer.tileCache = std::make_unique<TileCache>();
            layer.tileCache->Init(m_Width, m_Height, m_CanvasFormat);
            layer.tileCache->ImportRGBA32F(layerPixels.data(), m_Width, m_Height);
            layer.tileCache->MarkAllDirty();
            layer.needsUpload = true;

            m_Layers.push_back(std::move(layer));
        }
        m_CompositeDirty = true;

        m_UndoRedoManager.Clear();
        m_IsDocumentModified = false;
        m_CurrentProjectFilePath = filepath;
        Logger::Get().Info("Successfully loaded project from " + filepath);
        return true;
    }
    catch (const std::exception& e) {
        Logger::Get().Error("Exception loading RAYP: " + std::string(e.what()));
        return false;
    }
}

static bool SaveCanvasRaypInternal(const std::string& filepath, int width, int height, int activeLayerIdx,
                                  const std::vector<std::string>& layerNames,
                                  const std::vector<bool>& layerVisibles,
                                  const std::vector<float>& layerOpacities,
                                  const std::vector<std::vector<float>>& layerPixels,
                                  const std::string& exportPath,
                                  const std::string& exportFormat,
                                  bool exportAdvancedMode,
                                  const std::string& exportCompressionSpeed,
                                  bool exportGenerateMipMaps,
                                  const std::string& exportMipFilter,
                                  const std::string& exportPngColorSpace) {
    try {
        json metadata;
        metadata["width"] = width;
        metadata["height"] = height;
        metadata["active_layer"] = activeLayerIdx;

        metadata["export_path"] = exportPath;
        metadata["export_format"] = exportFormat;
        metadata["export_advanced_mode"] = exportAdvancedMode;
        metadata["export_compression_speed"] = exportCompressionSpeed;
        metadata["export_generate_mip_maps"] = exportGenerateMipMaps;
        metadata["export_mip_filter"] = exportMipFilter;
        metadata["export_png_color_space"] = exportPngColorSpace;

        json layersArray = json::array();
        for (size_t i = 0; i < layerNames.size(); ++i) {
            json layerJson;
            layerJson["name"] = layerNames[i];
            layerJson["visible"] = layerVisibles[i];
            layerJson["opacity"] = layerOpacities[i];
            layersArray.push_back(layerJson);
        }
        metadata["layers"] = layersArray;

        std::string metadataStr = metadata.dump();

#ifdef _WIN32
        std::ofstream out(UTF8ToWString(filepath), std::ios::binary);
#else
        std::ofstream out(filepath, std::ios::binary);
#endif
        if (!out.is_open()) {
            return false;
        }

        out.write("RAYP", 4);
        uint32_t version = 1;
        out.write(reinterpret_cast<const char*>(&version), sizeof(version));

        uint64_t metadataSize = metadataStr.size();
        out.write(reinterpret_cast<const char*>(&metadataSize), sizeof(metadataSize));
        out.write(metadataStr.data(), metadataStr.size());

        for (size_t i = 0; i < layerPixels.size(); ++i) {
            const auto& pixels = layerPixels[i];
            uint64_t uncompressedSize = pixels.size() * sizeof(float);
            
            int compSize = 0;
            unsigned char* compData = stbi_zlib_compress(
                reinterpret_cast<unsigned char*>(const_cast<float*>(pixels.data())),
                static_cast<int>(uncompressedSize),
                &compSize,
                8
            );

            if (!compData) {
                return false;
            }

            uint64_t compressedSize = compSize;
            out.write(reinterpret_cast<const char*>(&uncompressedSize), sizeof(uncompressedSize));
            out.write(reinterpret_cast<const char*>(&compressedSize), sizeof(compressedSize));
            out.write(reinterpret_cast<const char*>(compData), compressedSize);

            free(compData);
        }
        return true;
    }
    catch (...) {
        return false;
    }
}

void Canvas::SaveCanvasRaypAsync(const std::string& filepath, std::function<void(bool)> callback) {
    int width = m_Width;
    int height = m_Height;
    int activeLayer = m_ActiveLayerIdx;
    
    std::vector<std::string> names;
    std::vector<bool> visibles;
    std::vector<float> opacities;
    std::vector<std::vector<float>> pixels;
    
    names.reserve(m_Layers.size());
    visibles.reserve(m_Layers.size());
    opacities.reserve(m_Layers.size());
    pixels.reserve(m_Layers.size());
    
    for (size_t i = 0; i < m_Layers.size(); ++i) {
        const auto& layer = m_Layers[i];
        names.push_back(layer.name);
        visibles.push_back(layer.visible);
        opacities.push_back(layer.opacity);
        pixels.push_back(ExportLayerF(layer, width, height));
    }

    std::string expPath = m_ExportPath;
    std::string expFormat = m_ExportFormat;
    bool expAdv = m_ExportAdvancedMode;
    std::string expSpeed = m_ExportCompressionSpeed;
    bool expMips = m_ExportGenerateMipMaps;
    std::string expMipF = m_ExportMipFilter;
    std::string expPngCS = m_ExportPngColorSpace;
    
    std::thread([=, pixels = std::move(pixels)]() {
        bool success = SaveCanvasRaypInternal(filepath, width, height, activeLayer, names, visibles, opacities, pixels,
                                             expPath, expFormat, expAdv, expSpeed, expMips, expMipF, expPngCS);
        if (callback) {
            callback(success);
        }
    }).detach();
}

bool Canvas::SaveProjectAuto() {
    if (m_CurrentProjectFilePath.empty()) {
        Logger::Get().Error("Cannot auto-save: current project file path is empty.");
        return false;
    }

    if (m_ProjectType == ProjectType::Simple) {
        std::string path = m_CurrentProjectFilePath;
        size_t dot = path.find_last_of('.');
        std::string ext = "";
        if (dot != std::string::npos) {
            ext = path.substr(dot + 1);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        }

        bool success = false;
        if (ext == "dds") {
            success = SaveCanvasCompressed(path, m_ExportFormat, m_ExportGenerateMipMaps, m_ExportMipFilter, m_ExportCompressionSpeed);
        } else {
            std::string icc = m_ExportPngColorSpace;
            success = SaveCanvasStandard(path, icc == "sRGB" ? "" : icc);
        }

        if (success) {
            m_IsDocumentModified = false;
            Logger::Get().Info("Simple project saved back to source image: " + path);
        } else {
            Logger::Get().Error("Failed to save simple project back to: " + path);
        }
        return success;
    } else {
        bool success = SaveCanvasRayp(m_CurrentProjectFilePath);
        if (success) {
            m_IsDocumentModified = false;
            Logger::Get().Info("Advanced project saved to RAYP package: " + m_CurrentProjectFilePath);
        } else {
            Logger::Get().Error("Failed to save advanced project to RAYP package: " + m_CurrentProjectFilePath);
        }
        return success;
    }
}
