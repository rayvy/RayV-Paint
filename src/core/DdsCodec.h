#pragma once
// DdsCodec — in-process DirectXTex facade (replaces texconv.exe).
// Shell thumbs stay on bcdec (light shell).

#include "TileCache.h"

#include <cstdint>
#include <string>
#include <vector>

#include <dxgiformat.h>

namespace DdsCodec {

struct SourceInfo {
    DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
    int width = 0;
    int height = 0;
    int depth = 1;
    int mipCount = 1;
    int arraySize = 1;
    bool isCube = false;
    bool isVolume = false;
    bool isDx10 = false;
    bool srgb = false;
    bool singleChannel = false; // R8 / R16F / R32F …
    bool dualChannel = false;   // R8G8 / BC5 …
    std::string fourCC;
    std::string formatLabel;  // "BC7_UNORM_SRGB"
    std::string uiLabel;      // Paint.NET-style for export combo
    int suggestedDepth = 0;   // 0=U8 1=F16 2=F32
};

// --- Analyze (metadata only) ---
bool AnalyzeFile(const std::string& utf8Path, SourceInfo& out);
bool AnalyzeMemory(const void* data, size_t size, SourceInfo& out);

// --- Load mip0 into TileCache (decompressed) ---
// Storage: U8 / F16 / F32 from suggestedDepth.
bool LoadToTileCache(const std::string& utf8Path, TileCache& outCache,
                     int& outWidth, int& outHeight, SourceInfo& outInfo);

// --- Save ---
struct SaveOptions {
    DXGI_FORMAT format = DXGI_FORMAT_BC7_UNORM_SRGB;
    bool generateMips = false; // full mip+BC7 chain is expensive; off by default
    std::string mipFilter = "Cubic";      // Point/Box/Linear/Cubic/Fant/Lanczos
    std::string compressionSpeed = "Fast"; // Fast for interactive export
};

// RGBA8 source (w*h*4). Used for U8 composites / batch.
bool SaveRgba8(const std::string& utf8Path, const uint8_t* rgba, int w, int h,
               const SaveOptions& opt);

// Float RGBA (w*h*4 floats). Used for F16/F32 projects.
bool SaveRgba32F(const std::string& utf8Path, const float* rgba, int w, int h,
                 const SaveOptions& opt);

// --- Format catalog helpers ---
const char* FormatLabel(DXGI_FORMAT fmt);
std::string UiLabelFromDxgi(DXGI_FORMAT fmt);
bool UiLabelToDxgi(const std::string& uiOrDxgi, DXGI_FORMAT& out);
bool IsBlockCompressed(DXGI_FORMAT fmt);
bool IsFloatFormat(DXGI_FORMAT fmt);
int SuggestDocumentDepth(DXGI_FORMAT fmt);

// Full list for export UI (null-terminated C strings owned statically).
int CatalogCount();
const char* const* CatalogUiLabels();

} // namespace DdsCodec
