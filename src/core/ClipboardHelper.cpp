#include "ClipboardHelper.h"
#include "Logger.h"

#include <windows.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <stb_image.h>
#include <stb_image_write.h>

namespace ClipboardHelper {
namespace {

// Sequence number right after our last successful SetClipboardData.
// External copy bumps GetClipboardSequenceNumber() → prefer system paste.
DWORD g_LastOwnedClipboardSeq = 0;

bool OpenClipboardRetry(int attempts = 20) {
    for (int i = 0; i < attempts; ++i) {
        if (OpenClipboard(nullptr))
            return true;
        Sleep(5);
    }
    return false;
}

UINT PngClipboardFormat() {
    static UINT s_fmt = 0;
    if (!s_fmt)
        s_fmt = RegisterClipboardFormatW(L"PNG");
    return s_fmt;
}

// Some apps also register image/png (rare on Windows; harmless to check).
UINT ImagePngClipboardFormat() {
    static UINT s_fmt = 0;
    if (!s_fmt)
        s_fmt = RegisterClipboardFormatA("image/png");
    return s_fmt;
}

uint8_t FloatToU8(float v) {
    return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

bool RgbaFloatFromStb(const stbi_uc* data, int w, int h, std::vector<float>& out) {
    if (!data || w <= 0 || h <= 0) return false;
    out.resize((size_t)w * (size_t)h * 4);
    for (size_t i = 0, n = (size_t)w * h; i < n; ++i) {
        out[i * 4 + 0] = data[i * 4 + 0] / 255.0f;
        out[i * 4 + 1] = data[i * 4 + 1] / 255.0f;
        out[i * 4 + 2] = data[i * 4 + 2] / 255.0f;
        out[i * 4 + 3] = data[i * 4 + 3] / 255.0f;
    }
    return true;
}

bool DecodePngBytes(const void* data, size_t size, std::vector<float>& out, int& w, int& h) {
    if (!data || size < 8) return false;
    int channels = 0;
    stbi_uc* rgba = stbi_load_from_memory(
        static_cast<const stbi_uc*>(data), static_cast<int>(size),
        &w, &h, &channels, 4);
    if (!rgba) return false;
    bool ok = RgbaFloatFromStb(rgba, w, h, out);
    stbi_image_free(rgba);
    return ok;
}

// Encode RGBA float → PNG bytes via stb.
bool EncodePng(const std::vector<float>& rgba, int width, int height, std::vector<uint8_t>& outPng) {
    if (rgba.empty() || width <= 0 || height <= 0) return false;
    std::vector<uint8_t> u8((size_t)width * height * 4);
    for (size_t i = 0, n = (size_t)width * height; i < n; ++i) {
        u8[i * 4 + 0] = FloatToU8(rgba[i * 4 + 0]);
        u8[i * 4 + 1] = FloatToU8(rgba[i * 4 + 1]);
        u8[i * 4 + 2] = FloatToU8(rgba[i * 4 + 2]);
        u8[i * 4 + 3] = FloatToU8(rgba[i * 4 + 3]);
    }
    struct Ctx { std::vector<uint8_t>* dst; };
    Ctx ctx{ &outPng };
    outPng.clear();
    auto writeFn = [](void* context, void* data, int size) {
        auto* c = static_cast<Ctx*>(context);
        const auto* p = static_cast<const uint8_t*>(data);
        c->dst->insert(c->dst->end(), p, p + size);
    };
    int ok = stbi_write_png_to_func(writeFn, &ctx, width, height, 4, u8.data(), width * 4);
    return ok != 0 && !outPng.empty();
}

HGLOBAL MakeGlobal(const void* data, size_t size) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!h) return nullptr;
    void* p = GlobalLock(h);
    if (!p) {
        GlobalFree(h);
        return nullptr;
    }
    std::memcpy(p, data, size);
    GlobalUnlock(h);
    return h;
}

// ---- DIB decode (CF_DIB / CF_DIBV5) ----

const uint8_t* DibPixelBase(const BITMAPINFOHEADER* bih, size_t memBytes, size_t& outMax) {
    if (!bih || bih->biSize < sizeof(BITMAPINFOHEADER)) return nullptr;
    size_t offset = bih->biSize;

    // BI_BITFIELDS with classic BITMAPINFOHEADER: 3 DWORD masks follow the header.
    if (bih->biCompression == BI_BITFIELDS && bih->biSize == sizeof(BITMAPINFOHEADER))
        offset += 12;

    // Color table for ≤8 bpp
    if (bih->biBitCount <= 8) {
        DWORD colors = bih->biClrUsed;
        if (colors == 0)
            colors = 1u << bih->biBitCount;
        offset += (size_t)colors * sizeof(RGBQUAD);
    } else if (bih->biClrUsed > 0 && bih->biBitCount < 16) {
        offset += (size_t)bih->biClrUsed * sizeof(RGBQUAD);
    }

    if (offset >= memBytes) return nullptr;
    outMax = memBytes - offset;
    return reinterpret_cast<const uint8_t*>(bih) + offset;
}

bool DecodeDibMemory(const void* mem, size_t memBytes,
                     std::vector<float>& out, int& outW, int& outH) {
    if (!mem || memBytes < sizeof(BITMAPINFOHEADER)) return false;
    const auto* bih = reinterpret_cast<const BITMAPINFOHEADER*>(mem);

    int width = bih->biWidth;
    int height = bih->biHeight;
    bool bottomUp = true;
    if (height < 0) {
        bottomUp = false;
        height = -height;
    }
    if (width <= 0 || height <= 0) return false;

    const int bitCount = bih->biBitCount;
    // Support 24/32 primarily; 8-bit palette optional later.
    if (bitCount != 24 && bitCount != 32) {
        Logger::Get().Warn("Clipboard DIB: unsupported bit depth " + std::to_string(bitCount));
        return false;
    }

    size_t pixMax = 0;
    const uint8_t* srcPixels = DibPixelBase(bih, memBytes, pixMax);
    if (!srcPixels) return false;

    const int bytesPerPixel = bitCount / 8;
    const int rowStride = ((width * bytesPerPixel + 3) & ~3);
    if ((size_t)rowStride * (size_t)height > pixMax + rowStride) {
        // Soft check — some producers leave biSizeImage = 0
        if (bih->biSizeImage != 0 && bih->biSizeImage > pixMax) {
            Logger::Get().Error("Clipboard DIB: pixel data truncated");
            return false;
        }
    }

    // Alpha presence: 32-bit with non-zero alpha channel, or BITMAPV5 with valid alpha mask.
    bool trustAlpha = (bitCount == 32);
    if (bih->biSize >= sizeof(BITMAPV5HEADER)) {
        const auto* v5 = reinterpret_cast<const BITMAPV5HEADER*>(bih);
        // If alpha mask is zero, many apps still store alpha in the high byte of BI_RGB/BITFIELDS.
        if (v5->bV5AlphaMask == 0 && bih->biCompression == BI_RGB)
            trustAlpha = true; // still read byte 3; may be 0
    }

    out.assign((size_t)width * height * 4, 0.0f);
    outW = width;
    outH = height;

    bool anyAlpha = false;
    bool allOpaque = true;

    for (int y = 0; y < height; ++y) {
        int destY = bottomUp ? (height - 1 - y) : y;
        const uint8_t* srcRow = srcPixels + (size_t)y * rowStride;
        for (int x = 0; x < width; ++x) {
            const int s = x * bytesPerPixel;
            const uint8_t b = srcRow[s + 0];
            const uint8_t g = srcRow[s + 1];
            const uint8_t r = srcRow[s + 2];
            uint8_t a = 255;
            if (bytesPerPixel == 4 && trustAlpha) {
                a = srcRow[s + 3];
                if (a < 255) allOpaque = false;
                if (a > 0) anyAlpha = true;
            }
            const size_t di = ((size_t)destY * width + x) * 4;
            out[di + 0] = r / 255.0f;
            out[di + 1] = g / 255.0f;
            out[di + 2] = b / 255.0f;
            out[di + 3] = a / 255.0f;
        }
    }

    // CF_DIB 32-bit from some apps has A=0 for every pixel (unused alpha slot).
    // If every alpha is 0, treat as opaque — otherwise paste is invisible.
    if (bitCount == 32 && !anyAlpha) {
        for (size_t i = 0, n = (size_t)width * height; i < n; ++i)
            out[i * 4 + 3] = 1.0f;
        Logger::Get().Info("Clipboard DIB: all-zero alpha treated as opaque");
    }
    (void)allOpaque;
    return true;
}

bool PasteFromRegisteredFormat(UINT fmt, std::vector<float>& out, int& w, int& h) {
    if (!fmt || !IsClipboardFormatAvailable(fmt)) return false;
    HANDLE handle = GetClipboardData(fmt);
    if (!handle) return false;
    SIZE_T sz = GlobalSize(handle);
    const void* p = GlobalLock(handle);
    if (!p) return false;
    bool ok = DecodePngBytes(p, static_cast<size_t>(sz), out, w, h);
    GlobalUnlock(handle);
    if (ok) {
        Logger::Get().Info("Clipboard: pasted PNG " + std::to_string(w) + "x" +
                           std::to_string(h) + " (fmt=" + std::to_string(fmt) + ")");
    }
    return ok;
}

bool PasteFromDibFormat(UINT fmt, std::vector<float>& out, int& w, int& h) {
    if (!IsClipboardFormatAvailable(fmt)) return false;
    HANDLE handle = GetClipboardData(fmt);
    if (!handle) return false;
    SIZE_T sz = GlobalSize(handle);
    const void* p = GlobalLock(handle);
    if (!p) return false;
    bool ok = DecodeDibMemory(p, static_cast<size_t>(sz), out, w, h);
    GlobalUnlock(handle);
    if (ok) {
        Logger::Get().Info(std::string("Clipboard: pasted ") +
                           (fmt == CF_DIBV5 ? "CF_DIBV5 " : "CF_DIB ") +
                           std::to_string(w) + "x" + std::to_string(h));
    }
    return ok;
}

bool PasteFromBitmapHandle(std::vector<float>& out, int& w, int& h) {
    if (!IsClipboardFormatAvailable(CF_BITMAP)) return false;
    HBITMAP hbm = (HBITMAP)GetClipboardData(CF_BITMAP);
    if (!hbm) return false;

    BITMAP bm = {};
    if (GetObject(hbm, sizeof(bm), &bm) == 0 || bm.bmWidth <= 0 || bm.bmHeight <= 0)
        return false;

    w = bm.bmWidth;
    h = bm.bmHeight;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    std::vector<uint8_t> bgra((size_t)w * h * 4);
    HDC hdc = GetDC(nullptr);
    if (!hdc) return false;
    int got = GetDIBits(hdc, hbm, 0, (UINT)h, bgra.data(), &bmi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, hdc);
    if (got == 0) return false;

    out.resize((size_t)w * h * 4);
    bool anyA = false;
    for (size_t i = 0, n = (size_t)w * h; i < n; ++i) {
        out[i * 4 + 0] = bgra[i * 4 + 2] / 255.0f;
        out[i * 4 + 1] = bgra[i * 4 + 1] / 255.0f;
        out[i * 4 + 2] = bgra[i * 4 + 0] / 255.0f;
        uint8_t a = bgra[i * 4 + 3];
        if (a > 0) anyA = true;
        out[i * 4 + 3] = a / 255.0f;
    }
    if (!anyA) {
        for (size_t i = 0, n = (size_t)w * h; i < n; ++i)
            out[i * 4 + 3] = 1.0f;
    }
    Logger::Get().Info("Clipboard: pasted CF_BITMAP " + std::to_string(w) + "x" + std::to_string(h));
    return true;
}

// Build classic CF_DIB (BITMAPINFOHEADER + bottom-up BGRA).
HGLOBAL MakeDib32(const std::vector<float>& rgba, int width, int height) {
    const DWORD pixelBytes = (DWORD)width * height * 4;
    const DWORD dibSize = sizeof(BITMAPINFOHEADER) + pixelBytes;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, dibSize);
    if (!hMem) return nullptr;
    char* pMem = (char*)GlobalLock(hMem);
    if (!pMem) {
        GlobalFree(hMem);
        return nullptr;
    }

    auto* bih = reinterpret_cast<BITMAPINFOHEADER*>(pMem);
    std::memset(bih, 0, sizeof(*bih));
    bih->biSize = sizeof(BITMAPINFOHEADER);
    bih->biWidth = width;
    bih->biHeight = height; // bottom-up
    bih->biPlanes = 1;
    bih->biBitCount = 32;
    bih->biCompression = BI_RGB;
    bih->biSizeImage = pixelBytes;

    auto* dest = reinterpret_cast<uint8_t*>(pMem + sizeof(BITMAPINFOHEADER));
    for (int y = 0; y < height; ++y) {
        int srcY = height - 1 - y;
        for (int x = 0; x < width; ++x) {
            size_t si = ((size_t)srcY * width + x) * 4;
            size_t di = ((size_t)y * width + x) * 4;
            dest[di + 0] = FloatToU8(rgba[si + 2]);
            dest[di + 1] = FloatToU8(rgba[si + 1]);
            dest[di + 2] = FloatToU8(rgba[si + 0]);
            dest[di + 3] = FloatToU8(rgba[si + 3]);
        }
    }
    GlobalUnlock(hMem);
    return hMem;
}

// CF_DIBV5 with explicit alpha mask (preferred by many modern apps alongside PNG).
HGLOBAL MakeDibV5_32(const std::vector<float>& rgba, int width, int height) {
    const DWORD pixelBytes = (DWORD)width * height * 4;
    const DWORD dibSize = sizeof(BITMAPV5HEADER) + pixelBytes;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, dibSize);
    if (!hMem) return nullptr;
    char* pMem = (char*)GlobalLock(hMem);
    if (!pMem) {
        GlobalFree(hMem);
        return nullptr;
    }

    auto* v5 = reinterpret_cast<BITMAPV5HEADER*>(pMem);
    std::memset(v5, 0, sizeof(*v5));
    v5->bV5Size = sizeof(BITMAPV5HEADER);
    v5->bV5Width = width;
    v5->bV5Height = height;
    v5->bV5Planes = 1;
    v5->bV5BitCount = 32;
    v5->bV5Compression = BI_BITFIELDS;
    v5->bV5SizeImage = pixelBytes;
    v5->bV5RedMask   = 0x00FF0000;
    v5->bV5GreenMask = 0x0000FF00;
    v5->bV5BlueMask  = 0x000000FF;
    v5->bV5AlphaMask = 0xFF000000;
    v5->bV5CSType = LCS_sRGB;
    v5->bV5Intent = LCS_GM_IMAGES;

    auto* dest = reinterpret_cast<uint8_t*>(pMem + sizeof(BITMAPV5HEADER));
    for (int y = 0; y < height; ++y) {
        int srcY = height - 1 - y;
        for (int x = 0; x < width; ++x) {
            size_t si = ((size_t)srcY * width + x) * 4;
            size_t di = ((size_t)y * width + x) * 4;
            dest[di + 0] = FloatToU8(rgba[si + 2]); // B
            dest[di + 1] = FloatToU8(rgba[si + 1]); // G
            dest[di + 2] = FloatToU8(rgba[si + 0]); // R
            dest[di + 3] = FloatToU8(rgba[si + 3]); // A
        }
    }
    GlobalUnlock(hMem);
    return hMem;
}

} // namespace

bool HasClipboardImage() {
    if (!OpenClipboardRetry()) return false;
    const UINT png = PngClipboardFormat();
    const UINT ipng = ImagePngClipboardFormat();
    bool has =
        (png && IsClipboardFormatAvailable(png)) ||
        (ipng && IsClipboardFormatAvailable(ipng)) ||
        IsClipboardFormatAvailable(CF_DIBV5) ||
        IsClipboardFormatAvailable(CF_DIB) ||
        IsClipboardFormatAvailable(CF_BITMAP);
    CloseClipboard();
    return has;
}

bool IsSystemClipboardNewerThanLastCopy() {
    return GetClipboardSequenceNumber() != g_LastOwnedClipboardSeq;
}

bool CopyImageToClipboard(const std::vector<float>& rgbaPixels, int width, int height) {
    if (rgbaPixels.empty() || width <= 0 || height <= 0) return false;
    if ((size_t)width * height * 4 > rgbaPixels.size()) return false;

    std::vector<uint8_t> pngBytes;
    const bool havePng = EncodePng(rgbaPixels, width, height, pngBytes);

    HGLOBAL hPng = havePng ? MakeGlobal(pngBytes.data(), pngBytes.size()) : nullptr;
    HGLOBAL hDibV5 = MakeDibV5_32(rgbaPixels, width, height);
    HGLOBAL hDib = MakeDib32(rgbaPixels, width, height);

    if (!hPng && !hDibV5 && !hDib) {
        Logger::Get().Error("Clipboard: failed to allocate image buffers");
        return false;
    }

    if (!OpenClipboardRetry()) {
        if (hPng) GlobalFree(hPng);
        if (hDibV5) GlobalFree(hDibV5);
        if (hDib) GlobalFree(hDib);
        Logger::Get().Error("Clipboard: OpenClipboard failed (copy)");
        return false;
    }

    EmptyClipboard();
    bool any = false;

    if (hPng) {
        UINT pngFmt = PngClipboardFormat();
        if (pngFmt && SetClipboardData(pngFmt, hPng)) {
            any = true;
            hPng = nullptr; // ownership transferred
        }
    }
    if (hDibV5) {
        if (SetClipboardData(CF_DIBV5, hDibV5)) {
            any = true;
            hDibV5 = nullptr;
        }
    }
    if (hDib) {
        if (SetClipboardData(CF_DIB, hDib)) {
            any = true;
            hDib = nullptr;
        }
    }

    // Capture sequence after our write so we can detect external overwrites.
    g_LastOwnedClipboardSeq = GetClipboardSequenceNumber();
    CloseClipboard();

    if (hPng) GlobalFree(hPng);
    if (hDibV5) GlobalFree(hDibV5);
    if (hDib) GlobalFree(hDib);

    if (!any) {
        Logger::Get().Error("Clipboard: SetClipboardData failed for all formats");
        return false;
    }

    Logger::Get().Info("Clipboard: copied image " + std::to_string(width) + "x" +
                       std::to_string(height) + (havePng ? " (PNG+DIB)" : " (DIB)"));
    return true;
}

bool PasteImageFromClipboard(std::vector<float>& outRgbaPixels, int& outWidth, int& outHeight) {
    outRgbaPixels.clear();
    outWidth = outHeight = 0;

    if (!OpenClipboardRetry()) {
        Logger::Get().Error("Clipboard: OpenClipboard failed (paste)");
        return false;
    }

    bool ok = false;

    // 1) PNG — real alpha from Chrome / Blender / Photoshop / Paint.NET / etc.
    if (!ok) ok = PasteFromRegisteredFormat(PngClipboardFormat(), outRgbaPixels, outWidth, outHeight);
    if (!ok) ok = PasteFromRegisteredFormat(ImagePngClipboardFormat(), outRgbaPixels, outWidth, outHeight);

    // 2) DIBV5 — alpha mask
    if (!ok) ok = PasteFromDibFormat(CF_DIBV5, outRgbaPixels, outWidth, outHeight);

    // 3) Classic DIB
    if (!ok) ok = PasteFromDibFormat(CF_DIB, outRgbaPixels, outWidth, outHeight);

    // 4) HBITMAP fallback (often opaque)
    if (!ok) ok = PasteFromBitmapHandle(outRgbaPixels, outWidth, outHeight);

    CloseClipboard();

    if (!ok) {
        Logger::Get().Warn("Clipboard: no supported image format available");
        return false;
    }
    return true;
}

} // namespace ClipboardHelper
