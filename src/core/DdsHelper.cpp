#include "DdsHelper.h"
#include "TileCache.h"
#include "Logger.h"
#include <fstream>
#include <cstring>
#include <algorithm>
#include <vector>

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


#define BCDEC_IMPLEMENTATION
#include "bcdec.h"


#ifndef MAKEFOURCC
    #define MAKEFOURCC(ch0, ch1, ch2, ch3) \
                ((uint32_t)(uint8_t)(ch0) | ((uint32_t)(uint8_t)(ch1) << 8) | \
                ((uint32_t)(uint8_t)(ch2) << 16) | ((uint32_t)(uint8_t)(ch3) << 24 ))
#endif

// DXGI Format definitions
constexpr uint32_t DXGI_FORMAT_R32G32B32A32_FLOAT = 2;
constexpr uint32_t DXGI_FORMAT_R16G16B16A16_FLOAT = 10;
constexpr uint32_t DXGI_FORMAT_R16G16B16A16_UNORM = 11;
constexpr uint32_t DXGI_FORMAT_R8G8B8A8_UNORM = 28;
constexpr uint32_t DXGI_FORMAT_R32_FLOAT = 41;
constexpr uint32_t DXGI_FORMAT_R16_FLOAT = 54;
constexpr uint32_t DXGI_FORMAT_R8_UNORM = 61;

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

static float HalfToFloat(uint16_t h) {
    uint32_t sign = (uint32_t(h) & 0x8000u) << 16;
    uint32_t exp  = (uint32_t(h) & 0x7C00u) >> 10;
    uint32_t mant = uint32_t(h) & 0x03FFu;

    uint32_t bits = 0;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {
            // Normalize subnormal value.
            exp = 1;
            while ((mant & 0x0400u) == 0) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x03FFu;
            bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
        }
    } else if (exp == 0x1F) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }

    float out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

bool DdsHelper::LoadDDS(const std::string& filename, DdsImage& outImage) {
#ifdef _WIN32
    std::ifstream file(UTF8ToWString(filename), std::ios::binary);
#else
    std::ifstream file(filename, std::ios::binary);
#endif
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

bool DdsHelper::LoadDDSToTileCache(const std::string& filename, TileCache& outCache, int& outWidth, int& outHeight, DdsFormat& outFormat) {
#ifdef _WIN32
    std::ifstream file(UTF8ToWString(filename), std::ios::binary);
#else
    std::ifstream file(filename, std::ios::binary);
#endif
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
    outCache.Init(outWidth, outHeight, CanvasPixelFormat::RGBA8);

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

    if (isDX10) {
        DDS_HEADER_DXT10 dxt10Header;
        file.read(reinterpret_cast<char*>(&dxt10Header), sizeof(dxt10Header));
        dxgiFormat = dxt10Header.dxgiFormat;

        if (dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM) {
            outFormat = DdsFormat::RGBA8_UNORM;
        } else if (dxgiFormat == DXGI_FORMAT_R32G32B32A32_FLOAT) {
            outFormat = DdsFormat::RGBA32_FLOAT;
        } else if (dxgiFormat == DXGI_FORMAT_R16G16B16A16_FLOAT) {
            outFormat = DdsFormat::RGBA16_FLOAT;
        } else if (dxgiFormat == DXGI_FORMAT_R16G16B16A16_UNORM) {
            outFormat = DdsFormat::RGBA16_UNORM;
        } else if (dxgiFormat == DXGI_FORMAT_R32_FLOAT) {
            outFormat = DdsFormat::R32_FLOAT;
        } else if (dxgiFormat == DXGI_FORMAT_R16_FLOAT) {
            outFormat = DdsFormat::R16_FLOAT;
        } else if (dxgiFormat == DXGI_FORMAT_R8_UNORM) {
            outFormat = DdsFormat::R8_UNORM;
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
            } else {
                Logger::Get().Error("Unsupported legacy FourCC format in DDS file: " + filename);
                return false;
            }
        } else {
            bool isRGBA = (header.ddspf.dwFlags & 0x40) || (header.ddspf.dwFlags & 0x41);
            if (isRGBA && header.ddspf.dwRGBBitCount == 32) {
                outFormat = DdsFormat::RGBA8_UNORM;
            } else {
                Logger::Get().Error("Unsupported legacy DDS format. Only uncompressed 32-bit RGBA/BGRA is supported natively in: " + filename);
                return false;
            }
        }
    }

    auto writeRowRGBA8 = [&](const std::vector<uint8_t>& row, int y) {
        if (y >= 0 && y < outHeight) {
            outCache.ImportRGBA8(row.data(), outWidth, 1, 0, y);
        }
    };

    if (isDX10 && dxgiFormat == DXGI_FORMAT_R32G32B32A32_FLOAT) {
        std::vector<float> rowFloats((size_t)outWidth * 4);
        std::vector<uint8_t> rowRGBA((size_t)outWidth * 4);
        for (int y = 0; y < outHeight; ++y) {
            file.read(reinterpret_cast<char*>(rowFloats.data()), (std::streamsize)rowFloats.size() * (std::streamsize)sizeof(float));
            if (!file) {
                Logger::Get().Error("Failed reading RGBA32F pixel row from: " + filename);
                return false;
            }
            for (int x = 0; x < outWidth; ++x) {
                size_t src = (size_t)x * 4;
                size_t dst = src;
                rowRGBA[dst + 0] = FloatToU8(rowFloats[src + 0]);
                rowRGBA[dst + 1] = FloatToU8(rowFloats[src + 1]);
                rowRGBA[dst + 2] = FloatToU8(rowFloats[src + 2]);
                rowRGBA[dst + 3] = FloatToU8(rowFloats[src + 3]);
            }
            writeRowRGBA8(rowRGBA, y);
        }
    } else if (isDX10 && dxgiFormat == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        std::vector<uint16_t> rowHalf((size_t)outWidth * 4);
        std::vector<uint8_t> rowRGBA((size_t)outWidth * 4);
        for (int y = 0; y < outHeight; ++y) {
            file.read(reinterpret_cast<char*>(rowHalf.data()), (std::streamsize)rowHalf.size() * (std::streamsize)sizeof(uint16_t));
            if (!file) {
                Logger::Get().Error("Failed reading RGBA16F pixel row from: " + filename);
                return false;
            }
            for (int x = 0; x < outWidth; ++x) {
                size_t src = (size_t)x * 4;
                size_t dst = src;
                rowRGBA[dst + 0] = FloatToU8(HalfToFloat(rowHalf[src + 0]));
                rowRGBA[dst + 1] = FloatToU8(HalfToFloat(rowHalf[src + 1]));
                rowRGBA[dst + 2] = FloatToU8(HalfToFloat(rowHalf[src + 2]));
                rowRGBA[dst + 3] = FloatToU8(HalfToFloat(rowHalf[src + 3]));
            }
            writeRowRGBA8(rowRGBA, y);
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
    } else if (isDX10 && dxgiFormat == DXGI_FORMAT_R32_FLOAT) {
        std::vector<float> rowFloats((size_t)outWidth);
        std::vector<uint8_t> rowRGBA((size_t)outWidth * 4);
        for (int y = 0; y < outHeight; ++y) {
            file.read(reinterpret_cast<char*>(rowFloats.data()), (std::streamsize)rowFloats.size() * (std::streamsize)sizeof(float));
            if (!file) {
                Logger::Get().Error("Failed reading R32F pixel row from: " + filename);
                return false;
            }
            for (int x = 0; x < outWidth; ++x) {
                uint8_t v = FloatToU8(rowFloats[(size_t)x]);
                size_t dst = (size_t)x * 4;
                rowRGBA[dst + 0] = v;
                rowRGBA[dst + 1] = v;
                rowRGBA[dst + 2] = v;
                rowRGBA[dst + 3] = 255;
            }
            writeRowRGBA8(rowRGBA, y);
        }
    } else if (isDX10 && dxgiFormat == DXGI_FORMAT_R16_FLOAT) {
        std::vector<uint16_t> rowHalf((size_t)outWidth);
        std::vector<uint8_t> rowRGBA((size_t)outWidth * 4);
        for (int y = 0; y < outHeight; ++y) {
            file.read(reinterpret_cast<char*>(rowHalf.data()), (std::streamsize)rowHalf.size() * (std::streamsize)sizeof(uint16_t));
            if (!file) {
                Logger::Get().Error("Failed reading R16F pixel row from: " + filename);
                return false;
            }
            for (int x = 0; x < outWidth; ++x) {
                uint8_t v = FloatToU8(HalfToFloat(rowHalf[(size_t)x]));
                size_t dst = (size_t)x * 4;
                rowRGBA[dst + 0] = v;
                rowRGBA[dst + 1] = v;
                rowRGBA[dst + 2] = v;
                rowRGBA[dst + 3] = 255;
            }
            writeRowRGBA8(rowRGBA, y);
        }
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

    Logger::Get().Info("DDS texture loaded successfully (" + std::to_string(outWidth) + "x" + std::to_string(outHeight) + "): " + filename);
    return true;
}

static uint16_t FloatToHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, sizeof(float));
    uint32_t sign = (x >> 16) & 0x8000;
    int32_t exponent = ((x >> 23) & 0xFF) - 127;
    uint32_t mantissa = x & 0x007FFFFF;

    if (exponent == -127) { // Zero or subnormal
        return (uint16_t)sign;
    }
    if (exponent > 15) { // Overflow, map to infinity
        return (uint16_t)(sign | 0x7C00);
    }
    if (exponent < -14) { // Underflow, map to zero
        return (uint16_t)sign;
    }
    exponent += 15;
    mantissa >>= 13;
    return (uint16_t)(sign | (exponent << 10) | mantissa);
}

bool DdsHelper::SaveDDS(const std::string& filename, const DdsImage& image) {
#ifdef _WIN32
    std::ofstream file(UTF8ToWString(filename), std::ios::binary);
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
    dxt10Header.resourceDimension = 3; // DDS DX10 Texture2D resource dimension
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
