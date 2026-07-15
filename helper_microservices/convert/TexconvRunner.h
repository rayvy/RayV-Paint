#pragma once
#include <string>
#include <vector>
#include <functional>

namespace helpers {

struct ConvertOptions {
    std::string format = "BC7_UNORM_SRGB"; // DXGI / texconv -f
    bool generateMips = true;
    std::string compressionSpeed = "Medium"; // Fast / Medium / Slow
    std::string mipFilter = "CUBIC";
};

struct ConvertResult {
    std::string srcPath;
    std::string dstPath;
    bool ok = false;
    std::string message;
};

// Locate texconv.exe next to helpers / parent dirs (same layout as Core).
std::wstring FindTexconvPath();

// Convert one PNG (or any image texconv accepts) → DDS next to source (same stem).
// outDdsPath empty → same directory as src, .dds extension.
ConvertResult ConvertOne(const std::string& srcUtf8, const ConvertOptions& opt,
                         const std::string& outDdsUtf8 = {});

// Batch; optional progress callback (index, total, current path).
std::vector<ConvertResult> ConvertMany(
    const std::vector<std::string>& sources,
    const ConvertOptions& opt,
    const std::function<void(int, int, const std::string&)>& onProgress = {});

} // namespace helpers
