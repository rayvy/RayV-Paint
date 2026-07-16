#include "AssetThumbCache.h"
#include "AssetStore.h"
#include "../core/ImageManager.h"
#include "../core/Logger.h"
#include "../core/ThreadPool.h"
#include "../package/PackageIO.h"
#include <stb_image.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace assets {

AssetThumbCache& AssetThumbCache::Get() {
    static AssetThumbCache s;
    return s;
}

void AssetThumbCache::Downscale(const uint8_t* src, int sw, int sh,
                                std::vector<uint8_t>& dst, int& dw, int& dh, int maxSide) {
    float scale = 1.f;
    if (sw > maxSide || sh > maxSide)
        scale = (float)maxSide / (float)std::max(sw, sh);
    dw = std::max(1, (int)(sw * scale + 0.5f));
    dh = std::max(1, (int)(sh * scale + 0.5f));
    dst.assign((size_t)dw * dh * 4, 0);
    for (int y = 0; y < dh; ++y) {
        int sy = std::min(sh - 1, y * sh / dh);
        for (int x = 0; x < dw; ++x) {
            int sx = std::min(sw - 1, x * sw / dw);
            size_t di = ((size_t)y * dw + x) * 4;
            size_t si = ((size_t)sy * sw + sx) * 4;
            dst[di+0]=src[si+0]; dst[di+1]=src[si+1];
            dst[di+2]=src[si+2]; dst[di+3]=src[si+3];
        }
    }
}

bool AssetThumbCache::CreateSrv(ID3D11Device* device, const uint8_t* rgba, int w, int h, GpuThumb& out) {
    if (!device || !rgba || w <= 0 || h <= 0) return false;
    ReleaseGpu(out);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = rgba;
    init.SysMemPitch = (UINT)w * 4;
    if (FAILED(device->CreateTexture2D(&td, &init, &out.tex)) || !out.tex) return false;
    if (FAILED(device->CreateShaderResourceView(out.tex, nullptr, &out.srv)) || !out.srv) {
        out.tex->Release(); out.tex = nullptr;
        return false;
    }
    return true;
}

void AssetThumbCache::ReleaseGpu(GpuThumb& g) {
    if (g.srv) { g.srv->Release(); g.srv = nullptr; }
    if (g.tex) { g.tex->Release(); g.tex = nullptr; }
}

void AssetThumbCache::EvictIfNeeded() {
    size_t n = 0;
    for (auto& kv : m_Entries) {
        if (kv.second.gpuLo.srv) n++;
        if (kv.second.gpuHi.srv) n++;
    }
    while (n > kMaxGpu) {
        std::string victim;
        uint64_t best = UINT64_MAX;
        bool hi = false;
        for (auto& kv : m_Entries) {
            if (kv.second.gpuLo.srv && kv.second.gpuLo.lastUse < best) {
                best = kv.second.gpuLo.lastUse; victim = kv.first; hi = false;
            }
            if (kv.second.gpuHi.srv && kv.second.gpuHi.lastUse < best) {
                best = kv.second.gpuHi.lastUse; victim = kv.first; hi = true;
            }
        }
        if (victim.empty()) break;
        auto& e = m_Entries[victim];
        if (hi) { ReleaseGpu(e.gpuHi); }
        else { ReleaseGpu(e.gpuLo); }
        n--;
    }
}

bool AssetThumbCache::EnsureThumbsOnDisk(const std::string& assetFilePath, bool writeHi, bool allowWrite) {
    if (assetFilePath.empty()) return false;
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    if (!ImageManager::LoadImageFromFile(assetFilePath, px, w, h) || w <= 0 || h <= 0)
        return false;

    std::vector<uint8_t> lo, hi;
    int lw = 0, lh = 0, hw = 0, hh = 0;
    Downscale(px.data(), w, h, lo, lw, lh, kThumbLo);
    if (writeHi)
        Downscale(px.data(), w, h, hi, hw, hh, kThumbHi);

    if (allowWrite) {
        std::string loPath = ThumbPathFor(assetFilePath, false);
        std::string hiPath = ThumbPathFor(assetFilePath, true);
        ImageManager::SaveRGBA8ToFile(loPath, lo.data(), lw, lh);
        if (writeHi && !hi.empty())
            ImageManager::SaveRGBA8ToFile(hiPath, hi.data(), hw, hh);
    }

    std::string key;
    // Best-effort key from path via store helpers
    key = AssetStore::Get().AcquireFile(assetFilePath);
    if (!key.empty())
        AssetStore::Get().Release(key); // just for key resolution; don't hold ref
    if (key.empty())
        key = AssetStore::MakeExternalId(AssetStore::NormalizePath(assetFilePath)).key;

    std::lock_guard<std::mutex> lock(m_Mu);
    auto& e = m_Entries[key];
    e.lo.w = lw; e.lo.h = lh; e.lo.rgba = std::move(lo);
    if (writeHi) {
        e.hi.w = hw; e.hi.h = hh; e.hi.rgba = std::move(hi);
    }
    e.failed = false;
    return true;
}

bool AssetThumbCache::EnsureThumbsFromRgba(const std::string& key, int w, int h,
                                           const uint8_t* rgba, bool wantHi) {
    if (key.empty() || !rgba || w <= 0 || h <= 0) return false;
    std::vector<uint8_t> lo, hi;
    int lw = 0, lh = 0, hw = 0, hh = 0;
    Downscale(rgba, w, h, lo, lw, lh, kThumbLo);
    if (wantHi)
        Downscale(rgba, w, h, hi, hw, hh, kThumbHi);
    std::lock_guard<std::mutex> lock(m_Mu);
    auto& e = m_Entries[key];
    e.lo.w = lw; e.lo.h = lh; e.lo.rgba = std::move(lo);
    if (wantHi) {
        e.hi.w = hw; e.hi.h = hh; e.hi.rgba = std::move(hi);
    }
    e.failed = false;
    return true;
}

ID3D11ShaderResourceView* AssetThumbCache::GetSrv(ID3D11Device* device, const std::string& key,
                                                   bool highQuality) {
    if (!device || key.empty()) return nullptr;

    {
        std::lock_guard<std::mutex> lock(m_Mu);
        auto it = m_Entries.find(key);
        if (it != m_Entries.end()) {
            if (it->second.failed) return nullptr;
            CpuThumb& cpu = highQuality ? it->second.hi : it->second.lo;
            GpuThumb& gpu = highQuality ? it->second.gpuHi : it->second.gpuLo;
            // Fall back to lo if hi missing
            if (highQuality && cpu.rgba.empty()) {
                cpu = it->second.lo;
            }
            if (!cpu.rgba.empty()) {
                if (!gpu.srv)
                    CreateSrv(device, cpu.rgba.data(), cpu.w, cpu.h, gpu);
                if (gpu.srv) {
                    gpu.lastUse = ++m_Clock;
                    EvictIfNeeded();
                    return gpu.srv;
                }
            }
        }
    }

    // Try load sidecar from disk path
    std::string path = AssetStore::ResolvePath(key);
    if (path.empty()) {
        if (const TextureAsset* a = AssetStore::Get().Get(key))
            path = a->sourcePath;
    }

    auto tryPackageThumb = [&](bool hi) -> bool {
        if (path.empty()) return false;
        std::string ext;
        try {
            ext = fs::path(path).extension().string();
            for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        } catch (...) {}
        if (ext != ".rvpaf") return false;
        rvp::Package pkg;
        if (!rvp::ReadPackage(path, pkg, nullptr)) return false;
        const char* resName = hi ? rvp::paths::kThumbHPng : rvp::paths::kThumbPng;
        const std::vector<uint8_t>* blob = pkg.Get(resName);
        if (!blob && hi) blob = pkg.Get(rvp::paths::kThumbPng);
        if (!blob || blob->empty()) return false;
        int w = 0, h = 0, n = 0;
        stbi_uc* img = stbi_load_from_memory(blob->data(), (int)blob->size(), &w, &h, &n, 4);
        if (!img || w <= 0 || h <= 0) {
            if (img) stbi_image_free(img);
            return false;
        }
        std::vector<uint8_t> px(img, img + (size_t)w * h * 4);
        stbi_image_free(img);
        std::lock_guard<std::mutex> lock(m_Mu);
        auto& e = m_Entries[key];
        CpuThumb& cpu = hi ? e.hi : e.lo;
        cpu.w = w; cpu.h = h; cpu.rgba = std::move(px);
        return true;
    };

    auto trySidecar = [&](bool hi) -> bool {
        if (path.empty()) return false;
        std::string tp = ThumbPathFor(path, hi);
        std::vector<uint8_t> px;
        int w = 0, h = 0;
        if (!ImageManager::LoadImageFromFile(tp, px, w, h) || w <= 0 || h <= 0)
            return false;
        std::lock_guard<std::mutex> lock(m_Mu);
        auto& e = m_Entries[key];
        CpuThumb& cpu = hi ? e.hi : e.lo;
        cpu.w = w; cpu.h = h; cpu.rgba = std::move(px);
        return true;
    };

    if (tryPackageThumb(highQuality) || tryPackageThumb(false) ||
        trySidecar(highQuality) || trySidecar(false)) {
        return GetSrv(device, key, highQuality);
    }

    // Kick async full-image downscale (once)
    {
        std::lock_guard<std::mutex> lock(m_Mu);
        for (const auto& p : m_Pending)
            if (p && p->key == key) return nullptr;

        // Project: use payload if ready
        auto payload = AssetStore::Get().GetPayload(key);
        if (payload && !payload->rgba.empty()) {
            EnsureThumbsFromRgba(key, payload->w, payload->h, payload->rgba.data(), true);
            return GetSrv(device, key, highQuality);
        }

        if (path.empty()) return nullptr;

        auto job = std::make_shared<Pending>();
        job->key = key;
        job->hi = highQuality;
        job->sourcePath = path;
        m_Pending.push_back(job);
        ThreadPool::Get().Enqueue([job]() {
            std::vector<uint8_t> px;
            int w = 0, h = 0;
            if (!ImageManager::LoadImageFromFile(job->sourcePath, px, w, h) || w <= 0 || h <= 0) {
                job->ok = false;
                job->done = true;
                return;
            }
            // Produce both sizes
            job->ok = true;
            // Store full in rgba temporarily; Poll will downscale both
            job->w = w; job->h = h;
            job->rgba = std::move(px);
            job->done = true;
        });
    }
    return nullptr;
}

int AssetThumbCache::Poll(ID3D11Device* device, double budgetMs) {
    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();
    int n = 0;
    std::vector<std::shared_ptr<Pending>> done;
    {
        std::lock_guard<std::mutex> lock(m_Mu);
        for (auto it = m_Pending.begin(); it != m_Pending.end();) {
            if (*it && (*it)->done) {
                done.push_back(*it);
                it = m_Pending.erase(it);
            } else ++it;
        }
    }
    for (auto& job : done) {
        if (budgetMs > 0) {
            double elapsed = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
            if (elapsed > budgetMs && n > 0) break;
        }
        if (!job->ok || job->rgba.empty()) {
            std::lock_guard<std::mutex> lock(m_Mu);
            m_Entries[job->key].failed = true;
            continue;
        }
        EnsureThumbsFromRgba(job->key, job->w, job->h, job->rgba.data(), true);
        // Write sidecars for User library only
        AssetCategory cat;
        std::string rest;
        if (ParseKey(job->key, cat, rest) && cat == AssetCategory::User && !job->sourcePath.empty()) {
            try {
                std::vector<uint8_t> lo, hi;
                int lw, lh, hw, hh;
                Downscale(job->rgba.data(), job->w, job->h, lo, lw, lh, kThumbLo);
                Downscale(job->rgba.data(), job->w, job->h, hi, hw, hh, kThumbHi);
                ImageManager::SaveRGBA8ToFile(ThumbPathFor(job->sourcePath, false), lo.data(), lw, lh);
                ImageManager::SaveRGBA8ToFile(ThumbPathFor(job->sourcePath, true), hi.data(), hw, hh);
            } catch (...) {}
        }
        if (device)
            GetSrv(device, job->key, false);
        n++;
    }
    return n;
}

void AssetThumbCache::Clear() {
    std::lock_guard<std::mutex> lock(m_Mu);
    for (auto& kv : m_Entries) {
        ReleaseGpu(kv.second.gpuLo);
        ReleaseGpu(kv.second.gpuHi);
    }
    m_Entries.clear();
    m_Pending.clear();
}

void AssetThumbCache::EvictGpu() {
    std::lock_guard<std::mutex> lock(m_Mu);
    for (auto& kv : m_Entries) {
        ReleaseGpu(kv.second.gpuLo);
        ReleaseGpu(kv.second.gpuHi);
    }
}

bool AssetThumbCache::GetCpuThumb(const std::string& key, bool highQuality,
                                  std::vector<uint8_t>& outRgba, int& outW, int& outH) const {
    std::lock_guard<std::mutex> lock(m_Mu);
    auto it = m_Entries.find(key);
    if (it == m_Entries.end()) return false;
    const CpuThumb& c = highQuality && !it->second.hi.rgba.empty() ? it->second.hi : it->second.lo;
    if (c.rgba.empty()) return false;
    outRgba = c.rgba;
    outW = c.w;
    outH = c.h;
    return true;
}

} // namespace assets
