#pragma once
#include <vector>
#include <cstdint>

namespace ClipboardHelper {
    // True if Windows clipboard holds a pasteable image (PNG / DIBV5 / DIB / BITMAP).
    bool HasClipboardImage();

    // True when clipboard sequence changed since our last successful CopyImageToClipboard
    // (i.e. another app put something on the clipboard — prefer system paste).
    bool IsSystemClipboardNewerThanLastCopy();

    // Copies RGBA float pixels to the Windows clipboard as:
    //   1) "PNG"  — preserves alpha (Chrome, Blender, Photoshop, Paint.NET, …)
    //   2) CF_DIBV5 — 32-bit BGRA with alpha mask
    //   3) CF_DIB  — 32-bit BGRA fallback for older apps
    bool CopyImageToClipboard(const std::vector<float>& rgbaPixels, int width, int height);

    // Pastes image from Windows clipboard → top-down RGBA float [0..1].
    // Priority: PNG → CF_DIBV5 → CF_DIB → CF_BITMAP.
    bool PasteImageFromClipboard(std::vector<float>& outRgbaPixels, int& outWidth, int& outHeight);
}
