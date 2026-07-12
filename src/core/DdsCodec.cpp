#include "DdsCodec.h"
#include "Logger.h"
#include "PathUtil.h"
#include "MemoryStats.h"
#include "ConfigManager.h"
#include "ImageManager.h"
#include "TexconvHelper.h"

#include <DirectXTex.h>
#include <filesystem>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

using namespace DirectX;

namespace DdsCodec {
namespace {

static std::wstring ToWidePath(const std::string& utf8Path) {
#ifdef _WIN32
    return PathUtil::Utf8ToWide(PathUtil::NormalizeToUtf8Path(utf8Path));
#else
    return std::wstring(utf8Path.begin(), utf8Path.end());
#endif
}

static std::string HrHex(HRESULT hr) {
    char b[16];
    snprintf(b, sizeof(b), "%08X", (unsigned)(uint32_t)hr);
    return b;
}

static void FillFromMeta(const TexMetadata& m, SourceInfo& out) {
    out = {};
    out.dxgi = m.format;
    out.width = (int)m.width;
    out.height = (int)m.height;
    out.depth = (int)(m.depth ? m.depth : 1);
    out.mipCount = (int)(m.mipLevels ? m.mipLevels : 1);
    out.arraySize = (int)(m.arraySize ? m.arraySize : 1);
    out.isCube = m.IsCubemap();
    out.isVolume = m.IsVolumemap();
    out.isDx10 = true;
    out.srgb = IsSRGB(m.format);
    out.formatLabel = FormatLabel(m.format);
    out.uiLabel = UiLabelFromDxgi(m.format);
    out.suggestedDepth = SuggestDocumentDepth(m.format);

    switch (m.format) {
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
        out.singleChannel = true;
        break;
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
        out.dualChannel = true;
        break;
    default:
        break;
    }
}

static TEX_FILTER_FLAGS MapMipFilter(const std::string& f) {
    if (f == "Point" || f == "Nearest Neighbor" || f == "POINT") return TEX_FILTER_POINT;
    if (f == "Box" || f == "BOX") return TEX_FILTER_BOX;
    if (f == "Linear" || f == "Bilinear" || f == "LINEAR") return TEX_FILTER_LINEAR;
    if (f == "Fant" || f == "FANT") return TEX_FILTER_FANT;
    if (f == "Lanczos") return TEX_FILTER_TRIANGLE;
    // Cubic / Bicubic / default
    return TEX_FILTER_CUBIC;
}

static TEX_COMPRESS_FLAGS MapCompressFlags(const std::string& speed, DXGI_FORMAT fmt) {
    TEX_COMPRESS_FLAGS fl = TEX_COMPRESS_DEFAULT;
#ifdef _OPENMP
    fl = TEX_COMPRESS_FLAGS(fl | TEX_COMPRESS_PARALLEL);
#endif
    // BC7 / BC6H quality
    if (fmt == DXGI_FORMAT_BC7_UNORM || fmt == DXGI_FORMAT_BC7_UNORM_SRGB ||
        fmt == DXGI_FORMAT_BC6H_UF16 || fmt == DXGI_FORMAT_BC6H_SF16) {
        if (speed == "Fast")
            fl = TEX_COMPRESS_FLAGS(fl | TEX_COMPRESS_BC7_QUICK);
        if (speed == "Slow" || speed == "Best")
            fl = TEX_COMPRESS_FLAGS(fl | TEX_COMPRESS_BC7_USE_3SUBSETS);
    }
    // No TEX_COMPRESS_SRGB_* here. Editor U8 is display-referred (same as old bcdec).
    // For *_SRGB DDS targets we compress as the linear sibling then OverrideFormat
    // (see SaveScratch) so Compress never applies OETF to already-sRGB bytes.
    return fl;
}

// 8-bit RGBA layouts we store as opaque display-referred bytes (no color-space math).
static bool IsDisplayRgba8Layout(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

static bool IsBgraLayout(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

static void CopyImageToRgba8Tight(const Image& img, std::vector<uint8_t>& out, bool swapBGRA) {
    const int w = (int)img.width;
    const int h = (int)img.height;
    out.resize((size_t)w * (size_t)h * 4u);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = img.pixels + y * img.rowPitch;
        uint8_t* dst = out.data() + (size_t)y * w * 4;
        if (!swapBGRA) {
            std::memcpy(dst, row, (size_t)w * 4);
        } else {
            for (int x = 0; x < w; ++x) {
                dst[x*4+0] = row[x*4+2];
                dst[x*4+1] = row[x*4+1];
                dst[x*4+2] = row[x*4+0];
                dst[x*4+3] = row[x*4+3];
            }
        }
    }
}

static bool ImageToTileCache(const Image& img, TileCache& outCache, int depthHint) {
    const int w = (int)img.width;
    const int h = (int)img.height;
    if (w <= 0 || h <= 0 || !img.pixels) return false;

    CanvasPixelFormat store = CanvasPixelFormat::RGBA8;
    if (depthHint >= 2) store = CanvasPixelFormat::RGBA32F;
    else if (depthHint == 1) store = CanvasPixelFormat::RGBA16F;

    // --- U8 document: keep display-referred bytes. No sRGB↔linear. ---
    if (store == CanvasPixelFormat::RGBA8) {
        outCache.Init(w, h, CanvasPixelFormat::RGBA8);
        std::vector<uint8_t> tight;

        if (IsDisplayRgba8Layout(img.format)) {
            // Bit-identical import (sRGB tag is metadata only — same as legacy bcdec path)
            CopyImageToRgba8Tight(img, tight, IsBgraLayout(img.format));
            outCache.ImportRGBA8(tight.data(), w, h);
            return true;
        }

        // Real format change (R10G10B10A2, R8, R8G8, 565, …) — expand without sRGB curve
        ScratchImage converted;
        // Force non-sRGB path: treat source as raw UNORM even if tagged _SRGB
        Image raw = img;
        if (IsSRGB(raw.format)) {
            // Reinterpret tag only — Convert would apply EOTF and darken midtones
            if (raw.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
                raw.format = DXGI_FORMAT_R8G8B8A8_UNORM;
            else if (raw.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
                raw.format = DXGI_FORMAT_B8G8R8A8_UNORM;
            else if (raw.format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB)
                raw.format = DXGI_FORMAT_B8G8R8X8_UNORM;
        }
        HRESULT hr = Convert(raw, DXGI_FORMAT_R8G8B8A8_UNORM,
                             TEX_FILTER_FORCE_NON_WIC, TEX_THRESHOLD_DEFAULT, converted);
        if (FAILED(hr) || !converted.GetImages()) {
            Logger::Get().ErrorTag("dds", "Convert to RGBA8 failed hr=0x" + HrHex(hr));
            return false;
        }
        CopyImageToRgba8Tight(*converted.GetImages(), tight, false);
        outCache.ImportRGBA8(tight.data(), w, h);
        return true;
    }

    // --- F16/F32: numeric expand (needed for HDR / true float sources) ---
    ScratchImage converted;
    DXGI_FORMAT want = DXGI_FORMAT_R32G32B32A32_FLOAT;
    const Image* srcImg = &img;
    if (img.format != want) {
        // For float targets, Convert is appropriate. Avoid SRGB→linear on 8-bit sRGB
        // by only taking this path when depthHint requested float (HDR sources).
        Image raw = img;
        if (IsSRGB(raw.format) && FormatDataType(raw.format) == FORMAT_TYPE_UNORM) {
            // Still avoid accidental EOTF when promoting 8-bit sRGB to float for editing
            // — store as linear 0..1 = byte/255 (display-referred float), not scene-linear.
            // Strip SRGB tag so Convert does byte/255 only.
            if (raw.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
                raw.format = DXGI_FORMAT_R8G8B8A8_UNORM;
            else if (raw.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
                raw.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        }
        HRESULT hr = Convert(raw, want, TEX_FILTER_FORCE_NON_WIC, TEX_THRESHOLD_DEFAULT, converted);
        if (FAILED(hr) || !converted.GetImages()) {
            Logger::Get().ErrorTag("dds", "Convert to RGBA32F failed hr=0x" + HrHex(hr));
            return false;
        }
        srcImg = converted.GetImages();
    }

    outCache.Init(w, h, CanvasPixelFormat::RGBA32F);
    if (srcImg->rowPitch == (size_t)w * 16)
        outCache.ImportRGBA32F(reinterpret_cast<const float*>(srcImg->pixels), w, h);
    else {
        std::vector<float> tight((size_t)w * h * 4);
        for (int y = 0; y < h; ++y)
            std::memcpy(tight.data() + (size_t)y * w * 4,
                        srcImg->pixels + y * srcImg->rowPitch, (size_t)w * 16);
        outCache.ImportRGBA32F(tight.data(), w, h);
    }
    if (store == CanvasPixelFormat::RGBA16F)
        outCache.ConvertFormat(CanvasPixelFormat::RGBA16F);
    return true;
}

static bool SaveScratch(const std::string& utf8Path, ScratchImage& image, const SaveOptions& opt) {
    if (!image.GetImages()) return false;

    ScratchImage chain;
    // image already may have mips from caller; ensure we operate on a stable ScratchImage
    chain = std::move(image);

    DXGI_FORMAT target = opt.format;
    ScratchImage finalImg;

    const Image* images = chain.GetImages();
    size_t nimg = chain.GetImageCount();
    TexMetadata m = chain.GetMetadata();

    if (IsCompressed(target)) {
        // Canvas U8 is display-referred. Compress as *linear* BC sibling, then retag *_SRGB.
        // Encoding as BC7_UNORM_SRGB makes DirectXTex apply OETF → washed/bleached Diffuse.
        DXGI_FORMAT compressFmt = target;
        if (target == DXGI_FORMAT_BC1_UNORM_SRGB) compressFmt = DXGI_FORMAT_BC1_UNORM;
        else if (target == DXGI_FORMAT_BC2_UNORM_SRGB) compressFmt = DXGI_FORMAT_BC2_UNORM;
        else if (target == DXGI_FORMAT_BC3_UNORM_SRGB) compressFmt = DXGI_FORMAT_BC3_UNORM;
        else if (target == DXGI_FORMAT_BC7_UNORM_SRGB) compressFmt = DXGI_FORMAT_BC7_UNORM;

        DXGI_FORMAT srcNeed = (compressFmt == DXGI_FORMAT_BC6H_UF16 || compressFmt == DXGI_FORMAT_BC6H_SF16)
            ? DXGI_FORMAT_R32G32B32A32_FLOAT
            : DXGI_FORMAT_R8G8B8A8_UNORM;
        ScratchImage conv;
        const Image* srcImages = images;
        size_t srcCount = nimg;
        TexMetadata srcMeta = m;
        if (m.format != srcNeed) {
            HRESULT hr = Convert(images, nimg, m, srcNeed, TEX_FILTER_FORCE_NON_WIC,
                                 TEX_THRESHOLD_DEFAULT, conv);
            if (FAILED(hr)) {
                Logger::Get().ErrorTag("dds", "Convert pre-BC failed hr=0x" + HrHex(hr));
                return false;
            }
            srcImages = conv.GetImages();
            srcCount = conv.GetImageCount();
            srcMeta = conv.GetMetadata();
        }
        TEX_COMPRESS_FLAGS cflags = MapCompressFlags(opt.compressionSpeed, compressFmt);
        HRESULT hr = Compress(srcImages, srcCount, srcMeta, compressFmt, cflags,
                              TEX_THRESHOLD_DEFAULT, finalImg);
        if (FAILED(hr)) {
            Logger::Get().ErrorTag("dds", "Compress failed hr=0x" + HrHex(hr));
            return false;
        }
        if (compressFmt != target)
            finalImg.OverrideFormat(target);
    } else if (m.format != target) {
        // Prefer tag-only when both ends are 8-bit RGBA (sRGB vs linear DXGI label)
        if (IsDisplayRgba8Layout(m.format) && IsDisplayRgba8Layout(target) &&
            !IsBgraLayout(m.format) && !IsBgraLayout(target)) {
            finalImg = std::move(chain);
            finalImg.OverrideFormat(target);
        } else {
            HRESULT hr = Convert(images, nimg, m, target, TEX_FILTER_FORCE_NON_WIC,
                                 TEX_THRESHOLD_DEFAULT, finalImg);
            if (FAILED(hr)) {
                Logger::Get().ErrorTag("dds", "Convert to target failed hr=0x" + HrHex(hr));
                return false;
            }
        }
    } else {
        finalImg = std::move(chain);
    }

    std::wstring wpath = ToWidePath(utf8Path);
    HRESULT hr = SaveToDDSFile(finalImg.GetImages(), finalImg.GetImageCount(),
                               finalImg.GetMetadata(), DDS_FLAGS_NONE, wpath.c_str());
    if (FAILED(hr)) {
        Logger::Get().ErrorTag("dds", "SaveToDDSFile failed hr=0x" + HrHex(hr) + " " + utf8Path);
        return false;
    }
    Logger::Get().InfoTag("dds",
        "Saved " + utf8Path + " [" + FormatLabel(target) + "] mips=" +
        std::to_string(finalImg.GetMetadata().mipLevels));
    return true;
}

} // namespace

// ---------------------------------------------------------------------------

const char* FormatLabel(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return "R32G32B32A32_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_UNORM: return "R16G16B16A16_UNORM";
    case DXGI_FORMAT_R32G32_FLOAT: return "R32G32_FLOAT";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_R16G16_FLOAT: return "R16G16_FLOAT";
    case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
    case DXGI_FORMAT_R8G8_UNORM: return "R8G8_UNORM";
    case DXGI_FORMAT_R8G8_SNORM: return "R8G8_SNORM";
    case DXGI_FORMAT_R16_FLOAT: return "R16_FLOAT";
    case DXGI_FORMAT_R8_UNORM: return "R8_UNORM";
    case DXGI_FORMAT_BC1_UNORM: return "BC1_UNORM";
    case DXGI_FORMAT_BC1_UNORM_SRGB: return "BC1_UNORM_SRGB";
    case DXGI_FORMAT_BC2_UNORM: return "BC2_UNORM";
    case DXGI_FORMAT_BC2_UNORM_SRGB: return "BC2_UNORM_SRGB";
    case DXGI_FORMAT_BC3_UNORM: return "BC3_UNORM";
    case DXGI_FORMAT_BC3_UNORM_SRGB: return "BC3_UNORM_SRGB";
    case DXGI_FORMAT_BC4_UNORM: return "BC4_UNORM";
    case DXGI_FORMAT_BC4_SNORM: return "BC4_SNORM";
    case DXGI_FORMAT_BC5_UNORM: return "BC5_UNORM";
    case DXGI_FORMAT_BC5_SNORM: return "BC5_SNORM";
    case DXGI_FORMAT_B5G6R5_UNORM: return "B5G6R5_UNORM";
    case DXGI_FORMAT_B5G5R5A1_UNORM: return "B5G5R5A1_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8X8_UNORM: return "B8G8R8X8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return "B8G8R8X8_UNORM_SRGB";
    case DXGI_FORMAT_BC6H_UF16: return "BC6H_UF16";
    case DXGI_FORMAT_BC6H_SF16: return "BC6H_SF16";
    case DXGI_FORMAT_BC7_UNORM: return "BC7_UNORM";
    case DXGI_FORMAT_BC7_UNORM_SRGB: return "BC7_UNORM_SRGB";
    case DXGI_FORMAT_B4G4R4A4_UNORM: return "B4G4R4A4_UNORM";
    default: {
        thread_local char buf[32];
        snprintf(buf, sizeof(buf), "DXGI_%u", (unsigned)fmt);
        return buf;
    }
    }
}

std::string UiLabelFromDxgi(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_BC1_UNORM: return "BC1 (Linear, DXT1)";
    case DXGI_FORMAT_BC1_UNORM_SRGB: return "BC1 (sRGB, DX 10+)";
    case DXGI_FORMAT_BC2_UNORM: return "BC2 (Linear, DXT3)";
    case DXGI_FORMAT_BC2_UNORM_SRGB: return "BC2 (sRGB, DX 10+)";
    case DXGI_FORMAT_BC3_UNORM: return "BC3 (Linear, DXT5)";
    case DXGI_FORMAT_BC3_UNORM_SRGB: return "BC3 (sRGB, DX 10+)";
    case DXGI_FORMAT_BC4_UNORM: return "BC4 (Linear, Unsigned)";
    case DXGI_FORMAT_BC5_UNORM: return "BC5 (Linear, Unsigned)";
    case DXGI_FORMAT_BC5_SNORM: return "BC5 (Linear, Signed)";
    case DXGI_FORMAT_BC6H_UF16: return "BC6H (Linear, Unsigned, DX 11+)";
    case DXGI_FORMAT_BC7_UNORM: return "BC7 (Linear, DX 11+)";
    case DXGI_FORMAT_BC7_UNORM_SRGB: return "BC7 (sRGB, DX 11+)";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8 (Linear, A8R8G8B8)";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8 (sRGB, DX 10+)";
    case DXGI_FORMAT_B8G8R8X8_UNORM: return "B8G8R8X8 (Linear, X8R8G8B8)";
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return "B8G8R8X8 (sRGB, DX 10+)";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8 (Linear, A8B8G8R8)";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8 (sRGB, DX 10+)";
    case DXGI_FORMAT_B5G5R5A1_UNORM: return "B5G5R5A1 (Linear, A1R5G5B5)";
    case DXGI_FORMAT_B4G4R4A4_UNORM: return "B4G4R4A4 (Linear, A4R4G4B4)";
    case DXGI_FORMAT_B5G6R5_UNORM: return "B5G6R5 (Linear, R5G6B5)";
    case DXGI_FORMAT_R8_UNORM: return "R8 (Linear, Unsigned, L8)";
    case DXGI_FORMAT_R8G8_UNORM: return "R8G8 (Linear, Unsigned)";
    case DXGI_FORMAT_R8G8_SNORM: return "R8G8 (Linear, Signed, V8U8)";
    case DXGI_FORMAT_R32_FLOAT: return "R32 (Linear, Float)";
    case DXGI_FORMAT_R16_FLOAT: return "R16_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "RGBA16_FLOAT";
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return "RGBA32_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_UNORM: return "RGBA16_UNORM";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
    default: return FormatLabel(fmt);
    }
}

bool UiLabelToDxgi(const std::string& s, DXGI_FORMAT& out) {
    // Exact DXGI names
    auto tryName = [&](const char* n, DXGI_FORMAT f) -> bool {
        if (s == n) { out = f; return true; }
        return false;
    };
    if (tryName("BC1_UNORM", DXGI_FORMAT_BC1_UNORM)) return true;
    if (tryName("BC1_UNORM_SRGB", DXGI_FORMAT_BC1_UNORM_SRGB)) return true;
    if (tryName("BC2_UNORM", DXGI_FORMAT_BC2_UNORM)) return true;
    if (tryName("BC2_UNORM_SRGB", DXGI_FORMAT_BC2_UNORM_SRGB)) return true;
    if (tryName("BC3_UNORM", DXGI_FORMAT_BC3_UNORM)) return true;
    if (tryName("BC3_UNORM_SRGB", DXGI_FORMAT_BC3_UNORM_SRGB)) return true;
    if (tryName("BC4_UNORM", DXGI_FORMAT_BC4_UNORM)) return true;
    if (tryName("BC5_UNORM", DXGI_FORMAT_BC5_UNORM)) return true;
    if (tryName("BC5_SNORM", DXGI_FORMAT_BC5_SNORM)) return true;
    if (tryName("BC6H_UF16", DXGI_FORMAT_BC6H_UF16)) return true;
    if (tryName("BC6H_SF16", DXGI_FORMAT_BC6H_SF16)) return true;
    if (tryName("BC7_UNORM", DXGI_FORMAT_BC7_UNORM)) return true;
    if (tryName("BC7_UNORM_SRGB", DXGI_FORMAT_BC7_UNORM_SRGB)) return true;
    if (tryName("R8G8B8A8_UNORM", DXGI_FORMAT_R8G8B8A8_UNORM)) return true;
    if (tryName("RGBA8_UNORM", DXGI_FORMAT_R8G8B8A8_UNORM)) return true;
    if (tryName("R8G8B8A8_UNORM_SRGB", DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)) return true;
    if (tryName("B8G8R8A8_UNORM", DXGI_FORMAT_B8G8R8A8_UNORM)) return true;
    if (tryName("R8_UNORM", DXGI_FORMAT_R8_UNORM)) return true;
    if (tryName("R8G8_UNORM", DXGI_FORMAT_R8G8_UNORM)) return true;
    if (tryName("R32_FLOAT", DXGI_FORMAT_R32_FLOAT)) return true;
    if (tryName("R16_FLOAT", DXGI_FORMAT_R16_FLOAT)) return true;
    if (tryName("RGBA16_FLOAT", DXGI_FORMAT_R16G16B16A16_FLOAT)) return true;
    if (tryName("R16G16B16A16_FLOAT", DXGI_FORMAT_R16G16B16A16_FLOAT)) return true;
    if (tryName("RGBA32_FLOAT", DXGI_FORMAT_R32G32B32A32_FLOAT)) return true;
    if (tryName("R32G32B32A32_FLOAT", DXGI_FORMAT_R32G32B32A32_FLOAT)) return true;
    if (tryName("RGBA16_UNORM", DXGI_FORMAT_R16G16B16A16_UNORM)) return true;
    if (tryName("R10G10B10A2_UNORM", DXGI_FORMAT_R10G10B10A2_UNORM)) return true;
    if (tryName("R11G11B10_FLOAT", DXGI_FORMAT_R11G11B10_FLOAT)) return true;

    // Substring match Paint.NET-style labels
    auto has = [&](const char* p) { return s.find(p) != std::string::npos; };
    if (has("BC1") && (has("sRGB") || has("SRGB"))) { out = DXGI_FORMAT_BC1_UNORM_SRGB; return true; }
    if (has("BC1")) { out = DXGI_FORMAT_BC1_UNORM; return true; }
    if (has("BC2") && (has("sRGB") || has("SRGB"))) { out = DXGI_FORMAT_BC2_UNORM_SRGB; return true; }
    if (has("BC2")) { out = DXGI_FORMAT_BC2_UNORM; return true; }
    if (has("RXGB")) { out = DXGI_FORMAT_BC3_UNORM; return true; }
    if (has("BC3") && (has("sRGB") || has("SRGB"))) { out = DXGI_FORMAT_BC3_UNORM_SRGB; return true; }
    if (has("BC3")) { out = DXGI_FORMAT_BC3_UNORM; return true; }
    if (has("BC4")) { out = DXGI_FORMAT_BC4_UNORM; return true; }
    if (has("BC5") && has("Signed")) { out = DXGI_FORMAT_BC5_SNORM; return true; }
    if (has("BC5") || has("ATI2")) { out = DXGI_FORMAT_BC5_UNORM; return true; }
    if (has("BC6H") && has("Signed")) { out = DXGI_FORMAT_BC6H_SF16; return true; }
    if (has("BC6H")) { out = DXGI_FORMAT_BC6H_UF16; return true; }
    if (has("BC7") && (has("sRGB") || has("SRGB"))) { out = DXGI_FORMAT_BC7_UNORM_SRGB; return true; }
    if (has("BC7")) { out = DXGI_FORMAT_BC7_UNORM; return true; }
    if (has("B8G8R8A8") && (has("sRGB") || has("SRGB"))) { out = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; return true; }
    if (has("B8G8R8A8") || has("A8R8G8B8")) { out = DXGI_FORMAT_B8G8R8A8_UNORM; return true; }
    if (has("B8G8R8X8") && (has("sRGB") || has("SRGB"))) { out = DXGI_FORMAT_B8G8R8X8_UNORM_SRGB; return true; }
    if (has("B8G8R8X8") || has("X8R8G8B8")) { out = DXGI_FORMAT_B8G8R8X8_UNORM; return true; }
    if (has("R8G8B8A8") && (has("sRGB") || has("SRGB"))) { out = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; return true; }
    if (has("R8G8B8A8") || has("A8B8G8R8") || has("RGBA8")) { out = DXGI_FORMAT_R8G8B8A8_UNORM; return true; }
    if (has("R8G8B8X8") || has("X8B8G8R8")) { out = DXGI_FORMAT_R8G8B8A8_UNORM; return true; }
    if (has("B5G5R5A1") || has("A1R5G5B5")) { out = DXGI_FORMAT_B5G5R5A1_UNORM; return true; }
    if (has("B4G4R4A4") || has("A4R4G4B4")) { out = DXGI_FORMAT_B4G4R4A4_UNORM; return true; }
    if (has("B5G6R5") || has("R5G6B5")) { out = DXGI_FORMAT_B5G6R5_UNORM; return true; }
    if ((has("R8 (") || has("L8")) && !has("R8G8")) { out = DXGI_FORMAT_R8_UNORM; return true; }
    if (has("R8G8") && has("Signed")) { out = DXGI_FORMAT_R8G8_SNORM; return true; }
    if (has("R8G8") || has("A8L8") || has("V8U8")) { out = DXGI_FORMAT_R8G8_UNORM; return true; }
    if (has("R32") && has("Float")) { out = DXGI_FORMAT_R32_FLOAT; return true; }
    if (has("R16_FLOAT") || (has("R16") && has("Float") && !has("R16G16"))) { out = DXGI_FORMAT_R16_FLOAT; return true; }
    if (has("RGBA16_FLOAT") || has("R16G16B16A16_FLOAT")) { out = DXGI_FORMAT_R16G16B16A16_FLOAT; return true; }
    if (has("RGBA32_FLOAT") || has("R32G32B32A32_FLOAT")) { out = DXGI_FORMAT_R32G32B32A32_FLOAT; return true; }
    if (has("RGBA16_UNORM") || has("R16G16B16A16_UNORM")) { out = DXGI_FORMAT_R16G16B16A16_UNORM; return true; }
    if (has("R10G10B10A2")) { out = DXGI_FORMAT_R10G10B10A2_UNORM; return true; }
    if (has("R11G11B10")) { out = DXGI_FORMAT_R11G11B10_FLOAT; return true; }

    out = DXGI_FORMAT_BC7_UNORM_SRGB;
    return false;
}

bool IsBlockCompressed(DXGI_FORMAT fmt) { return IsCompressed(fmt); }

bool IsFloatFormat(DXGI_FORMAT fmt) {
    return FormatDataType(fmt) == FORMAT_TYPE_FLOAT;
}

int SuggestDocumentDepth(DXGI_FORMAT fmt) {
    if (IsFloatFormat(fmt)) {
        switch (fmt) {
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R32G32B32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_D32_FLOAT:
            return 2;
        default:
            return 1;
        }
    }
    return 0;
}

bool AnalyzeMemory(const void* data, size_t size, SourceInfo& out) {
    out = {};
    if (!data || size < 128) return false;
    TexMetadata meta = {};
    HRESULT hr = GetMetadataFromDDSMemory(
        static_cast<const uint8_t*>(data), size, DDS_FLAGS_NONE, meta);
    if (FAILED(hr)) return false;
    FillFromMeta(meta, out);
    return true;
}

bool AnalyzeFile(const std::string& utf8Path, SourceInfo& out) {
    out = {};
    if (utf8Path.empty()) return false;
    TexMetadata meta = {};
    HRESULT hr = GetMetadataFromDDSFile(ToWidePath(utf8Path).c_str(), DDS_FLAGS_NONE, meta);
    if (FAILED(hr)) {
        Logger::Get().WarnTag("dds", "Analyze failed: " + utf8Path + " hr=0x" + HrHex(hr));
        return false;
    }
    FillFromMeta(meta, out);
    Logger::Get().InfoTag("dds",
        "Analyze " + out.formatLabel + " " + std::to_string(out.width) + "x" +
        std::to_string(out.height) + " mips=" + std::to_string(out.mipCount));
    return true;
}

bool LoadToTileCache(const std::string& utf8Path, TileCache& outCache,
                     int& outWidth, int& outHeight, SourceInfo& outInfo) {
    auto t0 = std::chrono::high_resolution_clock::now();
    outInfo = {};
    outWidth = outHeight = 0;

    ScratchImage image;
    TexMetadata meta = {};
    HRESULT hr = LoadFromDDSFile(ToWidePath(utf8Path).c_str(), DDS_FLAGS_NONE, &meta, image);
    if (FAILED(hr)) {
        Logger::Get().ErrorTag("dds", "LoadFromDDSFile failed: " + utf8Path + " hr=0x" + HrHex(hr));
        return false;
    }
    FillFromMeta(meta, outInfo);
    outWidth = (int)meta.width;
    outHeight = (int)meta.height;

    // Work on mip0 image 0
    const Image* img = image.GetImage(0, 0, 0);
    if (!img) return false;

    ScratchImage decompressed;
    const Image* work = img;
    if (IsCompressed(img->format)) {
        // BC6H → float. BC1–7 (incl. *_SRGB): decompress to default (often R8G8B8A8_UNORM_SRGB)
        // then strip SRGB tag via OverrideFormat — NO Convert sRGB→linear (that darkened Diffuse).
        DXGI_FORMAT decFmt = (img->format == DXGI_FORMAT_BC6H_UF16 ||
                              img->format == DXGI_FORMAT_BC6H_SF16)
            ? DXGI_FORMAT_R32G32B32A32_FLOAT
            : DXGI_FORMAT_UNKNOWN; // DirectXTex DefaultDecompress
        if (decFmt == DXGI_FORMAT_UNKNOWN) {
            // Prefer same-colorspace 8-bit target so Decompress does not EOTF
            if (IsSRGB(img->format))
                decFmt = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            else if (img->format == DXGI_FORMAT_BC4_UNORM || img->format == DXGI_FORMAT_BC4_SNORM)
                decFmt = DXGI_FORMAT_R8_UNORM;
            else if (img->format == DXGI_FORMAT_BC5_UNORM || img->format == DXGI_FORMAT_BC5_SNORM)
                decFmt = DXGI_FORMAT_R8G8_UNORM;
            else
                decFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
        }
        hr = Decompress(*img, decFmt, decompressed);
        if (FAILED(hr)) {
            Logger::Get().ErrorTag("dds", "Decompress failed hr=0x" + HrHex(hr));
            return false;
        }
        // Reinterpret sRGB-tagged 8-bit as raw UNORM (bytes unchanged) for our U8 canvas
        if (IsSRGB(decompressed.GetMetadata().format)) {
            DXGI_FORMAT bare = DXGI_FORMAT_R8G8B8A8_UNORM;
            if (decompressed.GetMetadata().format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
                bare = DXGI_FORMAT_B8G8R8A8_UNORM;
            decompressed.OverrideFormat(bare);
        }
        work = decompressed.GetImages();
        if (!work) return false;
    } else if (IsSRGB(img->format) && outInfo.suggestedDepth == 0) {
        // Uncompressed sRGB 8-bit: strip tag only (no Convert)
        Image bare = *img;
        if (bare.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
            bare.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        else if (bare.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
            bare.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        else if (bare.format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB)
            bare.format = DXGI_FORMAT_B8G8R8X8_UNORM;
        if (!ImageToTileCache(bare, outCache, outInfo.suggestedDepth))
            return false;
        goto load_done;
    }

    if (!ImageToTileCache(*work, outCache, outInfo.suggestedDepth))
        return false;
load_done:

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - t0).count();
    Logger::Get().InfoTag("dds",
        "LoadToTileCache " + utf8Path + " " + outInfo.formatLabel + " " +
        std::to_string(outWidth) + "x" + std::to_string(outHeight) +
        " depthHint=" + std::to_string(outInfo.suggestedDepth) +
        " " + std::to_string(ms) + "ms tiles=" + std::to_string(outCache.GetTileCount()));
    MemoryStats::LogSnapshot("after_DdsCodec_Load");
    return true;
}

bool SaveRgba8(const std::string& utf8Path, const uint8_t* rgba, int w, int h,
               const SaveOptions& opt) {
    if (!rgba || w <= 0 || h <= 0) return false;
    auto t0 = std::chrono::high_resolution_clock::now();

    // ---- FAST PATH: block-compressed DDS via texconv.exe ----
    // DirectXTex CPU BC7 is 10–100× slower for 2K/4K and spikes RSS hard.
    // texconv remains the production compress backend (same as pre-regression).
    if (IsCompressed(opt.format)) {
        std::string tempDir = ConfigManager::GetUserSubdirectory("temp");
        std::string tempPng = tempDir + "/rayv_export_tmp.png";
        if (!ImageManager::SaveRGBA8ToFile(tempPng, rgba, w, h)) {
            Logger::Get().ErrorTag("dds", "temp PNG write failed for texconv");
            return false;
        }
        ExportSettings es;
        es.isDds = true;
        es.ddsFormatStr = FormatLabel(opt.format); // DXGI name texconv understands
        es.advancedMode = true;
        es.compressionSpeed = opt.compressionSpeed.empty() ? "Fast" : opt.compressionSpeed;
        es.generateMipMaps = opt.generateMips; // default false from open path
        es.mipFilter = opt.mipFilter.empty() ? "Cubic" : opt.mipFilter;
        es.exportPath = utf8Path;
        bool ok = TexconvHelper::CompressDDS(tempPng, utf8Path, es);
        try {
            std::filesystem::remove(PathUtil::FromUtf8(tempPng));
        } catch (...) {}
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - t0).count();
        if (ok)
            Logger::Get().InfoTag("dds",
                "SaveRgba8 texconv " + std::string(FormatLabel(opt.format)) + " " +
                std::to_string(ms) + "ms mips=" + (opt.generateMips ? "on" : "off"));
        else
            Logger::Get().ErrorTag("dds", "texconv compress failed: " + utf8Path);
        return ok;
    }

    // ---- Uncompressed / packed DXGI: DirectXTex in-process (cheap) ----
    ScratchImage image;
    HRESULT hr = image.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, (size_t)w, (size_t)h, 1, 1);
    if (FAILED(hr)) return false;
    const Image* img = image.GetImages();
    for (int y = 0; y < h; ++y)
        std::memcpy(img->pixels + y * img->rowPitch, rgba + (size_t)y * w * 4, (size_t)w * 4);

    if (opt.generateMips) {
        ScratchImage mips;
        hr = GenerateMipMaps(*img, MapMipFilter(opt.mipFilter), 0, mips);
        if (SUCCEEDED(hr))
            image = std::move(mips);
    }

    bool ok = SaveScratch(utf8Path, image, opt);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - t0).count();
    if (ok)
        Logger::Get().InfoTag("dds", "SaveRgba8 DXTex " + std::to_string(ms) + "ms");
    return ok;
}

bool SaveRgba32F(const std::string& utf8Path, const float* rgba, int w, int h,
                 const SaveOptions& opt) {
    if (!rgba || w <= 0 || h <= 0) return false;
    ScratchImage image;
    HRESULT hr = image.Initialize2D(DXGI_FORMAT_R32G32B32A32_FLOAT, (size_t)w, (size_t)h, 1, 1);
    if (FAILED(hr)) return false;
    const Image* img = image.GetImages();
    for (int y = 0; y < h; ++y)
        std::memcpy(img->pixels + y * img->rowPitch,
                    reinterpret_cast<const uint8_t*>(rgba) + (size_t)y * w * 16,
                    (size_t)w * 16);

    if (opt.generateMips) {
        ScratchImage mips;
        hr = GenerateMipMaps(*img, MapMipFilter(opt.mipFilter), 0, mips);
        if (SUCCEEDED(hr))
            image = std::move(mips);
    }
    return SaveScratch(utf8Path, image, opt);
}

// Catalog for UI
static const char* kCatalog[] = {
    "BC1 (Linear, DXT1)", "BC1 (sRGB, DX 10+)",
    "BC2 (Linear, DXT3)", "BC2 (sRGB, DX 10+)",
    "BC3 (Linear, DXT5)", "BC3 (sRGB, DX 10+)", "BC3 (Linear, RXGB)",
    "BC4 (Linear, Unsigned)",
    "BC5 (Linear, Unsigned)", "BC5 (Linear, Signed)",
    "BC6H (Linear, Unsigned, DX 11+)",
    "BC7 (Linear, DX 11+)", "BC7 (sRGB, DX 11+)",
    "B8G8R8A8 (Linear, A8R8G8B8)", "B8G8R8A8 (sRGB, DX 10+)",
    "B8G8R8X8 (Linear, X8R8G8B8)", "B8G8R8X8 (sRGB, DX 10+)",
    "R8G8B8A8 (Linear, A8B8G8R8)", "R8G8B8A8 (sRGB, DX 10+)",
    "B5G5R5A1 (Linear, A1R5G5B5)", "B4G4R4A4 (Linear, A4R4G4B4)",
    "B5G6R5 (Linear, R5G6B5)",
    "R8 (Linear, Unsigned, L8)",
    "R8G8 (Linear, Unsigned)", "R8G8 (Linear, Signed, V8U8)",
    "R10G10B10A2_UNORM", "R11G11B10_FLOAT",
    "R32 (Linear, Float)", "R16_FLOAT",
    "RGBA16_FLOAT", "RGBA32_FLOAT", "RGBA16_UNORM", "RGBA8_UNORM",
};

int CatalogCount() { return (int)(sizeof(kCatalog) / sizeof(kCatalog[0])); }
const char* const* CatalogUiLabels() { return kCatalog; }

} // namespace DdsCodec
