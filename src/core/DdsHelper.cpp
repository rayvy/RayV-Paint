#include "DdsHelper.h"
#include "DdsCodec.h"
#include "TileCache.h"
#include "HalfFloat.h"
#include "Logger.h"
#include "MemoryStats.h"
#include "PathUtil.h"
#include <fstream>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <vector>
#include <chrono>
#include <cmath>
#include <limits>
#include <string>


#define BCDEC_IMPLEMENTATION
#include "bcdec.h"


#ifndef MAKEFOURCC
    #define MAKEFOURCC(ch0, ch1, ch2, ch3) \
                ((uint32_t)(uint8_t)(ch0) | ((uint32_t)(uint8_t)(ch1) << 8) | \
                ((uint32_t)(uint8_t)(ch2) << 16) | ((uint32_t)(uint8_t)(ch3) << 24 ))
#endif

#include <dxgiformat.h>

struct DDS_PIXELFORMAT {
    uint32_t dwSize;
    uint32_t dwFlags;
    uint32_t dwFourCC;
    uint32_t dwRGBBitCount;
    uint32_t dwRBitMask;
    uint32_t dwGBitMask;
    uint32_t dwBBitMask;
    uint32_t dwABitMask;
};

struct DDS_HEADER {
    uint32_t        dwSize;
    uint32_t        dwFlags;
    uint32_t        dwHeight;
    uint32_t        dwWidth;
    uint32_t        dwPitchOrLinearSize;
    uint32_t        dwDepth;
    uint32_t        dwMipMapCount;
    uint32_t        dwReserved1[11];
    DDS_PIXELFORMAT ddspf;
    uint32_t        dwCaps;
    uint32_t        dwCaps2;
    uint32_t        dwCaps3;
    uint32_t        dwCaps4;
    uint32_t        dwReserved2;
};

struct DDS_HEADER_DXT10 {
    uint32_t dxgiFormat;
    uint32_t resourceDimension;
    uint32_t miscFlag;
    uint32_t arraySize;
    uint32_t miscFlags2;
};

static uint8_t FloatToU8(float v) {
    return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// DXGI_FORMAT_R11G11B10_FLOAT (26) — packed RGB float, no sign bit
static void DecodeR11G11B10Float(uint32_t p, float& r, float& g, float& b) {
    auto decodeChannel = [](uint32_t raw, int mantBits, int expBits) -> float {
        const uint32_t mantMask = (1u << mantBits) - 1u;
        const uint32_t expMask  = (1u << expBits) - 1u;
        uint32_t m = raw & mantMask;
        uint32_t e = (raw >> mantBits) & expMask;
        if (e == 0) {
            if (m == 0) return 0.f;
            // subnormal
            float f = (float)m / (float)(1u << mantBits);
            return std::ldexp(f, 1 - ((int)(1u << (expBits - 1)) - 1));
        }
        if (e == expMask) {
            return m ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
        }
        int bias = (1 << (expBits - 1)) - 1;
        float fmant = 1.f + (float)m / (float)(1u << mantBits);
        return std::ldexp(fmant, (int)e - bias);
    };
    r = decodeChannel(p & 0x7FFu, 6, 5);
    g = decodeChannel((p >> 11) & 0x7FFu, 6, 5);
    b = decodeChannel((p >> 22) & 0x3FFu, 5, 5);
}

static float HalfToFloat(uint16_t h) { return HalfFloat::ToFloat(h); }

bool DdsHelper::LoadDDS(const std::string& filename, DdsImage& outImage) {
    // filesystem::path + wide open — UTF-8/Cyrillic safe on Windows
    std::ifstream file(PathUtil::FromUtf8(PathUtil::NormalizeToUtf8Path(filename)), std::ios::binary);
    if (!file.is_open()) {
        // ACP fallback path
        file.open(PathUtil::FromUtf8(filename), std::ios::binary);
    }
    if (!file.is_open()) {
        Logger::Get().Error("Failed to open DDS file for reading: " + filename);
        return false;
    }

    // Read Magic number
    uint32_t magic = 0;
    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (magic != MAKEFOURCC('D', 'D', 'S', ' ')) {
        Logger::Get().Error("Invalid DDS magic number in file: " + filename);
        return false;
    }

    // Read header
    DDS_HEADER header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (header.dwSize != 124 || header.ddspf.dwSize != 32) {
        Logger::Get().Error("Invalid DDS header size in file: " + filename);
        return false;
    }

    outImage.width = header.dwWidth;
    outImage.height = header.dwHeight;
    size_t numPixels = (size_t)outImage.width * outImage.height;
    outImage.pixels.resize(numPixels * 4);

    bool isDX10 = (header.ddspf.dwFlags & 0x4) && (header.ddspf.dwFourCC == MAKEFOURCC('D', 'X', '1', '0'));
    
    enum class CompressionFormat {
        None,
        BC1,
        BC2,
        BC3,
        BC4,
        BC5,
        BC7
    };
    
    CompressionFormat compFormat = CompressionFormat::None;
    size_t blockSize = 0;
    bool isFloatFormat = false;

    if (isDX10) {
        DDS_HEADER_DXT10 dxt10Header;
        file.read(reinterpret_cast<char*>(&dxt10Header), sizeof(dxt10Header));

        if (dxt10Header.dxgiFormat == DXGI_FORMAT_R32G32B32A32_FLOAT) {
            isFloatFormat = true;
        } 
        else if (dxt10Header.dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM) {
            // normal uncompressed
        }
        else if (dxt10Header.dxgiFormat == 71 || dxt10Header.dxgiFormat == 72) { // BC1
            compFormat = CompressionFormat::BC1;
            blockSize = 8;
        }
        else if (dxt10Header.dxgiFormat == 74 || dxt10Header.dxgiFormat == 75) { // BC2
            compFormat = CompressionFormat::BC2;
            blockSize = 16;
        }
        else if (dxt10Header.dxgiFormat == 77 || dxt10Header.dxgiFormat == 78) { // BC3
            compFormat = CompressionFormat::BC3;
            blockSize = 16;
        }
        else if (dxt10Header.dxgiFormat == 80 || dxt10Header.dxgiFormat == 81) { // BC4
            compFormat = CompressionFormat::BC4;
            blockSize = 8;
        }
        else if (dxt10Header.dxgiFormat == 83 || dxt10Header.dxgiFormat == 84) { // BC5
            compFormat = CompressionFormat::BC5;
            blockSize = 16;
        }
        else if (dxt10Header.dxgiFormat == 98 || dxt10Header.dxgiFormat == 99) { // BC7
            compFormat = CompressionFormat::BC7;
            blockSize = 16;
        }
        else {
            Logger::Get().Error("Unsupported DXGI format " + std::to_string(dxt10Header.dxgiFormat) + " in DDS file: " + filename);
            return false;
        }

        if (isFloatFormat) {
            outImage.format = DdsFormat::RGBA32_FLOAT;
            file.read(reinterpret_cast<char*>(outImage.pixels.data()), numPixels * 16);
            if (!file) {
                Logger::Get().Error("Failed reading R32G32B32A32_FLOAT pixel data from: " + filename);
                return false;
            }
        }
        else if (compFormat == CompressionFormat::None) {
            outImage.format = DdsFormat::RGBA8_UNORM;
            std::vector<uint8_t> rawBytes(numPixels * 4);
            file.read(reinterpret_cast<char*>(rawBytes.data()), numPixels * 4);
            if (!file) {
                Logger::Get().Error("Failed reading R8G8B8A8_UNORM pixel data from: " + filename);
                return false;
            }
            for (size_t i = 0; i < numPixels * 4; ++i) {
                outImage.pixels[i] = rawBytes[i] / 255.0f;
            }
        }
    } 
    else {
        // Legacy DDS loader
        bool isFourCC = (header.ddspf.dwFlags & 0x4);
        if (isFourCC) {
            uint32_t fourCC = header.ddspf.dwFourCC;
            if (fourCC == MAKEFOURCC('D', 'X', 'T', '1')) {
                compFormat = CompressionFormat::BC1;
                blockSize = 8;
            } else if (fourCC == MAKEFOURCC('D', 'X', 'T', '3')) {
                compFormat = CompressionFormat::BC2;
                blockSize = 16;
            } else if (fourCC == MAKEFOURCC('D', 'X', 'T', '5')) {
                compFormat = CompressionFormat::BC3;
                blockSize = 16;
            } else if (fourCC == MAKEFOURCC('A', 'T', 'I', '1') || fourCC == MAKEFOURCC('B', 'C', '4', 'U')) {
                compFormat = CompressionFormat::BC4;
                blockSize = 8;
            } else if (fourCC == MAKEFOURCC('A', 'T', 'I', '2') || fourCC == MAKEFOURCC('B', 'C', '5', 'U')) {
                compFormat = CompressionFormat::BC5;
                blockSize = 16;
            } else {
                Logger::Get().Error("Unsupported legacy FourCC format in DDS file: " + filename);
                return false;
            }
        }
        else {
            bool isRGBA = (header.ddspf.dwFlags & 0x40) || (header.ddspf.dwFlags & 0x41);
            if (isRGBA && header.ddspf.dwRGBBitCount == 32) {
                outImage.format = DdsFormat::RGBA8_UNORM;
                std::vector<uint8_t> rawBytes(numPixels * 4);
                file.read(reinterpret_cast<char*>(rawBytes.data()), numPixels * 4);
                if (!file) {
                    Logger::Get().Error("Failed reading legacy uncompressed 32-bit pixel data from: " + filename);
                    return false;
                }

                bool isBGRA = (header.ddspf.dwRBitMask == 0x00ff0000);
                
                for (size_t i = 0; i < numPixels; ++i) {
                    size_t base = i * 4;
                    if (isBGRA) {
                        outImage.pixels[base + 0] = rawBytes[base + 2] / 255.0f; // R
                        outImage.pixels[base + 1] = rawBytes[base + 1] / 255.0f; // G
                        outImage.pixels[base + 2] = rawBytes[base + 0] / 255.0f; // B
                        outImage.pixels[base + 3] = rawBytes[base + 3] / 255.0f; // A
                    } else {
                        outImage.pixels[base + 0] = rawBytes[base + 0] / 255.0f; // R
                        outImage.pixels[base + 1] = rawBytes[base + 1] / 255.0f; // G
                        outImage.pixels[base + 2] = rawBytes[base + 2] / 255.0f; // B
                        outImage.pixels[base + 3] = rawBytes[base + 3] / 255.0f; // A
                    }
                }
            } 
            else {
                Logger::Get().Error("Unsupported legacy DDS format. Only uncompressed 32-bit RGBA/BGRA is supported natively in: " + filename);
                return false;
            }
        }
    }

    if (compFormat != CompressionFormat::None) {
        int blocksW = (outImage.width + 3) / 4;
        int blocksH = (outImage.height + 3) / 4;
        size_t dataSize = (size_t)blocksW * blocksH * blockSize;

        std::vector<uint8_t> compressedData(dataSize);
        file.read(reinterpret_cast<char*>(compressedData.data()), dataSize);
        if (!file) {
            Logger::Get().Error("Failed to read compressed pixel data from: " + filename);
            return false;
        }

        if (compFormat == CompressionFormat::BC1 || compFormat == CompressionFormat::BC2 ||
            compFormat == CompressionFormat::BC3 || compFormat == CompressionFormat::BC7) {
            
            std::vector<uint8_t> decompressedRGBA8((size_t)blocksW * 4 * blocksH * 4 * 4, 0);
            
            for (int by = 0; by < blocksH; ++by) {
                for (int bx = 0; bx < blocksW; ++bx) {
                    size_t blockIdx = (size_t)by * blocksW + bx;
                    const uint8_t* blockSrc = compressedData.data() + blockIdx * blockSize;
                    uint8_t* blockDst = decompressedRGBA8.data() + ((size_t)by * 4 * blocksW * 4 + bx * 4) * 4;
                    
                    if (compFormat == CompressionFormat::BC1) {
                        bcdec_bc1(blockSrc, blockDst, blocksW * 4 * 4);
                    } else if (compFormat == CompressionFormat::BC2) {
                        bcdec_bc2(blockSrc, blockDst, blocksW * 4 * 4);
                    } else if (compFormat == CompressionFormat::BC3) {
                        bcdec_bc3(blockSrc, blockDst, blocksW * 4 * 4);
                    } else if (compFormat == CompressionFormat::BC7) {
                        bcdec_bc7(blockSrc, blockDst, blocksW * 4 * 4);
                    }
                }
            }
            
            outImage.format = DdsFormat::RGBA8_UNORM;
            for (int y = 0; y < outImage.height; ++y) {
                for (int x = 0; x < outImage.width; ++x) {
                    size_t srcIndex = ((size_t)y * blocksW * 4 + x) * 4;
                    size_t dstIndex = ((size_t)y * outImage.width + x) * 4;
                    outImage.pixels[dstIndex + 0] = decompressedRGBA8[srcIndex + 0] / 255.0f;
                    outImage.pixels[dstIndex + 1] = decompressedRGBA8[srcIndex + 1] / 255.0f;
                    outImage.pixels[dstIndex + 2] = decompressedRGBA8[srcIndex + 2] / 255.0f;
                    outImage.pixels[dstIndex + 3] = decompressedRGBA8[srcIndex + 3] / 255.0f;
                }
            }
        }
        else if (compFormat == CompressionFormat::BC4) {
            std::vector<uint8_t> decompressedR8((size_t)blocksW * 4 * blocksH * 4, 0);
            
            for (int by = 0; by < blocksH; ++by) {
                for (int bx = 0; bx < blocksW; ++bx) {
                    size_t blockIdx = (size_t)by * blocksW + bx;
                    const uint8_t* blockSrc = compressedData.data() + blockIdx * blockSize;
                    uint8_t* blockDst = decompressedR8.data() + ((size_t)by * 4 * blocksW * 4 + bx * 4);
                    
                    bcdec_bc4(blockSrc, blockDst, blocksW * 4);
                }
            }
            
            outImage.format = DdsFormat::RGBA8_UNORM;
            for (int y = 0; y < outImage.height; ++y) {
                for (int x = 0; x < outImage.width; ++x) {
                    size_t srcIndex = (size_t)y * blocksW * 4 + x;
                    size_t dstIndex = ((size_t)y * outImage.width + x) * 4;
                    float val = decompressedR8[srcIndex] / 255.0f;
                    outImage.pixels[dstIndex + 0] = val;
                    outImage.pixels[dstIndex + 1] = val;
                    outImage.pixels[dstIndex + 2] = val;
                    outImage.pixels[dstIndex + 3] = 1.0f;
                }
            }
        }
        else if (compFormat == CompressionFormat::BC5) {
            std::vector<uint8_t> decompressedRG8((size_t)blocksW * 4 * blocksH * 4 * 2, 0);
            
            for (int by = 0; by < blocksH; ++by) {
                for (int bx = 0; bx < blocksW; ++bx) {
                    size_t blockIdx = (size_t)by * blocksW + bx;
                    const uint8_t* blockSrc = compressedData.data() + blockIdx * blockSize;
                    uint8_t* blockDst = decompressedRG8.data() + ((size_t)by * 4 * blocksW * 4 + bx * 4) * 2;
                    
                    bcdec_bc5(blockSrc, blockDst, blocksW * 4 * 2);
                }
            }
            
            outImage.format = DdsFormat::RGBA8_UNORM;
            for (int y = 0; y < outImage.height; ++y) {
                for (int x = 0; x < outImage.width; ++x) {
                    size_t srcIndex = ((size_t)y * blocksW * 4 + x) * 2;
                    size_t dstIndex = ((size_t)y * outImage.width + x) * 4;
                    outImage.pixels[dstIndex + 0] = decompressedRG8[srcIndex + 0] / 255.0f;
                    outImage.pixels[dstIndex + 1] = decompressedRG8[srcIndex + 1] / 255.0f;
                    outImage.pixels[dstIndex + 2] = 0.0f;
                    outImage.pixels[dstIndex + 3] = 1.0f;
                }
            }
        }
    }

    Logger::Get().Info("DDS texture loaded successfully (" + std::to_string(outImage.width) + "x" + std::to_string(outImage.height) + "): " + filename);
    return true;
}

// Map DirectXTex SourceInfo → legacy DdsFormat for Canvas channel heuristics.
static DdsFormat LegacyFromSource(const DdsCodec::SourceInfo& s) {
    if (s.singleChannel) {
        if (s.dxgi == DXGI_FORMAT_R32_FLOAT) return DdsFormat::R32_FLOAT;
        if (s.dxgi == DXGI_FORMAT_R16_FLOAT) return DdsFormat::R16_FLOAT;
        return DdsFormat::R8_UNORM;
    }
    if (s.dualChannel) return DdsFormat::R8G8_UNORM;
    if (s.suggestedDepth >= 2) return DdsFormat::RGBA32_FLOAT;
    if (s.suggestedDepth == 1) {
        if (s.dxgi == DXGI_FORMAT_R16G16B16A16_UNORM) return DdsFormat::RGBA16_UNORM;
        return DdsFormat::RGBA16_FLOAT;
    }
    return DdsFormat::RGBA8_UNORM;
}

bool DdsHelper::LoadDDSToTileCache(const std::string& filename, TileCache& outCache, int& outWidth, int& outHeight, DdsFormat& outFormat) {
    Logger::Get().InfoTag("io", "LoadDDSToTileCache (DirectXTex) begin: " + filename);
    MemoryStats::LogSnapshot("dds_open_start");

    DdsCodec::SourceInfo info;
    if (DdsCodec::LoadToTileCache(filename, outCache, outWidth, outHeight, info)) {
        outFormat = LegacyFromSource(info);
        return true;
    }
    // Fall through to legacy path if DirectXTex rejects the file
    Logger::Get().WarnTag("io", "DirectXTex load failed — trying legacy DdsHelper path");
    auto loadStart = std::chrono::high_resolution_clock::now();

    std::ifstream file(PathUtil::FromUtf8(PathUtil::NormalizeToUtf8Path(filename)), std::ios::binary);
    if (!file.is_open())
        file.open(PathUtil::FromUtf8(filename), std::ios::binary);
    if (!file.is_open()) {
        Logger::Get().Error("Failed to open DDS file for reading: " + filename);
        return false;
    }

    uint32_t magic = 0;
    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (magic != MAKEFOURCC('D', 'D', 'S', ' ')) {
        Logger::Get().Error("Invalid DDS magic number in file: " + filename);
        return false;
    }

    DDS_HEADER header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (header.dwSize != 124 || header.ddspf.dwSize != 32) {
        Logger::Get().Error("Invalid DDS header size in file: " + filename);
        return false;
    }

    outWidth = static_cast<int>(header.dwWidth);
    outHeight = static_cast<int>(header.dwHeight);
    Logger::Get().InfoTag("io",
        "DDS header " + std::to_string(outWidth) + "x" + std::to_string(outHeight) +
        " estRGBA8=" + MemoryStats::FormatBytes(MemoryStats::EstimateImageBytes(outWidth, outHeight, 4)));

    bool isDX10 = (header.ddspf.dwFlags & 0x4) && (header.ddspf.dwFourCC == MAKEFOURCC('D', 'X', '1', '0'));

    enum class CompressionFormat {
        None,
        BC1,
        BC2,
        BC3,
        BC4,
        BC5,
        BC7
    };

    CompressionFormat compFormat = CompressionFormat::None;
    size_t blockSize = 0;
    uint32_t dxgiFormat = 0;
    // Load storage: U8 default; F16 for half/HDR mid; F32 only for full float sources.
    CanvasPixelFormat loadStorage = CanvasPixelFormat::RGBA8;

    if (isDX10) {
        DDS_HEADER_DXT10 dxt10Header;
        file.read(reinterpret_cast<char*>(&dxt10Header), sizeof(dxt10Header));
        dxgiFormat = dxt10Header.dxgiFormat;

        if (dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM || dxgiFormat == 28 /*R8G8B8A8_UNORM*/ ||
            dxgiFormat == 29 /*R8G8B8A8_UNORM_SRGB*/) {
            outFormat = DdsFormat::RGBA8_UNORM;
        } else if (dxgiFormat == DXGI_FORMAT_R32G32B32A32_FLOAT || dxgiFormat == 2) {
            outFormat = DdsFormat::RGBA32_FLOAT;
            loadStorage = CanvasPixelFormat::RGBA32F;
        } else if (dxgiFormat == DXGI_FORMAT_R16G16B16A16_FLOAT || dxgiFormat == 10) {
            outFormat = DdsFormat::RGBA16_FLOAT;
            loadStorage = CanvasPixelFormat::RGBA16F;
        } else if (dxgiFormat == DXGI_FORMAT_R16G16B16A16_UNORM || dxgiFormat == 11) {
            outFormat = DdsFormat::RGBA16_UNORM;
        } else if (dxgiFormat == DXGI_FORMAT_R32_FLOAT || dxgiFormat == 41) {
            outFormat = DdsFormat::R32_FLOAT;
            loadStorage = CanvasPixelFormat::RGBA32F;
        } else if (dxgiFormat == DXGI_FORMAT_R16_FLOAT || dxgiFormat == 54) {
            outFormat = DdsFormat::R16_FLOAT;
            loadStorage = CanvasPixelFormat::RGBA16F;
        } else if (dxgiFormat == DXGI_FORMAT_R8_UNORM || dxgiFormat == 61) {
            outFormat = DdsFormat::R8_UNORM;
        } else if (dxgiFormat == 26) { // R11G11B10_FLOAT
            outFormat = DdsFormat::RGBA16_FLOAT;
            loadStorage = CanvasPixelFormat::RGBA16F;
        } else if (dxgiFormat == 20 || dxgiFormat == 40) {
            // D32_FLOAT_S8X24_UINT (20) / D32_FLOAT (40) — match DirectXTex/PDN convert:
            // Depth → RGB grayscale, Stencil → Alpha. Stay U8 (no R32 project).
            outFormat = DdsFormat::RGBA8_UNORM;
            loadStorage = CanvasPixelFormat::RGBA8;
        } else if (dxgiFormat == 49 || dxgiFormat == 50) { // R8G8_UNORM / R8G8_SNORM
            outFormat = DdsFormat::R8G8_UNORM;
        } else if (dxgiFormat == 24) { // R10G10B10A2_UNORM
            outFormat = DdsFormat::RGBA8_UNORM; // expand 10-bit → 8 for now; full 10-bit later
        } else if (dxgiFormat == 71 || dxgiFormat == 72) {
            compFormat = CompressionFormat::BC1;
            blockSize = 8;
            outFormat = DdsFormat::RGBA8_UNORM;
        } else if (dxgiFormat == 74 || dxgiFormat == 75) {
            compFormat = CompressionFormat::BC2;
            blockSize = 16;
            outFormat = DdsFormat::RGBA8_UNORM;
        } else if (dxgiFormat == 77 || dxgiFormat == 78) {
            compFormat = CompressionFormat::BC3;
            blockSize = 16;
            outFormat = DdsFormat::RGBA8_UNORM;
        } else if (dxgiFormat == 80 || dxgiFormat == 81) {
            compFormat = CompressionFormat::BC4;
            blockSize = 8;
            outFormat = DdsFormat::R8_UNORM;
        } else if (dxgiFormat == 83 || dxgiFormat == 84) {
            compFormat = CompressionFormat::BC5;
            blockSize = 16;
            outFormat = DdsFormat::RGBA8_UNORM;
        } else if (dxgiFormat == 95 || dxgiFormat == 96) { // BC6H
            Logger::Get().Error("BC6H open not yet decoded (use texconv preprocess). DXGI=" + std::to_string(dxgiFormat));
            return false;
        } else if (dxgiFormat == 98 || dxgiFormat == 99) {
            compFormat = CompressionFormat::BC7;
            blockSize = 16;
            outFormat = DdsFormat::RGBA8_UNORM;
        } else {
            Logger::Get().Error("Unsupported DXGI format " + std::to_string(dxgiFormat) + " in DDS file: " + filename);
            return false;
        }
    } else {
        bool isFourCC = (header.ddspf.dwFlags & 0x4);
        if (isFourCC) {
            uint32_t fourCC = header.ddspf.dwFourCC;
            if (fourCC == MAKEFOURCC('D', 'X', 'T', '1')) {
                compFormat = CompressionFormat::BC1;
                blockSize = 8;
                outFormat = DdsFormat::RGBA8_UNORM;
            } else if (fourCC == MAKEFOURCC('D', 'X', 'T', '3')) {
                compFormat = CompressionFormat::BC2;
                blockSize = 16;
                outFormat = DdsFormat::RGBA8_UNORM;
            } else if (fourCC == MAKEFOURCC('D', 'X', 'T', '5')) {
                compFormat = CompressionFormat::BC3;
                blockSize = 16;
                outFormat = DdsFormat::RGBA8_UNORM;
            } else if (fourCC == MAKEFOURCC('A', 'T', 'I', '1') || fourCC == MAKEFOURCC('B', 'C', '4', 'U')) {
                compFormat = CompressionFormat::BC4;
                blockSize = 8;
                outFormat = DdsFormat::R8_UNORM;
            } else if (fourCC == MAKEFOURCC('A', 'T', 'I', '2') || fourCC == MAKEFOURCC('B', 'C', '5', 'U')) {
                compFormat = CompressionFormat::BC5;
                blockSize = 16;
                outFormat = DdsFormat::RGBA8_UNORM;
            } else if (fourCC == 111) {
                // D3DFMT_R16F — NVIDIA/Photoshop DDS plugin, D3DX
                outFormat = DdsFormat::R16_FLOAT;
                loadStorage = CanvasPixelFormat::RGBA16F;
                Logger::Get().InfoTag("io", "Legacy FourCC D3DFMT_R16F (111)");
            } else if (fourCC == 112) {
                // D3DFMT_G16R16F → expand to RG float
                outFormat = DdsFormat::RGBA16_FLOAT;
                loadStorage = CanvasPixelFormat::RGBA16F;
                dxgiFormat = 34; // R16G16_FLOAT (reuse decode helper below via outFormat)
                Logger::Get().InfoTag("io", "Legacy FourCC D3DFMT_G16R16F (112)");
            } else if (fourCC == 113) {
                // D3DFMT_A16B16G16R16F
                outFormat = DdsFormat::RGBA16_FLOAT;
                loadStorage = CanvasPixelFormat::RGBA16F;
                Logger::Get().InfoTag("io", "Legacy FourCC D3DFMT_A16B16G16R16F (113)");
            } else if (fourCC == 114) {
                // D3DFMT_R32F — common PS / NVIDIA Texture Tools height maps
                outFormat = DdsFormat::R32_FLOAT;
                loadStorage = CanvasPixelFormat::RGBA32F;
                Logger::Get().InfoTag("io", "Legacy FourCC D3DFMT_R32F (114)");
            } else if (fourCC == 115) {
                // D3DFMT_G32R32F
                outFormat = DdsFormat::RGBA32_FLOAT;
                loadStorage = CanvasPixelFormat::RGBA32F;
                dxgiFormat = 16; // R32G32_FLOAT marker for RG32 path
                Logger::Get().InfoTag("io", "Legacy FourCC D3DFMT_G32R32F (115)");
            } else if (fourCC == 116) {
                // D3DFMT_A32B32G32R32F
                outFormat = DdsFormat::RGBA32_FLOAT;
                loadStorage = CanvasPixelFormat::RGBA32F;
                Logger::Get().InfoTag("io", "Legacy FourCC D3DFMT_A32B32G32R32F (116)");
            } else {
                char fcbuf[48];
                std::snprintf(fcbuf, sizeof(fcbuf), "Unsupported legacy FourCC %u (0x%08X) in DDS file: ",
                              fourCC, fourCC);
                Logger::Get().Error(std::string(fcbuf) + filename);
                return false;
            }
        } else {
            // Legacy uncompressed — masks mirror DirectXTex GetDXGIFormat / DDSPF_* tables.
            // Note: DX9 writers often encode R8G8 as A8L8 (DDPF_LUMINANCE|ALPHAPIXELS,
            // R=0x00FF, A=0xFF00) rather than DX10 DXGI R8G8_UNORM.
            const uint32_t pf = header.ddspf.dwFlags;
            const uint32_t bits = header.ddspf.dwRGBBitCount;
            const uint32_t rM = header.ddspf.dwRBitMask;
            const uint32_t gM = header.ddspf.dwGBitMask;
            const uint32_t bM = header.ddspf.dwBBitMask;
            const uint32_t aM = header.ddspf.dwABitMask;
            const bool isRGB  = (pf & 0x40) != 0;       // DDPF_RGB
            const bool isLum  = (pf & 0x20000) != 0;    // DDPF_LUMINANCE
            const bool isBump = (pf & 0x80000) != 0;    // DDPF_BUMPDUDV (V8U8 etc.)
            auto isMask = [&](uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
                return rM == r && gM == g && bM == b && aM == a;
            };

            if (isRGB && bits == 32 && isMask(0xffffffffu, 0, 0, 0)) {
                // D3DX R32F without FourCC (rare)
                outFormat = DdsFormat::R32_FLOAT;
                loadStorage = CanvasPixelFormat::RGBA32F;
                Logger::Get().InfoTag("io", "Legacy RGB mask R32F (0xFFFFFFFF)");
            } else if ((isRGB || (pf & 0x41) == 0x41) && bits == 32) {
                outFormat = DdsFormat::RGBA8_UNORM;
            } else if ((isLum || isRGB || isBump) && bits == 16 &&
                       (isMask(0x00ff, 0, 0, 0xff00) || isMask(0xff, 0, 0, 0xff00))) {
                // A8L8 / NVTT RGB-as-luminance → DXGI R8G8_UNORM (L→R, A→G)
                outFormat = DdsFormat::R8G8_UNORM;
            } else if (isRGB && bits == 16 && gM != 0 && bM == 0 && aM == 0) {
                // Rare true RG masks without alpha
                outFormat = DdsFormat::R8G8_UNORM;
            } else if (isBump && bits == 16 && isMask(0x00ff, 0xff00, 0, 0)) {
                // V8U8 signed bump → treat as R8G8 for open
                outFormat = DdsFormat::R8G8_UNORM;
            } else if ((isLum || isRGB) && bits == 8 && isMask(0xff, 0, 0, 0)) {
                outFormat = DdsFormat::R8_UNORM;
            } else {
                char detail[160];
                std::snprintf(detail, sizeof(detail),
                    "Unsupported legacy DDS format (flags=0x%X bits=%u R=0x%X G=0x%X B=0x%X A=0x%X) in: ",
                    pf, bits, rM, gM, bM, aM);
                Logger::Get().Error(std::string(detail) + filename);
                return false;
            }
        }
    }

    outCache.Init(outWidth, outHeight, loadStorage);

    auto writeRowRGBA8 = [&](const std::vector<uint8_t>& row, int y) {
        if (y >= 0 && y < outHeight) {
            outCache.ImportRGBA8(row.data(), outWidth, 1, 0, y);
        }
    };
    auto writeRowRGBA32F = [&](const std::vector<float>& row, int y) {
        // ImportRGBA32F quantizes into U8/F16/F32 storage as needed.
        if (y >= 0 && y < outHeight) {
            outCache.ImportRGBA32F(row.data(), outWidth, 1, 0, y);
        }
    };

    // --- HDR / unusual DXGI paths (float cache) ---
    if (isDX10 && dxgiFormat == 26) { // R11G11B10_FLOAT
        std::vector<uint32_t> rowPk((size_t)outWidth);
        std::vector<float> rowF((size_t)outWidth * 4);
        for (int y = 0; y < outHeight; ++y) {
            file.read(reinterpret_cast<char*>(rowPk.data()), (std::streamsize)rowPk.size() * 4);
            if (!file) {
                Logger::Get().Error("Failed reading R11G11B10 row from: " + filename);
                return false;
            }
            for (int x = 0; x < outWidth; ++x) {
                float r, g, b;
                DecodeR11G11B10Float(rowPk[(size_t)x], r, g, b);
                size_t d = (size_t)x * 4;
                rowF[d + 0] = r; rowF[d + 1] = g; rowF[d + 2] = b; rowF[d + 3] = 1.f;
            }
            writeRowRGBA32F(rowF, y);
        }
        Logger::Get().InfoTag("io", "Loaded R11G11B10_FLOAT as float RGBA");
        return true;
    }
    if (isDX10 && (dxgiFormat == 20 || dxgiFormat == 40)) {
        // DirectXTex LoadScanline (D32_FLOAT_S8X24): XMVector(depth, stencil_u8, 0, 1)
        // Convert DEPTH→RGBA8 UNORM:
        //   Stencil → Alpha  (A = stencil/255)
        //   Depth   → RGB    (R=G=B = saturate(depth))
        // Typical frame dumps: clear pixels have depth=0 and stencil=0 → fully transparent;
        // covered geometry keeps stencil as alpha (often a constant like 32).
        // PDN FileType+ uses the same DirectXTex Convert path.
        const bool withStencil = (dxgiFormat == 20);
        const size_t stride = withStencil ? 8 : 4;
        std::vector<uint8_t> rowRaw((size_t)outWidth * stride);
        std::vector<uint8_t> rowRGBA((size_t)outWidth * 4);
        for (int y = 0; y < outHeight; ++y) {
            file.read(reinterpret_cast<char*>(rowRaw.data()), (std::streamsize)rowRaw.size());
            if (!file) {
                Logger::Get().Error("Failed reading depth row from: " + filename);
                return false;
            }
            for (int x = 0; x < outWidth; ++x) {
                float depth = 0.f;
                std::memcpy(&depth, rowRaw.data() + (size_t)x * stride, sizeof(float));
                if (!std::isfinite(depth))
                    depth = 0.f;
                uint8_t gray = FloatToU8(depth);
                uint8_t alpha = 255;
                if (withStencil) {
                    // Low byte of the second dword is stencil (S8X24)
                    alpha = rowRaw[(size_t)x * stride + 4];
                }
                size_t d = (size_t)x * 4;
                rowRGBA[d + 0] = gray;
                rowRGBA[d + 1] = gray;
                rowRGBA[d + 2] = gray;
                rowRGBA[d + 3] = alpha;
            }
            writeRowRGBA8(rowRGBA, y);
        }
        Logger::Get().InfoTag("io", "Loaded depth DXGI " + std::to_string(dxgiFormat) +
            " as RGBA8 (DirectXTex: depth→RGB, stencil→A; U8 project)");
        return true;
    }
    if (isDX10 && (dxgiFormat == 49 || dxgiFormat == 50)) {
        // R8G8_UNORM (49) / R8G8_SNORM (50) — expand to RGBA, B=0, A=255
        const bool snorm = (dxgiFormat == 50);
        std::vector<uint8_t> rowRG((size_t)outWidth * 2);
        std::vector<uint8_t> rowRGBA((size_t)outWidth * 4);
        for (int y = 0; y < outHeight; ++y) {
            file.read(reinterpret_cast<char*>(rowRG.data()), (std::streamsize)rowRG.size());
            if (!file) {
                Logger::Get().Error("Failed reading R8G8 row from: " + filename);
                return false;
            }
            for (int x = 0; x < outWidth; ++x) {
                size_t s = (size_t)x * 2;
                size_t d = (size_t)x * 4;
                if (snorm) {
                    // SNORM: byte as signed → [-1,1] → [0,1] display
                    auto snormToU8 = [](uint8_t raw) -> uint8_t {
                        int8_t s8;
                        std::memcpy(&s8, &raw, 1);
                        float n = (s8 == -128) ? -1.f : (s8 / 127.f);
                        return FloatToU8(n * 0.5f + 0.5f);
                    };
                    rowRGBA[d + 0] = snormToU8(rowRG[s + 0]);
                    rowRGBA[d + 1] = snormToU8(rowRG[s + 1]);
                } else {
                    rowRGBA[d + 0] = rowRG[s + 0];
                    rowRGBA[d + 1] = rowRG[s + 1];
                }
                rowRGBA[d + 2] = 0;
                rowRGBA[d + 3] = 255;
            }
            writeRowRGBA8(rowRGBA, y);
        }
        Logger::Get().InfoTag("io", std::string("Loaded R8G8 ") + (snorm ? "SNORM" : "UNORM") +
            " as RG + opaque alpha");
        return true;
    }

    // Float paths: DX10 DXGI *or* legacy D3DFMT FourCC 111–116 / RGB-mask R32F
    if ((isDX10 && (dxgiFormat == DXGI_FORMAT_R32G32B32A32_FLOAT || dxgiFormat == 2)) ||
        (!isDX10 && outFormat == DdsFormat::RGBA32_FLOAT && loadStorage == CanvasPixelFormat::RGBA32F &&
         dxgiFormat != 16 /* not RG32 */)) {
        std::vector<float> rowFloats((size_t)outWidth * 4);
        for (int y = 0; y < outHeight; ++y) {
            file.read(reinterpret_cast<char*>(rowFloats.data()), (std::streamsize)rowFloats.size() * (std::streamsize)sizeof(float));
            if (!file) {
                Logger::Get().Error("Failed reading RGBA32F pixel row from: " + filename);
                return false;
            }
            writeRowRGBA32F(rowFloats, y);
        }
    } else if (!isDX10 && dxgiFormat == 16 && outFormat == DdsFormat::RGBA32_FLOAT) {
        // D3DFMT_G32R32F — two float32 channels
        std::vector<float> rowRG((size_t)outWidth * 2);
        std::vector<float> rowF((size_t)outWidth * 4);
        for (int y = 0; y < outHeight; ++y) {
            file.read(reinterpret_cast<char*>(rowRG.data()), (std::streamsize)rowRG.size() * sizeof(float));
            if (!file) {
                Logger::Get().Error("Failed reading G32R32F row from: " + filename);
                return false;
            }
            for (int x = 0; x < outWidth; ++x) {
                size_t s = (size_t)x * 2, d = (size_t)x * 4;
                float r = rowRG[s + 0], g = rowRG[s + 1];
                if (!std::isfinite(r)) r = 0.f;
                if (!std::isfinite(g)) g = 0.f;
                rowF[d + 0] = r; rowF[d + 1] = g; rowF[d + 2] = 0.f; rowF[d + 3] = 1.f;
            }
            writeRowRGBA32F(rowF, y);
        }
    } else if ((isDX10 && (dxgiFormat == DXGI_FORMAT_R16G16B16A16_FLOAT || dxgiFormat == 10)) ||
               (!isDX10 && outFormat == DdsFormat::RGBA16_FLOAT && loadStorage == CanvasPixelFormat::RGBA16F &&
                dxgiFormat != 34 /* not RG16 */)) {
        std::vector<uint16_t> rowHalf((size_t)outWidth * 4);
        std::vector<float> rowF((size_t)outWidth * 4);
        for (int y = 0; y < outHeight; ++y) {
            file.read(reinterpret_cast<char*>(rowHalf.data()), (std::streamsize)rowHalf.size() * (std::streamsize)sizeof(uint16_t));
            if (!file) {
                Logger::Get().Error("Failed reading RGBA16F pixel row from: " + filename);
                return false;
            }
            for (int x = 0; x < outWidth; ++x) {
                size_t src = (size_t)x * 4;
                rowF[src + 0] = HalfToFloat(rowHalf[src + 0]);
                rowF[src + 1] = HalfToFloat(rowHalf[src + 1]);
                rowF[src + 2] = HalfToFloat(rowHalf[src + 2]);
                rowF[src + 3] = HalfToFloat(rowHalf[src + 3]);
            }
            writeRowRGBA32F(rowF, y);
        }
    } else if (!isDX10 && dxgiFormat == 34 && outFormat == DdsFormat::RGBA16_FLOAT) {
        // D3DFMT_G16R16F
        std::vector<uint16_t> rowRG((size_t)outWidth * 2);
        std::vector<float> rowF((size_t)outWidth * 4);
        for (int y = 0; y < outHeight; ++y) {
            file.read(reinterpret_cast<char*>(rowRG.data()), (std::streamsize)rowRG.size() * sizeof(uint16_t));
            if (!file) {
                Logger::Get().Error("Failed reading G16R16F row from: " + filename);
                return false;
            }
            for (int x = 0; x < outWidth; ++x) {
                size_t s = (size_t)x * 2, d = (size_t)x * 4;
                float r = HalfToFloat(rowRG[s + 0]), g = HalfToFloat(rowRG[s + 1]);
                if (!std::isfinite(r)) r = 0.f;
                if (!std::isfinite(g)) g = 0.f;
                rowF[d + 0] = r; rowF[d + 1] = g; rowF[d + 2] = 0.f; rowF[d + 3] = 1.f;
            }
            writeRowRGBA32F(rowF, y);
        }
    } else if (isDX10 && dxgiFormat == DXGI_FORMAT_R16G16B16A16_UNORM) {
        std::vector<uint16_t> rowU16((size_t)outWidth * 4);
        std::vector<uint8_t> rowRGBA((size_t)outWidth * 4);
        for (int y = 0; y < outHeight; ++y) {
            file.read(reinterpret_cast<char*>(rowU16.data()), (std::streamsize)rowU16.size() * (std::streamsize)sizeof(uint16_t));
            if (!file) {
                Logger::Get().Error("Failed reading RGBA16UNORM pixel row from: " + filename);
                return false;
            }
            for (int x = 0; x < outWidth; ++x) {
                size_t src = (size_t)x * 4;
                size_t dst = src;
                rowRGBA[dst + 0] = static_cast<uint8_t>((rowU16[src + 0] / 65535.0f) * 255.0f + 0.5f);
                rowRGBA[dst + 1] = static_cast<uint8_t>((rowU16[src + 1] / 65535.0f) * 255.0f + 0.5f);
                rowRGBA[dst + 2] = static_cast<uint8_t>((rowU16[src + 2] / 65535.0f) * 255.0f + 0.5f);
                rowRGBA[dst + 3] = static_cast<uint8_t>((rowU16[src + 3] / 65535.0f) * 255.0f + 0.5f);
            }
            writeRowRGBA8(rowRGBA, y);
        }
    } else if (outFormat == DdsFormat::R32_FLOAT && loadStorage == CanvasPixelFormat::RGBA32F) {
        // True mono float HDR — DX10 R32_FLOAT or legacy D3DFMT_R32F (114).
        // Only base mip is consumed; remaining mips (if any) are ignored.
        std::vector<float> rowFloats((size_t)outWidth);
        std::vector<float> rowF((size_t)outWidth * 4);
        for (int y = 0; y < outHeight; ++y) {
            file.read(reinterpret_cast<char*>(rowFloats.data()), (std::streamsize)rowFloats.size() * (std::streamsize)sizeof(float));
            if (!file) {
                Logger::Get().Error("Failed reading R32F pixel row from: " + filename);
                return false;
            }
            for (int x = 0; x < outWidth; ++x) {
                float v = rowFloats[(size_t)x];
                if (!std::isfinite(v)) v = 0.f;
                size_t dst = (size_t)x * 4;
                rowF[dst + 0] = v;
                rowF[dst + 1] = v;
                rowF[dst + 2] = v;
                rowF[dst + 3] = 1.f;
            }
            writeRowRGBA32F(rowF, y);
        }
        Logger::Get().InfoTag("io", "Loaded R32F mono as float grayscale (F32 document)");
    } else if (outFormat == DdsFormat::R16_FLOAT && loadStorage == CanvasPixelFormat::RGBA16F) {
        std::vector<uint16_t> rowHalf((size_t)outWidth);
        std::vector<float> rowF((size_t)outWidth * 4);
        for (int y = 0; y < outHeight; ++y) {
            file.read(reinterpret_cast<char*>(rowHalf.data()), (std::streamsize)rowHalf.size() * (std::streamsize)sizeof(uint16_t));
            if (!file) {
                Logger::Get().Error("Failed reading R16F pixel row from: " + filename);
                return false;
            }
            for (int x = 0; x < outWidth; ++x) {
                float v = HalfToFloat(rowHalf[(size_t)x]);
                if (!std::isfinite(v)) v = 0.f;
                size_t dst = (size_t)x * 4;
                rowF[dst + 0] = v;
                rowF[dst + 1] = v;
                rowF[dst + 2] = v;
                rowF[dst + 3] = 1.f;
            }
            writeRowRGBA32F(rowF, y);
        }
        Logger::Get().InfoTag("io", "Loaded R16F mono as half grayscale (F16 document)");
    } else if (isDX10 && dxgiFormat == DXGI_FORMAT_R8_UNORM) {
        std::vector<uint8_t> rowR((size_t)outWidth);
        std::vector<uint8_t> rowRGBA((size_t)outWidth * 4);
        for (int y = 0; y < outHeight; ++y) {
            file.read(reinterpret_cast<char*>(rowR.data()), (std::streamsize)rowR.size());
            if (!file) {
                Logger::Get().Error("Failed reading R8 pixel row from: " + filename);
                return false;
            }
            for (int x = 0; x < outWidth; ++x) {
                uint8_t v = rowR[(size_t)x];
                size_t dst = (size_t)x * 4;
                rowRGBA[dst + 0] = v;
                rowRGBA[dst + 1] = v;
                rowRGBA[dst + 2] = v;
                rowRGBA[dst + 3] = 255;
            }
            writeRowRGBA8(rowRGBA, y);
        }
    } else if (isDX10 && dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM) {
        std::vector<uint8_t> rowRGBA((size_t)outWidth * 4);
        for (int y = 0; y < outHeight; ++y) {
            file.read(reinterpret_cast<char*>(rowRGBA.data()), (std::streamsize)rowRGBA.size());
            if (!file) {
                Logger::Get().Error("Failed reading RGBA8 pixel row from: " + filename);
                return false;
            }
            writeRowRGBA8(rowRGBA, y);
        }
    } else if (compFormat != CompressionFormat::None) {
        int blocksW = (outWidth + 3) / 4;
        int blocksH = (outHeight + 3) / 4;
        std::vector<uint8_t> compressedRow((size_t)blocksW * blockSize);

        if (compFormat == CompressionFormat::BC1 || compFormat == CompressionFormat::BC2 ||
            compFormat == CompressionFormat::BC3 || compFormat == CompressionFormat::BC7) {
            const int decodedStride = blocksW * 4 * 4;
            std::vector<uint8_t> decoded((size_t)decodedStride * 4);
            const int progressStep = std::max(1, blocksH / 20); // ~5% steps
            for (int by = 0; by < blocksH; ++by) {
                file.read(reinterpret_cast<char*>(compressedRow.data()), (std::streamsize)compressedRow.size());
                if (!file) {
                    Logger::Get().Error("Failed to read compressed DDS row from: " + filename);
                    return false;
                }

                for (int bx = 0; bx < blocksW; ++bx) {
                    const uint8_t* blockSrc = compressedRow.data() + (size_t)bx * blockSize;
                    uint8_t* blockDst = decoded.data() + (size_t)bx * 4 * 4;
                    if (compFormat == CompressionFormat::BC1) {
                        bcdec_bc1(blockSrc, blockDst, decodedStride);
                    } else if (compFormat == CompressionFormat::BC2) {
                        bcdec_bc2(blockSrc, blockDst, decodedStride);
                    } else if (compFormat == CompressionFormat::BC3) {
                        bcdec_bc3(blockSrc, blockDst, decodedStride);
                    } else {
                        bcdec_bc7(blockSrc, blockDst, decodedStride);
                    }
                }

                for (int row = 0; row < 4; ++row) {
                    int y = by * 4 + row;
                    if (y >= outHeight) break;
                    const uint8_t* rowPtr = decoded.data() + (size_t)row * decodedStride;
                    outCache.ImportRGBA8(rowPtr, blocksW * 4, 1, 0, y);
                }

                if (by == 0 || ((by + 1) % progressStep) == 0 || by + 1 == blocksH) {
                    int pct = (int)(((by + 1) * 100.0) / blocksH);
                    Logger::Get().InfoTag("io",
                        "DDS BC decode " + std::to_string(pct) + "% (" +
                        std::to_string(by + 1) + "/" + std::to_string(blocksH) +
                        " block-rows) tiles=" + std::to_string(outCache.GetTileCount()));
                }
            }
        } else if (compFormat == CompressionFormat::BC4) {
            const int decodedStride = blocksW * 4;
            std::vector<uint8_t> decodedR((size_t)decodedStride * 4);
            std::vector<uint8_t> rowRGBA((size_t)outWidth * 4);
            for (int by = 0; by < blocksH; ++by) {
                file.read(reinterpret_cast<char*>(compressedRow.data()), (std::streamsize)compressedRow.size());
                if (!file) {
                    Logger::Get().Error("Failed to read compressed DDS row from: " + filename);
                    return false;
                }

                for (int bx = 0; bx < blocksW; ++bx) {
                    const uint8_t* blockSrc = compressedRow.data() + (size_t)bx * blockSize;
                    uint8_t* blockDst = decodedR.data() + (size_t)bx * 4;
                    bcdec_bc4(blockSrc, blockDst, decodedStride);
                }

                for (int row = 0; row < 4; ++row) {
                    int y = by * 4 + row;
                    if (y >= outHeight) break;
                    const uint8_t* srcRow = decodedR.data() + (size_t)row * decodedStride;
                    for (int x = 0; x < outWidth; ++x) {
                        uint8_t v = srcRow[x];
                        size_t dst = (size_t)x * 4;
                        rowRGBA[dst + 0] = v;
                        rowRGBA[dst + 1] = v;
                        rowRGBA[dst + 2] = v;
                        rowRGBA[dst + 3] = 255;
                    }
                    writeRowRGBA8(rowRGBA, y);
                }
            }
        } else if (compFormat == CompressionFormat::BC5) {
            const int decodedStride = blocksW * 4 * 2;
            std::vector<uint8_t> decodedRG((size_t)decodedStride * 4);
            std::vector<uint8_t> rowRGBA((size_t)outWidth * 4);
            for (int by = 0; by < blocksH; ++by) {
                file.read(reinterpret_cast<char*>(compressedRow.data()), (std::streamsize)compressedRow.size());
                if (!file) {
                    Logger::Get().Error("Failed to read compressed DDS row from: " + filename);
                    return false;
                }

                for (int bx = 0; bx < blocksW; ++bx) {
                    const uint8_t* blockSrc = compressedRow.data() + (size_t)bx * blockSize;
                    uint8_t* blockDst = decodedRG.data() + (size_t)bx * 4 * 2;
                    bcdec_bc5(blockSrc, blockDst, decodedStride);
                }

                for (int row = 0; row < 4; ++row) {
                    int y = by * 4 + row;
                    if (y >= outHeight) break;
                    const uint8_t* srcRow = decodedRG.data() + (size_t)row * decodedStride;
                    for (int x = 0; x < outWidth; ++x) {
                        size_t src = (size_t)x * 2;
                        size_t dst = (size_t)x * 4;
                        rowRGBA[dst + 0] = srcRow[src + 0];
                        rowRGBA[dst + 1] = srcRow[src + 1];
                        rowRGBA[dst + 2] = 0;
                        rowRGBA[dst + 3] = 255;
                    }
                    writeRowRGBA8(rowRGBA, y);
                }
            }
        }
    } else if (!isDX10 && outFormat == DdsFormat::R8G8_UNORM) {
        // Legacy 16-bit: A8L8 (R=0x00FF,A=0xFF00) → DXGI R8G8 (L→R, A→G) per DirectXTex.
        // Also handles V8U8 (R=0x00FF,G=0xFF00) and similar.
        std::vector<uint8_t> rowRG((size_t)outWidth * 2);
        std::vector<uint8_t> rowRGBA((size_t)outWidth * 4);
        const uint32_t rMask = header.ddspf.dwRBitMask;
        const uint32_t gMask = header.ddspf.dwGBitMask;
        const uint32_t aMask = header.ddspf.dwABitMask;
        auto extract8 = [](uint16_t packed, uint32_t mask) -> uint8_t {
            if (mask == 0) return 0;
            uint32_t v = packed & mask;
            while (mask && (mask & 1u) == 0) { mask >>= 1; v >>= 1; }
            return static_cast<uint8_t>(v & 0xFFu);
        };
        // A8L8: second channel lives in ABitMask, not GBitMask
        const uint32_t secondMask = (gMask != 0) ? gMask : aMask;
        for (int y = 0; y < outHeight; ++y) {
            file.read(reinterpret_cast<char*>(rowRG.data()), (std::streamsize)rowRG.size());
            if (!file) {
                Logger::Get().Error("Failed reading legacy R8G8/A8L8 row from: " + filename);
                return false;
            }
            for (int x = 0; x < outWidth; ++x) {
                uint16_t packed = 0;
                std::memcpy(&packed, rowRG.data() + (size_t)x * 2, 2);
                size_t d = (size_t)x * 4;
                rowRGBA[d + 0] = extract8(packed, rMask);
                rowRGBA[d + 1] = extract8(packed, secondMask);
                rowRGBA[d + 2] = 0;
                rowRGBA[d + 3] = 255;
            }
            writeRowRGBA8(rowRGBA, y);
        }
        Logger::Get().InfoTag("io", "Loaded legacy A8L8/R8G8 as RG + opaque alpha");
    } else if (!isDX10 && outFormat == DdsFormat::R8_UNORM) {
        std::vector<uint8_t> rowR((size_t)outWidth);
        std::vector<uint8_t> rowRGBA((size_t)outWidth * 4);
        for (int y = 0; y < outHeight; ++y) {
            file.read(reinterpret_cast<char*>(rowR.data()), (std::streamsize)rowR.size());
            if (!file) {
                Logger::Get().Error("Failed reading legacy L8/R8 row from: " + filename);
                return false;
            }
            for (int x = 0; x < outWidth; ++x) {
                uint8_t v = rowR[(size_t)x];
                size_t d = (size_t)x * 4;
                rowRGBA[d + 0] = v;
                rowRGBA[d + 1] = v;
                rowRGBA[d + 2] = v;
                rowRGBA[d + 3] = 255;
            }
            writeRowRGBA8(rowRGBA, y);
        }
    } else {
        std::vector<uint8_t> rowBytes((size_t)outWidth * 4);
        std::vector<uint8_t> rowRGBA((size_t)outWidth * 4);
        bool isBGRA = (header.ddspf.dwRBitMask == 0x00ff0000);
        for (int y = 0; y < outHeight; ++y) {
            file.read(reinterpret_cast<char*>(rowBytes.data()), (std::streamsize)rowBytes.size());
            if (!file) {
                Logger::Get().Error("Failed reading RGBA8 pixel row from: " + filename);
                return false;
            }
            for (int x = 0; x < outWidth; ++x) {
                size_t src = (size_t)x * 4;
                size_t dst = src;
                if (isBGRA) {
                    rowRGBA[dst + 0] = rowBytes[src + 2];
                    rowRGBA[dst + 1] = rowBytes[src + 1];
                    rowRGBA[dst + 2] = rowBytes[src + 0];
                    rowRGBA[dst + 3] = rowBytes[src + 3];
                } else {
                    rowRGBA[dst + 0] = rowBytes[src + 0];
                    rowRGBA[dst + 1] = rowBytes[src + 1];
                    rowRGBA[dst + 2] = rowBytes[src + 2];
                    rowRGBA[dst + 3] = rowBytes[src + 3];
                }
            }
            writeRowRGBA8(rowRGBA, y);
        }
    }

    double ms = std::chrono::duration<double, std::milli>(
        std::chrono::high_resolution_clock::now() - loadStart).count();
    const size_t tiles = outCache.GetTileCount();
    const size_t est = MemoryStats::EstimateTileBytes(tiles, outCache.GetBytesPerPixel());
    Logger::Get().InfoTag("io",
        "DDS texture loaded successfully (" + std::to_string(outWidth) + "x" +
        std::to_string(outHeight) + ") tiles=" + std::to_string(tiles) +
        " est=" + MemoryStats::FormatBytes(est) +
        " in " + std::to_string(ms) + " ms: " + filename);
    MemoryStats::LogSnapshot("dds_open_done");
    return true;
}

static uint16_t FloatToHalf(float f) { return HalfFloat::FromFloat(f); }

std::string DdsHelper::SniffFormatLabel(const std::string& filename) {
    DdsCodec::SourceInfo info;
    if (DdsCodec::AnalyzeFile(filename, info) && info.width > 0) {
        char dim[32];
        std::snprintf(dim, sizeof(dim), "%dx%d", info.width, info.height);
        return info.formatLabel + " · " + dim;
    }
    // Legacy sniff fallback
    std::ifstream file(PathUtil::FromUtf8(PathUtil::NormalizeToUtf8Path(filename)), std::ios::binary);
    if (!file.is_open())
        file.open(PathUtil::FromUtf8(filename), std::ios::binary);
    if (!file.is_open()) return {};

    uint32_t magic = 0;
    file.read(reinterpret_cast<char*>(&magic), 4);
    if (magic != MAKEFOURCC('D', 'D', 'S', ' ')) return {};

    DDS_HEADER header{};
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!file || header.dwSize != 124) return {};

    char dim[32];
    std::snprintf(dim, sizeof(dim), "%ux%u", header.dwWidth, header.dwHeight);

    auto withDim = [&](const char* fmt) -> std::string {
        return std::string(fmt) + " · " + dim;
    };

    bool isDX10 = (header.ddspf.dwFlags & 0x4) &&
                  (header.ddspf.dwFourCC == MAKEFOURCC('D', 'X', '1', '0'));
    if (isDX10) {
        DDS_HEADER_DXT10 dx10{};
        file.read(reinterpret_cast<char*>(&dx10), sizeof(dx10));
        if (!file) return withDim("DX10");
        switch (dx10.dxgiFormat) {
        case 2:  return withDim("RGBA32F");
        case 10: return withDim("RGBA16F");
        case 11: return withDim("RGBA16");
        case 28: return withDim("RGBA8");
        case 41: return withDim("R32F");
        case 54: return withDim("R16F");
        case 61: return withDim("R8 / L8");
        case 49: // R8G8_UNORM
        case 50: return withDim("R8G8");
        case 51: return withDim("R8G8B8A8");
        case 24: // R10G10B10A2
            return withDim("R10G10B10A2");
        case 71: case 72: return withDim("BC1");
        case 74: case 75: return withDim("BC2");
        case 77: case 78: return withDim("BC3");
        case 80: case 81: return withDim("BC4");
        case 83: case 84: return withDim("BC5");
        case 95: return withDim("BC6H_UF16");
        case 96: return withDim("BC6H_SF16");
        case 98: case 99: return withDim("BC7");
        default: {
            char b[48];
            std::snprintf(b, sizeof(b), "DXGI_%u · %s", dx10.dxgiFormat, dim);
            return b;
        }
        }
    }

    if (header.ddspf.dwFlags & 0x4) {
        uint32_t cc = header.ddspf.dwFourCC;
        if (cc == MAKEFOURCC('D','X','T','1')) return withDim("BC1/DXT1");
        if (cc == MAKEFOURCC('D','X','T','3')) return withDim("BC2/DXT3");
        if (cc == MAKEFOURCC('D','X','T','5')) return withDim("BC3/DXT5");
        if (cc == MAKEFOURCC('A','T','I','1') || cc == MAKEFOURCC('B','C','4','U'))
            return withDim("BC4");
        if (cc == MAKEFOURCC('A','T','I','2') || cc == MAKEFOURCC('B','C','5','U'))
            return withDim("BC5");
        char four[5] = {
            (char)(cc & 0xFF), (char)((cc >> 8) & 0xFF),
            (char)((cc >> 16) & 0xFF), (char)((cc >> 24) & 0xFF), 0
        };
        return withDim(four);
    }

    // Uncompressed RGB(A) bitmasks
    if (header.ddspf.dwRGBBitCount == 8)
        return withDim("L8");
    if (header.ddspf.dwRGBBitCount == 16) {
        // L8A8 common
        if (header.ddspf.dwABitMask != 0) return withDim("L8A8");
        return withDim("R5G6B5");
    }
    if (header.ddspf.dwRGBBitCount == 32) {
        if (header.ddspf.dwABitMask) return withDim("RGBA8");
        return withDim("RGB8");
    }
    if (header.ddspf.dwRGBBitCount == 24) return withDim("RGB8");

    char bits[32];
    std::snprintf(bits, sizeof(bits), "%u-bit", header.ddspf.dwRGBBitCount);
    return withDim(bits);
}

bool DdsHelper::SaveDDS(const std::string& filename, const DdsImage& image) {
#ifdef _WIN32
    std::ofstream file(PathUtil::FromUtf8(PathUtil::NormalizeToUtf8Path(filename)), std::ios::binary);
#else
    std::ofstream file(filename, std::ios::binary);
#endif
    if (!file.is_open()) {
        Logger::Get().Error("Failed to open DDS file for writing: " + filename);
        return false;
    }

    uint32_t dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    uint32_t bytesPerPixel = 4;

    switch (image.format) {
        case DdsFormat::RGBA8_UNORM:
            dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            bytesPerPixel = 4;
            break;
        case DdsFormat::RGBA16_UNORM:
            dxgiFormat = DXGI_FORMAT_R16G16B16A16_UNORM;
            bytesPerPixel = 8;
            break;
        case DdsFormat::RGBA16_FLOAT:
            dxgiFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
            bytesPerPixel = 8;
            break;
        case DdsFormat::RGBA32_FLOAT:
            dxgiFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
            bytesPerPixel = 16;
            break;
        case DdsFormat::R8_UNORM:
            dxgiFormat = DXGI_FORMAT_R8_UNORM;
            bytesPerPixel = 1;
            break;
        case DdsFormat::R16_FLOAT:
            dxgiFormat = DXGI_FORMAT_R16_FLOAT;
            bytesPerPixel = 2;
            break;
        case DdsFormat::R32_FLOAT:
            dxgiFormat = DXGI_FORMAT_R32_FLOAT;
            bytesPerPixel = 4;
            break;
    }

    // Write Magic number
    uint32_t magic = MAKEFOURCC('D', 'D', 'S', ' ');
    file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));

    // Populate DDS_HEADER
    DDS_HEADER header = {};
    header.dwSize = 124;
    header.dwFlags = 0x1 | 0x2 | 0x4 | 0x1000; // DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT
    header.dwHeight = image.height;
    header.dwWidth = image.width;
    header.dwPitchOrLinearSize = image.width * bytesPerPixel;
    header.dwDepth = 0;
    header.dwMipMapCount = 1;
    
    header.ddspf.dwSize = 32;
    header.ddspf.dwFlags = 0x4; // DDPF_FOURCC
    header.ddspf.dwFourCC = MAKEFOURCC('D', 'X', '1', '0');
    
    header.dwCaps = 0x1000; // DDSCAPS_TEXTURE

    file.write(reinterpret_cast<const char*>(&header), sizeof(header));

    // Populate DDS_HEADER_DXT10
    DDS_HEADER_DXT10 dxt10Header = {};
    dxt10Header.dxgiFormat = dxgiFormat;
    dxt10Header.resourceDimension = 3; // D3D11_RESOURCE_DIMENSION_TEXTURE2D
    dxt10Header.arraySize = 1;

    file.write(reinterpret_cast<const char*>(&dxt10Header), sizeof(dxt10Header));

    // Convert and write pixel data
    size_t numPixels = (size_t)image.width * image.height;
    std::vector<uint8_t> outputData(numPixels * bytesPerPixel);

    for (size_t i = 0; i < numPixels; ++i) {
        size_t srcIdx = i * 4;
        size_t dstIdx = i * bytesPerPixel;

        float r = image.pixels[srcIdx + 0];
        float g = image.pixels[srcIdx + 1];
        float b = image.pixels[srcIdx + 2];
        float a = image.pixels[srcIdx + 3];

        if (image.format == DdsFormat::RGBA8_UNORM) {
            outputData[dstIdx + 0] = static_cast<uint8_t>(std::clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f);
            outputData[dstIdx + 1] = static_cast<uint8_t>(std::clamp(g, 0.0f, 1.0f) * 255.0f + 0.5f);
            outputData[dstIdx + 2] = static_cast<uint8_t>(std::clamp(b, 0.0f, 1.0f) * 255.0f + 0.5f);
            outputData[dstIdx + 3] = static_cast<uint8_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
        else if (image.format == DdsFormat::RGBA16_UNORM) {
            uint16_t ru = static_cast<uint16_t>(std::clamp(r, 0.0f, 1.0f) * 65535.0f + 0.5f);
            uint16_t gu = static_cast<uint16_t>(std::clamp(g, 0.0f, 1.0f) * 65535.0f + 0.5f);
            uint16_t bu = static_cast<uint16_t>(std::clamp(b, 0.0f, 1.0f) * 65535.0f + 0.5f);
            uint16_t au = static_cast<uint16_t>(std::clamp(a, 0.0f, 1.0f) * 65535.0f + 0.5f);
            std::memcpy(&outputData[dstIdx + 0], &ru, 2);
            std::memcpy(&outputData[dstIdx + 2], &gu, 2);
            std::memcpy(&outputData[dstIdx + 4], &bu, 2);
            std::memcpy(&outputData[dstIdx + 6], &au, 2);
        }
        else if (image.format == DdsFormat::RGBA16_FLOAT) {
            uint16_t rf = FloatToHalf(r);
            uint16_t gf = FloatToHalf(g);
            uint16_t bf = FloatToHalf(b);
            uint16_t af = FloatToHalf(a);
            std::memcpy(&outputData[dstIdx + 0], &rf, 2);
            std::memcpy(&outputData[dstIdx + 2], &gf, 2);
            std::memcpy(&outputData[dstIdx + 4], &bf, 2);
            std::memcpy(&outputData[dstIdx + 6], &af, 2);
        }
        else if (image.format == DdsFormat::RGBA32_FLOAT) {
            std::memcpy(&outputData[dstIdx + 0],  &r, 4);
            std::memcpy(&outputData[dstIdx + 4],  &g, 4);
            std::memcpy(&outputData[dstIdx + 8],  &b, 4);
            std::memcpy(&outputData[dstIdx + 12], &a, 4);
        }
        else if (image.format == DdsFormat::R8_UNORM) {
            float gray = 0.299f * r + 0.587f * g + 0.114f * b;
            outputData[dstIdx] = static_cast<uint8_t>(std::clamp(gray, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
        else if (image.format == DdsFormat::R16_FLOAT) {
            float gray = 0.299f * r + 0.587f * g + 0.114f * b;
            uint16_t gf = FloatToHalf(gray);
            std::memcpy(&outputData[dstIdx], &gf, 2);
        }
        else if (image.format == DdsFormat::R32_FLOAT) {
            float gray = 0.299f * r + 0.587f * g + 0.114f * b;
            std::memcpy(&outputData[dstIdx], &gray, 4);
        }
    }

    file.write(reinterpret_cast<const char*>(outputData.data()), outputData.size());

    if (!file) {
        Logger::Get().Error("Failed to write DDS pixel data to: " + filename);
        return false;
    }

    Logger::Get().Info("DDS texture saved successfully (" + std::to_string(image.width) + "x" + std::to_string(image.height) + "): " + filename);
    return true;
}
