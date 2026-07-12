#include "DdsHeaderInfo.h"

#include <algorithm>
#include <cstring>
#include <windows.h>

#ifndef MAKEFOURCC
#define MAKEFOURCC(ch0, ch1, ch2, ch3) \
    ((uint32_t)(uint8_t)(ch0) | ((uint32_t)(uint8_t)(ch1) << 8) | \
     ((uint32_t)(uint8_t)(ch2) << 16) | ((uint32_t)(uint8_t)(ch3) << 24))
#endif

namespace ddsinfo {
namespace {

struct DDS_PIXELFORMAT {
    uint32_t dwSize, dwFlags, dwFourCC, dwRGBBitCount;
    uint32_t dwRBitMask, dwGBitMask, dwBBitMask, dwABitMask;
};
struct DDS_HEADER {
    uint32_t dwSize, dwFlags, dwHeight, dwWidth, dwPitchOrLinearSize;
    uint32_t dwDepth, dwMipMapCount, dwReserved1[11];
    DDS_PIXELFORMAT ddspf;
    uint32_t dwCaps, dwCaps2, dwCaps3, dwCaps4, dwReserved2;
};
struct DDS_HEADER_DXT10 {
    uint32_t dxgiFormat, resourceDimension, miscFlag, arraySize, miscFlags2;
};

// DXGI_FORMAT numeric values (dxgiformat.h) — keep shell free of DirectXTex.
static const char* DxgiName(uint32_t f) {
    switch (f) {
    case 2:  return "R32G32B32A32_FLOAT";
    case 10: return "R16G16B16A16_FLOAT";
    case 11: return "R16G16B16A16_UNORM";
    case 16: return "R32G32_FLOAT";
    case 24: return "R10G10B10A2_UNORM";
    case 25: return "R10G10B10A2_UINT";
    case 26: return "R11G11B10_FLOAT";
    case 27: return "R8G8B8A8_TYPELESS";
    case 28: return "R8G8B8A8_UNORM";
    case 29: return "R8G8B8A8_UNORM_SRGB";
    case 30: return "R8G8B8A8_UINT";
    case 31: return "R8G8B8A8_SNORM";
    case 34: return "R16G16_FLOAT";
    case 40: return "D32_FLOAT";
    case 41: return "R32_FLOAT";
    case 49: return "R8G8_UNORM";
    case 50: return "R8G8_SNORM";
    case 54: return "R16_FLOAT";
    case 56: return "D16_UNORM";
    case 61: return "R8_UNORM";
    case 65: return "R9G9B9E5_SHAREDEXP";
    case 70: return "BC1_TYPELESS";
    case 71: return "BC1_UNORM";
    case 72: return "BC1_UNORM_SRGB";
    case 73: return "BC2_TYPELESS";
    case 74: return "BC2_UNORM";
    case 75: return "BC2_UNORM_SRGB";
    case 76: return "BC3_TYPELESS";
    case 77: return "BC3_UNORM";
    case 78: return "BC3_UNORM_SRGB";
    case 79: return "BC4_TYPELESS";
    case 80: return "BC4_UNORM";
    case 81: return "BC4_SNORM";
    case 82: return "BC5_TYPELESS";
    case 83: return "BC5_UNORM";
    case 84: return "BC5_SNORM";
    case 85: return "B5G6R5_UNORM";
    case 86: return "B5G5R5A1_UNORM";
    case 87: return "B8G8R8A8_UNORM";
    case 88: return "B8G8R8X8_UNORM";
    case 90: return "B8G8R8A8_TYPELESS";
    case 91: return "B8G8R8A8_UNORM_SRGB";
    case 92: return "B8G8R8X8_TYPELESS";
    case 93: return "B8G8R8X8_UNORM_SRGB";
    case 94: return "BC6H_TYPELESS";
    case 95: return "BC6H_UF16";
    case 96: return "BC6H_SF16";
    case 97: return "BC7_TYPELESS";
    case 98: return "BC7_UNORM";
    case 99: return "BC7_UNORM_SRGB";
    case 115: return "B4G4R4A4_UNORM";
    default: return nullptr;
    }
}

static int ApproxBppDxgi(uint32_t f) {
    switch (f) {
    case 2: return 128;
    case 10: case 11: return 64;
    case 16: case 24: case 25: case 26: case 28: case 29: case 30: case 31:
    case 40: case 41: case 87: case 88: case 91: case 93: return 32;
    case 34: case 49: case 50: case 54: case 56: case 85: case 86: case 115: return 16;
    case 61: return 8;
    case 65: return 32; // R9G9B9E5
    case 71: case 72: case 80: case 81: return 4;   // BC1/BC4
    case 74: case 75: case 77: case 78:
    case 83: case 84: case 95: case 96: case 98: case 99: return 8; // BC2/3/5/6/7
    default: return 0;
    }
}

static std::string FourCCStr(uint32_t cc) {
    char s[5] = {
        (char)(cc & 0xFF), (char)((cc >> 8) & 0xFF),
        (char)((cc >> 16) & 0xFF), (char)((cc >> 24) & 0xFF), 0
    };
    for (int i = 0; i < 4; ++i)
        if (s[i] < 32 || s[i] > 126) s[i] = '?';
    return s;
}

static std::string LegacyLabel(const DDS_HEADER& h) {
    if (h.ddspf.dwFlags & 0x4) { // DDPF_FOURCC
        uint32_t cc = h.ddspf.dwFourCC;
        if (cc == MAKEFOURCC('D', 'X', 'T', '1')) return "DXT1 (BC1)";
        if (cc == MAKEFOURCC('D', 'X', 'T', '2')) return "DXT2 (BC2)";
        if (cc == MAKEFOURCC('D', 'X', 'T', '3')) return "DXT3 (BC2)";
        if (cc == MAKEFOURCC('D', 'X', 'T', '4')) return "DXT4 (BC3)";
        if (cc == MAKEFOURCC('D', 'X', 'T', '5')) return "DXT5 (BC3)";
        if (cc == MAKEFOURCC('A', 'T', 'I', '1') || cc == MAKEFOURCC('B', 'C', '4', 'U'))
            return "BC4_UNORM";
        if (cc == MAKEFOURCC('A', 'T', 'I', '2') || cc == MAKEFOURCC('B', 'C', '5', 'U'))
            return "BC5_UNORM";
        if (cc == MAKEFOURCC('B', 'C', '4', 'S')) return "BC4_SNORM";
        if (cc == MAKEFOURCC('B', 'C', '5', 'S')) return "BC5_SNORM";
        if (cc == MAKEFOURCC('D', 'X', '1', '0')) return "DX10";
        return std::string("FourCC ") + FourCCStr(cc);
    }
    if (h.ddspf.dwRGBBitCount == 32) {
        if (h.ddspf.dwRBitMask == 0x00FF0000) return "B8G8R8A8_UNORM";
        return "R8G8B8A8_UNORM";
    }
    if (h.ddspf.dwRGBBitCount == 16) return "R8G8 / R16";
    if (h.ddspf.dwRGBBitCount == 8) return "R8_UNORM";
    if (h.ddspf.dwRGBBitCount)
        return std::to_string(h.ddspf.dwRGBBitCount) + "-bit RGB";
    return "Unknown";
}

static int ApproxBppLegacy(const DDS_HEADER& h) {
    if (h.ddspf.dwFlags & 0x4) {
        uint32_t cc = h.ddspf.dwFourCC;
        if (cc == MAKEFOURCC('D', 'X', 'T', '1') || cc == MAKEFOURCC('A', 'T', 'I', '1') ||
            cc == MAKEFOURCC('B', 'C', '4', 'U') || cc == MAKEFOURCC('B', 'C', '4', 'S'))
            return 4;
        if (cc == MAKEFOURCC('D', 'X', 'T', '2') || cc == MAKEFOURCC('D', 'X', 'T', '3') ||
            cc == MAKEFOURCC('D', 'X', 'T', '4') || cc == MAKEFOURCC('D', 'X', 'T', '5') ||
            cc == MAKEFOURCC('A', 'T', 'I', '2') || cc == MAKEFOURCC('B', 'C', '5', 'U') ||
            cc == MAKEFOURCC('B', 'C', '5', 'S'))
            return 8;
        return 0;
    }
    return (int)h.ddspf.dwRGBBitCount;
}

} // namespace

bool Parse(const uint8_t* data, size_t size, Info& out) {
    out = {};
    if (!data || size < 4 + sizeof(DDS_HEADER))
        return false;

    uint32_t magic = 0;
    std::memcpy(&magic, data, 4);
    if (magic != MAKEFOURCC('D', 'D', 'S', ' '))
        return false;

    DDS_HEADER h;
    std::memcpy(&h, data + 4, sizeof(h));
    if (h.dwSize != 124 || h.ddspf.dwSize != 32)
        return false;

    out.width = (int)h.dwWidth;
    out.height = (int)h.dwHeight;
    out.depth = h.dwDepth ? (int)h.dwDepth : 1;
    out.mipCount = h.dwMipMapCount ? (int)h.dwMipMapCount : 1;
    if (out.mipCount < 1) out.mipCount = 1;
    out.isCube = (h.dwCaps2 & 0x200) != 0;   // DDSCAPS2_CUBEMAP
    out.isVolume = (h.dwCaps2 & 0x200000) != 0; // DDSCAPS2_VOLUME

    const bool dx10 = (h.ddspf.dwFlags & 0x4) &&
                      (h.ddspf.dwFourCC == MAKEFOURCC('D', 'X', '1', '0'));
    out.isDx10 = dx10;

    if (dx10) {
        if (size < 4 + sizeof(DDS_HEADER) + sizeof(DDS_HEADER_DXT10))
            return false;
        DDS_HEADER_DXT10 dx;
        std::memcpy(&dx, data + 4 + sizeof(DDS_HEADER), sizeof(dx));
        out.dxgiFormat = dx.dxgiFormat;
        out.arraySize = dx.arraySize ? (int)dx.arraySize : 1;
        if (dx.miscFlag & 0x4) // DDS_RESOURCE_MISC_TEXTURECUBE
            out.isCube = true;
        std::memcpy(out.fourCC, "DX10", 5);
        if (const char* n = DxgiName(dx.dxgiFormat))
            out.formatLabel = n;
        else
            out.formatLabel = "DXGI " + std::to_string(dx.dxgiFormat);
        out.bitDepth = ApproxBppDxgi(dx.dxgiFormat);
        // sRGB DXGI tags
        switch (dx.dxgiFormat) {
        case 29: case 72: case 75: case 78: case 91: case 93: case 99:
            out.srgb = true;
            break;
        default: break;
        }
    } else {
        if (h.ddspf.dwFlags & 0x4) {
            std::string fcc = FourCCStr(h.ddspf.dwFourCC);
            std::memcpy(out.fourCC, fcc.c_str(), (std::min)((size_t)4, fcc.size()));
        }
        out.formatLabel = LegacyLabel(h);
        out.bitDepth = ApproxBppLegacy(h);
        out.arraySize = 1;
    }

    if (out.width <= 0 || out.height <= 0 || out.width > 65536 || out.height > 65536)
        return false;
    return true;
}

bool ParseFile(const wchar_t* path, Info& out) {
    out = {};
    if (!path || !*path) return false;
    HANDLE h = CreateFileW(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    uint8_t buf[256] = {};
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, sizeof(buf), &got, nullptr);
    CloseHandle(h);
    if (!ok || got < 128) return false;
    return Parse(buf, got, out);
}

} // namespace ddsinfo
