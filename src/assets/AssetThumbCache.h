#pragma once
#include "AssetTypes.h"
#include <d3d11.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace assets {

// CPU + GPU thumbnail cache for Asset Browser.
// Prefers disk sidecars (*.thumbnail.png / *.thumbnail_h.png); generates once on demand.
class AssetThumbCache {
public:
    static AssetThumbCache& Get();

    static constexpr int kThumbLo = 32;
    static constexpr int kThumbHi = 128;

    // Generate and optionally write sidecar PNGs next to file (User only writes).
    // Returns false if source cannot be decoded.
    bool EnsureThumbsOnDisk(const std::string& assetFilePath, bool writeHi = true,
                            bool allowWrite = true);

    // Generate from in-memory RGBA (project assets).
    bool EnsureThumbsFromRgba(const std::string& key, int w, int h,
                              const uint8_t* rgba, bool wantHi = true);

    // Non-blocking-ish: returns SRV if ready; may kick async generate.
    ID3D11ShaderResourceView* GetSrv(ID3D11Device* device, const std::string& key,
                                     bool highQuality);

    // Main-thread poll for async thumb jobs.
    int Poll(ID3D11Device* device, double budgetMs = 2.0);

    void Clear();
    void EvictGpu();

    // CPU peek (for packing tests / export)
    bool GetCpuThumb(const std::string& key, bool highQuality,
                     std::vector<uint8_t>& outRgba, int& outW, int& outH) const;

private:
    AssetThumbCache() = default;

    struct CpuThumb {
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
    };
    struct GpuThumb {
        ID3D11ShaderResourceView* srv = nullptr;
        ID3D11Texture2D* tex = nullptr;
        uint64_t lastUse = 0;
    };
    struct Entry {
        CpuThumb lo;
        CpuThumb hi;
        GpuThumb gpuLo;
        GpuThumb gpuHi;
        bool failed = false;
    };

    struct Pending {
        std::string key;
        bool hi = false;
        bool done = false;
        bool ok = false;
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
        std::string sourcePath;
        bool fromRgba = false;
    };

    static void Downscale(const uint8_t* src, int sw, int sh,
                          std::vector<uint8_t>& dst, int& dw, int& dh, int maxSide);
    static bool CreateSrv(ID3D11Device* device, const uint8_t* rgba, int w, int h, GpuThumb& out);
    static void ReleaseGpu(GpuThumb& g);
    void EvictIfNeeded();

    mutable std::mutex m_Mu;
    std::unordered_map<std::string, Entry> m_Entries;
    std::vector<std::shared_ptr<Pending>> m_Pending;
    uint64_t m_Clock = 1;
    static constexpr size_t kMaxGpu = 96;
};

} // namespace assets
