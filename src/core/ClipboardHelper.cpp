#include "ClipboardHelper.h"
#include "Logger.h"
#include <windows.h>
#include <algorithm>

namespace ClipboardHelper {
    bool CopyImageToClipboard(const std::vector<float>& rgbaPixels, int width, int height) {
        if (rgbaPixels.empty() || width <= 0 || height <= 0) return false;

        // Size of pixel data in bytes (32-bit BGRA)
        DWORD pixelDataSize = width * height * 4;
        DWORD dibSize = sizeof(BITMAPINFOHEADER) + pixelDataSize;

        // Allocate global memory block
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, dibSize);
        if (!hMem) {
            Logger::Get().Error("Clipboard: GlobalAlloc failed.");
            return false;
        }

        // Lock block
        char* pMem = (char*)GlobalLock(hMem);
        if (!pMem) {
            GlobalFree(hMem);
            Logger::Get().Error("Clipboard: GlobalLock failed.");
            return false;
        }

        // Fill BITMAPINFOHEADER
        BITMAPINFOHEADER* bih = (BITMAPINFOHEADER*)pMem;
        bih->biSize = sizeof(BITMAPINFOHEADER);
        bih->biWidth = width;
        bih->biHeight = height; // Positive height = bottom-up DIB
        bih->biPlanes = 1;
        bih->biBitCount = 32;
        bih->biCompression = BI_RGB;
        bih->biSizeImage = pixelDataSize;
        bih->biXPelsPerMeter = 0;
        bih->biYPelsPerMeter = 0;
        bih->biClrUsed = 0;
        bih->biClrImportant = 0;

        // Write pixel data: convert top-down RGBA float to bottom-up BGRA bytes
        unsigned char* destPixels = (unsigned char*)(pMem + sizeof(BITMAPINFOHEADER));
        for (int y = 0; y < height; ++y) {
            int srcY = height - 1 - y; // Bottom-up conversion
            for (int x = 0; x < width; ++x) {
                int srcIdx = (srcY * width + x) * 4;
                int destIdx = (y * width + x) * 4;

                float r = rgbaPixels[srcIdx + 0];
                float g = rgbaPixels[srcIdx + 1];
                float b = rgbaPixels[srcIdx + 2];
                float a = rgbaPixels[srcIdx + 3];

                destPixels[destIdx + 0] = static_cast<unsigned char>(std::clamp(b * 255.0f, 0.0f, 255.0f));
                destPixels[destIdx + 1] = static_cast<unsigned char>(std::clamp(g * 255.0f, 0.0f, 255.0f));
                destPixels[destIdx + 2] = static_cast<unsigned char>(std::clamp(r * 255.0f, 0.0f, 255.0f));
                destPixels[destIdx + 3] = static_cast<unsigned char>(std::clamp(a * 255.0f, 0.0f, 255.0f));
            }
        }

        GlobalUnlock(hMem);

        // Open Clipboard
        if (!OpenClipboard(NULL)) {
            GlobalFree(hMem);
            Logger::Get().Error("Clipboard: OpenClipboard failed.");
            return false;
        }

        EmptyClipboard();

        if (!SetClipboardData(CF_DIB, hMem)) {
            CloseClipboard();
            GlobalFree(hMem);
            Logger::Get().Error("Clipboard: SetClipboardData failed.");
            return false;
        }

        CloseClipboard();
        Logger::Get().Info("Successfully copied canvas image to clipboard.");
        return true;
    }

    bool PasteImageFromClipboard(std::vector<float>& outRgbaPixels, int& outWidth, int& outHeight) {
        if (!OpenClipboard(NULL)) {
            Logger::Get().Error("Clipboard: OpenClipboard failed during paste.");
            return false;
        }

        // Check if CF_DIB is available
        if (!IsClipboardFormatAvailable(CF_DIB)) {
            CloseClipboard();
            Logger::Get().Warn("Clipboard: No DIB image format available in clipboard.");
            return false;
        }

        HGLOBAL hMem = GetClipboardData(CF_DIB);
        if (!hMem) {
            CloseClipboard();
            Logger::Get().Error("Clipboard: GetClipboardData failed.");
            return false;
        }

        // Lock memory block
        char* pMem = (char*)GlobalLock(hMem);
        if (!pMem) {
            CloseClipboard();
            Logger::Get().Error("Clipboard: GlobalLock failed during paste.");
            return false;
        }

        BITMAPINFOHEADER* bih = (BITMAPINFOHEADER*)pMem;
        if (bih->biSize < sizeof(BITMAPINFOHEADER)) {
            GlobalUnlock(hMem);
            CloseClipboard();
            Logger::Get().Error("Clipboard: Invalid BITMAPINFOHEADER size.");
            return false;
        }

        int width = bih->biWidth;
        int height = bih->biHeight;
        bool isBottomUp = true;
        if (height < 0) {
            isBottomUp = false;
            height = -height;
        }

        if (width <= 0 || height <= 0) {
            GlobalUnlock(hMem);
            CloseClipboard();
            Logger::Get().Error("Clipboard: Invalid dimensions in clipboard image.");
            return false;
        }

        int bitCount = bih->biBitCount;
        if (bitCount != 24 && bitCount != 32) {
            GlobalUnlock(hMem);
            CloseClipboard();
            Logger::Get().Error("Clipboard: Unsupported bit depth (" + std::to_string(bitCount) + "). Only 24-bit and 32-bit are supported.");
            return false;
        }

        // In DIB, each row is padded to a multiple of 4 bytes
        int bytesPerPixel = bitCount / 8;
        int rowStride = ((width * bytesPerPixel + 3) & ~3);

        unsigned char* srcPixels = (unsigned char*)(pMem + bih->biSize);
        
        outRgbaPixels.assign((size_t)width * height * 4, 0.0f);
        outWidth = width;
        outHeight = height;

        for (int y = 0; y < height; ++y) {
            // Convert to top-down coordinates
            int destY = isBottomUp ? (height - 1 - y) : y;
            unsigned char* srcRow = srcPixels + y * rowStride;

            for (int x = 0; x < width; ++x) {
                int destIdx = (destY * width + x) * 4;
                int srcIdx = x * bytesPerPixel;

                unsigned char b = srcRow[srcIdx + 0];
                unsigned char g = srcRow[srcIdx + 1];
                unsigned char r = srcRow[srcIdx + 2];
                unsigned char a = (bytesPerPixel == 4) ? srcRow[srcIdx + 3] : 255;

                outRgbaPixels[destIdx + 0] = static_cast<float>(r) / 255.0f;
                outRgbaPixels[destIdx + 1] = static_cast<float>(g) / 255.0f;
                outRgbaPixels[destIdx + 2] = static_cast<float>(b) / 255.0f;
                outRgbaPixels[destIdx + 3] = static_cast<float>(a) / 255.0f;
            }
        }

        GlobalUnlock(hMem);
        CloseClipboard();
        Logger::Get().Info("Successfully pasted image from clipboard (" + std::to_string(width) + "x" + std::to_string(height) + ", " + std::to_string(bitCount) + "-bit).");
        return true;
    }
}
