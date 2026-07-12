#include "DdsThumbDecode.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#define BCDEC_IMPLEMENTATION
#include "../core/bcdec.h"

#ifndef MAKEFOURCC
#define MAKEFOURCC(ch0, ch1, ch2, ch3) \
    ((uint32_t)(uint8_t)(ch0) | ((uint32_t)(uint8_t)(ch1) << 8) | \
     ((uint32_t)(uint8_t)(ch2) << 16) | ((uint32_t)(uint8_t)(ch3) << 24))
#endif

namespace ddsthumb {
namespace {

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
    uint32_t dwSize;
    uint32_t dwFlags;
    uint32_t dwHeight;
    uint32_t dwWidth;
    uint32_t dwPitchOrLinearSize;
    uint32_t dwDepth;
    uint32_t dwMipMapCount;
    uint32_t dwReserved1[11];
    DDS_PIXELFORMAT ddspf;
    uint32_t dwCaps;
    uint32_t dwCaps2;
    uint32_t dwCaps3;
    uint32_t dwCaps4;
    uint32_t dwReserved2;
};

struct DDS_HEADER_DXT10 {
    uint32_t dxgiFormat;
    uint32_t resourceDimension;
    uint32_t miscFlag;
    uint32_t arraySize;
    uint32_t miscFlags2;
};

enum class Fmt : uint8_t {
    Unsupported = 0,
    RGBA8,
    BGRA8,
    R8,
    R8G8,
    BC1,
    BC2,
    BC3,
    BC4,
    BC5,
    BC7,
};

struct Layout {
    Fmt fmt = Fmt::Unsupported;
    int blockSize = 0;   // 0 for uncompressed
    int bytesPerPixel = 4;
    bool isBlock = false;
};

static Layout LayoutFromDxgi(uint32_t dxgi) {
    Layout L;
    switch (dxgi) {
    case 28: case 29: // R8G8B8A8_UNORM / _SRGB
        L.fmt = Fmt::RGBA8; L.bytesPerPixel = 4; break;
    case 87: case 91: // B8G8R8A8_UNORM / _SRGB
        L.fmt = Fmt::BGRA8; L.bytesPerPixel = 4; break;
    case 61: // R8_UNORM
        L.fmt = Fmt::R8; L.bytesPerPixel = 1; break;
    case 49: // R8G8_UNORM
        L.fmt = Fmt::R8G8; L.bytesPerPixel = 2; break;
    case 71: case 72: // BC1
        L.fmt = Fmt::BC1; L.isBlock = true; L.blockSize = 8; break;
    case 74: case 75: // BC2
        L.fmt = Fmt::BC2; L.isBlock = true; L.blockSize = 16; break;
    case 77: case 78: // BC3
        L.fmt = Fmt::BC3; L.isBlock = true; L.blockSize = 16; break;
    case 80: case 81: // BC4
        L.fmt = Fmt::BC4; L.isBlock = true; L.blockSize = 8; break;
    case 83: case 84: // BC5
        L.fmt = Fmt::BC5; L.isBlock = true; L.blockSize = 16; break;
    case 98: case 99: // BC7
        L.fmt = Fmt::BC7; L.isBlock = true; L.blockSize = 16; break;
    default:
        L.fmt = Fmt::Unsupported; break;
    }
    return L;
}

static Layout LayoutFromLegacy(const DDS_HEADER& h) {
    Layout L;
    const bool fourCC = (h.ddspf.dwFlags & 0x4) != 0;
    if (fourCC) {
        const uint32_t cc = h.ddspf.dwFourCC;
        if (cc == MAKEFOURCC('D', 'X', 'T', '1')) {
            L.fmt = Fmt::BC1; L.isBlock = true; L.blockSize = 8;
        } else if (cc == MAKEFOURCC('D', 'X', 'T', '3')) {
            L.fmt = Fmt::BC2; L.isBlock = true; L.blockSize = 16;
        } else if (cc == MAKEFOURCC('D', 'X', 'T', '5')) {
            L.fmt = Fmt::BC3; L.isBlock = true; L.blockSize = 16;
        } else if (cc == MAKEFOURCC('A', 'T', 'I', '1') || cc == MAKEFOURCC('B', 'C', '4', 'U')) {
            L.fmt = Fmt::BC4; L.isBlock = true; L.blockSize = 8;
        } else if (cc == MAKEFOURCC('A', 'T', 'I', '2') || cc == MAKEFOURCC('B', 'C', '5', 'U')) {
            L.fmt = Fmt::BC5; L.isBlock = true; L.blockSize = 16;
        } else if (cc == MAKEFOURCC('B', 'C', '4', 'S') || cc == MAKEFOURCC('B', 'C', '5', 'S')) {
            // signed — treat as unsigned for thumb preview
            L.fmt = (cc == MAKEFOURCC('B', 'C', '4', 'S')) ? Fmt::BC4 : Fmt::BC5;
            L.isBlock = true;
            L.blockSize = (L.fmt == Fmt::BC4) ? 8 : 16;
        }
        return L;
    }
    if (h.ddspf.dwRGBBitCount == 32) {
        if (h.ddspf.dwRBitMask == 0x00FF0000)
            L.fmt = Fmt::BGRA8;
        else
            L.fmt = Fmt::RGBA8;
        L.bytesPerPixel = 4;
    } else if (h.ddspf.dwRGBBitCount == 8) {
        L.fmt = Fmt::R8;
        L.bytesPerPixel = 1;
    } else if (h.ddspf.dwRGBBitCount == 16) {
        // R8G8: G may live in dwGBitMask or (legacy) dwABitMask
        const uint32_t r = h.ddspf.dwRBitMask;
        const uint32_t g = h.ddspf.dwGBitMask ? h.ddspf.dwGBitMask : h.ddspf.dwABitMask;
        if ((r == 0x00FF || r == 0xFF) && (g == 0xFF00)) {
            L.fmt = Fmt::R8G8;
            L.bytesPerPixel = 2;
        } else if (r == 0xFFFF || (r != 0 && h.ddspf.dwGBitMask == 0 && h.ddspf.dwBBitMask == 0)) {
            // R16-ish → preview as low 8 bits grayscale later; treat as R8G8 packed
            L.fmt = Fmt::R8G8;
            L.bytesPerPixel = 2;
        }
    }
    return L;
}

static size_t MipByteSize(int w, int h, const Layout& L) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (L.isBlock) {
        int bw = (w + 3) / 4;
        int bh = (h + 3) / 4;
        return (size_t)bw * (size_t)bh * (size_t)L.blockSize;
    }
    return (size_t)w * (size_t)h * (size_t)L.bytesPerPixel;
}

static bool DecodeMip(const uint8_t* src, size_t srcBytes, int w, int h,
                      const Layout& L, RgbaImage& out) {
    if (w < 1 || h < 1 || !src) return false;
    const size_t need = MipByteSize(w, h, L);
    if (srcBytes < need) return false;

    out.width = w;
    out.height = h;
    out.rgba.assign((size_t)w * (size_t)h * 4u, 0);

    if (!L.isBlock) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                size_t di = ((size_t)y * w + x) * 4;
                size_t si = ((size_t)y * w + x) * (size_t)L.bytesPerPixel;
                switch (L.fmt) {
                case Fmt::RGBA8:
                    out.rgba[di+0] = src[si+0];
                    out.rgba[di+1] = src[si+1];
                    out.rgba[di+2] = src[si+2];
                    out.rgba[di+3] = src[si+3];
                    break;
                case Fmt::BGRA8:
                    out.rgba[di+0] = src[si+2];
                    out.rgba[di+1] = src[si+1];
                    out.rgba[di+2] = src[si+0];
                    out.rgba[di+3] = src[si+3];
                    break;
                case Fmt::R8:
                    out.rgba[di+0] = out.rgba[di+1] = out.rgba[di+2] = src[si];
                    out.rgba[di+3] = 255;
                    break;
                case Fmt::R8G8:
                    out.rgba[di+0] = src[si+0];
                    out.rgba[di+1] = src[si+1];
                    out.rgba[di+2] = 0;
                    out.rgba[di+3] = 255;
                    break;
                default:
                    return false;
                }
            }
        }
        return true;
    }

    const int blocksW = (w + 3) / 4;
    const int blocksH = (h + 3) / 4;
    // Decode into padded block grid, then crop
    const int padW = blocksW * 4;
    const int padH = blocksH * 4;
    std::vector<uint8_t> pad((size_t)padW * (size_t)padH * 4u, 0);

    for (int by = 0; by < blocksH; ++by) {
        for (int bx = 0; bx < blocksW; ++bx) {
            const size_t blockIdx = (size_t)by * blocksW + bx;
            const uint8_t* blockSrc = src + blockIdx * (size_t)L.blockSize;
            uint8_t* blockDst = pad.data() + ((size_t)by * 4 * padW + (size_t)bx * 4) * 4;

            switch (L.fmt) {
            case Fmt::BC1:
                bcdec_bc1(blockSrc, blockDst, padW * 4);
                break;
            case Fmt::BC2:
                bcdec_bc2(blockSrc, blockDst, padW * 4);
                break;
            case Fmt::BC3:
                bcdec_bc3(blockSrc, blockDst, padW * 4);
                break;
            case Fmt::BC7:
                bcdec_bc7(blockSrc, blockDst, padW * 4);
                break;
            case Fmt::BC4: {
                uint8_t rBlock[16];
                bcdec_bc4(blockSrc, rBlock, 4);
                for (int py = 0; py < 4; ++py) {
                    for (int px = 0; px < 4; ++px) {
                        uint8_t v = rBlock[py * 4 + px];
                        uint8_t* p = blockDst + ((size_t)py * padW + px) * 4;
                        p[0] = p[1] = p[2] = v;
                        p[3] = 255;
                    }
                }
                break;
            }
            case Fmt::BC5: {
                uint8_t rgBlock[32];
                bcdec_bc5(blockSrc, rgBlock, 8);
                for (int py = 0; py < 4; ++py) {
                    for (int px = 0; px < 4; ++px) {
                        const uint8_t* s = rgBlock + (py * 4 + px) * 2;
                        uint8_t* p = blockDst + ((size_t)py * padW + px) * 4;
                        p[0] = s[0];
                        p[1] = s[1];
                        p[2] = 0;
                        p[3] = 255;
                    }
                }
                break;
            }
            default:
                return false;
            }
        }
    }

    for (int y = 0; y < h; ++y) {
        std::memcpy(out.rgba.data() + (size_t)y * w * 4,
                    pad.data() + (size_t)y * padW * 4,
                    (size_t)w * 4);
    }
    return true;
}

} // namespace

bool DecodeDdsToRgba8(const uint8_t* data, size_t size, int targetSize, RgbaImage& out) {
    out = {};
    if (!data || size < 4 + sizeof(DDS_HEADER))
        return false;

    uint32_t magic = 0;
    std::memcpy(&magic, data, 4);
    if (magic != MAKEFOURCC('D', 'D', 'S', ' '))
        return false;

    DDS_HEADER header;
    std::memcpy(&header, data + 4, sizeof(header));
    if (header.dwSize != 124 || header.ddspf.dwSize != 32)
        return false;

    const int fullW = (int)header.dwWidth;
    const int fullH = (int)header.dwHeight;
    if (fullW <= 0 || fullH <= 0 || fullW > 65536 || fullH > 65536)
        return false;

    size_t offset = 4 + sizeof(DDS_HEADER);
    Layout layout;
    const bool isDX10 = (header.ddspf.dwFlags & 0x4) &&
                        (header.ddspf.dwFourCC == MAKEFOURCC('D', 'X', '1', '0'));
    if (isDX10) {
        if (size < offset + sizeof(DDS_HEADER_DXT10))
            return false;
        DDS_HEADER_DXT10 dx10;
        std::memcpy(&dx10, data + offset, sizeof(dx10));
        offset += sizeof(dx10);
        layout = LayoutFromDxgi(dx10.dxgiFormat);
    } else {
        layout = LayoutFromLegacy(header);
    }
    if (layout.fmt == Fmt::Unsupported)
        return false;

    int mipCount = (int)header.dwMipMapCount;
    if (mipCount < 1) mipCount = 1;
    // Cap mips to what dimensions allow
    {
        int maxMips = 1;
        int tw = fullW, th = fullH;
        while (tw > 1 || th > 1) {
            tw = std::max(1, tw / 2);
            th = std::max(1, th / 2);
            ++maxMips;
        }
        if (mipCount > maxMips) mipCount = maxMips;
    }

    if (targetSize < 16) targetSize = 16;

    // Pick the smallest mip whose longest edge is still >= targetSize (or last mip).
    int pick = 0;
    int mw = fullW, mh = fullH;
    size_t skip = 0;
    for (int m = 0; m < mipCount; ++m) {
        const int longEdge = std::max(mw, mh);
        pick = m;
        if (longEdge <= targetSize)
            break;
        if (m + 1 < mipCount) {
            skip += MipByteSize(mw, mh, layout);
            mw = std::max(1, mw / 2);
            mh = std::max(1, mh / 2);
        }
    }

    // Recompute offset + size for picked mip
    mw = fullW;
    mh = fullH;
    skip = 0;
    for (int m = 0; m < pick; ++m) {
        skip += MipByteSize(mw, mh, layout);
        mw = std::max(1, mw / 2);
        mh = std::max(1, mh / 2);
    }

    if (offset + skip > size)
        return false;
    const uint8_t* mipData = data + offset + skip;
    const size_t mipAvail = size - (offset + skip);
    return DecodeMip(mipData, mipAvail, mw, mh, layout, out);
}

void ScaleRgba8(const RgbaImage& src, int outW, int outH, std::vector<uint8_t>& outRgba) {
    outRgba.assign((size_t)outW * (size_t)outH * 4u, 0);
    if (src.width <= 0 || src.height <= 0 || outW <= 0 || outH <= 0 || src.rgba.empty())
        return;

    // Fast path: identical size
    if (src.width == outW && src.height == outH) {
        outRgba = src.rgba;
        return;
    }

    // Box filter when downscaling; nearest when upscaling
    const bool down = outW <= src.width && outH <= src.height;
    for (int y = 0; y < outH; ++y) {
        for (int x = 0; x < outW; ++x) {
            uint8_t* d = outRgba.data() + ((size_t)y * outW + x) * 4;
            if (down) {
                int x0 = x * src.width / outW;
                int x1 = (x + 1) * src.width / outW;
                int y0 = y * src.height / outH;
                int y1 = (y + 1) * src.height / outH;
                if (x1 <= x0) x1 = x0 + 1;
                if (y1 <= y0) y1 = y0 + 1;
                uint32_t r = 0, g = 0, b = 0, a = 0, n = 0;
                for (int sy = y0; sy < y1; ++sy) {
                    for (int sx = x0; sx < x1; ++sx) {
                        const uint8_t* s = src.rgba.data() + ((size_t)sy * src.width + sx) * 4;
                        r += s[0]; g += s[1]; b += s[2]; a += s[3];
                        ++n;
                    }
                }
                if (n == 0) n = 1;
                d[0] = (uint8_t)(r / n);
                d[1] = (uint8_t)(g / n);
                d[2] = (uint8_t)(b / n);
                d[3] = (uint8_t)(a / n);
            } else {
                int sx = std::min(src.width - 1, x * src.width / outW);
                int sy = std::min(src.height - 1, y * src.height / outH);
                const uint8_t* s = src.rgba.data() + ((size_t)sy * src.width + sx) * 4;
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
            }
        }
    }
}

} // namespace ddsthumb
