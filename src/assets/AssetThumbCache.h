#pragma once
#include "AssetTypes.h"
#include <d3d11.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace assets {

// CPU + GPU thumbnail cache for Asset Browser.
// RULE: main thread never decodes images or reads packages for thumbs.
// Decode/IO runs on ThreadPool; Poll only applies CPU blobs + cheap GPU upload.
class AssetThumbCache {
public:
    static AssetThumbCache& Get();

    static constexpr int kThumbLo = 32;
    static constexpr int kThumbHi = 128;

    // Generate and optionally write sidecar PNGs (worker-safe).
    bool EnsureThumbsOnDisk(const std::string& assetFilePath, bool writeHi = true,
                            bool allowWrite = true);

    // Generate from in-memory RGBA (may be called from main for tiny payloads).
    bool EnsureThumbsFromRgba(const std::string& key, int w, int h,
                              const uint8_t* rgba, bool wantHi = true);

    // Non-blocking: returns SRV if ready; otherwise kicks async generate.
    ID3D11ShaderResourceView* GetSrv(ID3D11Device* device, const std::string& key,
                                     bool highQuality);

    // Main-thread poll for completed worker jobs (GPU upload budget).
    int Poll(ID3D11Device* device, double budgetMs = 2.0);

    bool IsBusy() const;
    int PendingCount() const;
    // UI: spinner vs failed placeholder (no infinite spin).
    bool IsPending(const std::string& key) const;
    bool IsFailed(const std::string& key) const;

    void Clear();
    void EvictGpu();

    bool GetCpuThumb(const std::string& key, bool highQuality,
                     std::vector<uint8_t>& outRgba, int& outW, int& outH) const;

    // Public so workers can downscale without friendship issues.
    static void Downscale(const uint8_t* src, int sw, int sh,
                          std::vector<uint8_t>& dst, int& dw, int& dh, int maxSide);

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
        // Worker output (preferred)
        std::vector<uint8_t> loRgba;
        int loW = 0, loH = 0;
        std::vector<uint8_t> hiRgba;
        int hiW = 0, hiH = 0;
        // Legacy / large project payload path
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
        std::string sourcePath;
        bool fromRgba = false;
        bool writeUserSidecar = false;
    };

    static bool CreateSrv(ID3D11Device* device, const uint8_t* rgba, int w, int h, GpuThumb& out);
    static void ReleaseGpu(GpuThumb& g);
    void EvictIfNeeded();
    void EnqueueDecode(const std::string& key, const std::string& path,
                       bool wantHi, bool writeUserSidecar);

    mutable std::mutex m_Mu;
    std::unordered_map<std::string, Entry> m_Entries;
    std::vector<std::shared_ptr<Pending>> m_Pending;
    std::atomic<int> m_InFlight{0};
    uint64_t m_Clock = 1;
    static constexpr size_t kMaxGpu = 256;
};

} // namespace assets
