#pragma once
#include <vector>

namespace ClipboardHelper {
    // Copies 32-bit RGBA float pixels to the Windows Clipboard as CF_DIB (converts to bottom-up 32-bit BGRA bytes)
    bool CopyImageToClipboard(const std::vector<float>& rgbaPixels, int width, int height);

    // Pastes 32-bit DIB from the Windows Clipboard and converts to top-down 32-bit RGBA float pixels
    bool PasteImageFromClipboard(std::vector<float>& outRgbaPixels, int& outWidth, int& outHeight);
}
