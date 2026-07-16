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

// Parallel progressive thumbs: each worker owns its buffers.
// (stbi_load_from_memory / ReadFileBytes are fine concurrent with separate outputs.)

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
    // May be called from workers (import). Not from UI paint path.
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
    key = AssetStore::Get().AcquireFile(assetFilePath);
    if (!key.empty())
        AssetStore::Get().Release(key);
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

// Worker-side: load package / sidecar / full image. Never call from main UI path.
static bool LoadPackageThumb(const std::string& path, bool hi,
                             std::vector<uint8_t>& out, int& w, int& h) {
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

    int n = 0;
    stbi_uc* img = stbi_load_from_memory(blob->data(), (int)blob->size(), &w, &h, &n, 4);
    if (!img || w <= 0 || h <= 0) {
        if (img) stbi_image_free(img);
        return false;
    }
    out.assign(img, img + (size_t)w * h * 4);
    stbi_image_free(img);
    return true;
}

void AssetThumbCache::EnqueueDecode(const std::string& key, const std::string& path,
                                    bool wantHi, bool writeUserSidecar) {
    auto job = std::make_shared<Pending>();
    job->key = key;
    job->hi = wantHi;
    job->sourcePath = path;
    job->writeUserSidecar = writeUserSidecar;
    {
        std::lock_guard<std::mutex> lock(m_Mu);
        for (const auto& p : m_Pending)
            if (p && p->key == key) return; // already in flight
        m_Pending.push_back(job);
        m_InFlight.store((int)m_Pending.size());
    }

    try {
        // Capture cache pointer for mutex on complete (must not race Poll/GetSrv).
        AssetThumbCache* self = this;
        ThreadPool::Get().Enqueue([job, self]() {
            const auto t0 = std::chrono::steady_clock::now();
            std::vector<uint8_t> loPx, hiPx, full;
            int lw = 0, lh = 0, hw = 0, hh = 0, fw = 0, fh = 0;
            bool ok = false;

            try {
                const std::string& path = job->sourcePath;

                // 1) Package embedded thumbs (.rvpaf)
                if (!path.empty()) {
                    if (LoadPackageThumb(path, false, loPx, lw, lh)) {
                        ok = true;
                        LoadPackageThumb(path, true, hiPx, hw, hh);
                    }
                }

                // 2) Disk sidecars (*.thumbnail.png) — cheap, prefer these
                if (!ok && !path.empty()) {
                    if (ImageManager::LoadImageFromFile(
                            ThumbPathFor(path, false), loPx, lw, lh) && lw > 0) {
                        ok = true;
                        ImageManager::LoadImageFromFile(
                            ThumbPathFor(path, true), hiPx, hw, hh);
                    }
                }

                // 3) Full decode + downscale (parallel across workers)
                if (!ok && !path.empty()) {
                    if (ImageManager::LoadImageFromFile(path, full, fw, fh) &&
                        fw > 0 && fh > 0) {
                        if (fw > 8192 || fh > 8192) {
                            ok = false;
                            Logger::Get().InfoTag("perf",
                                "AssetThumb skip huge " + path + " " +
                                std::to_string(fw) + "x" + std::to_string(fh));
                        } else {
                            AssetThumbCache::Downscale(full.data(), fw, fh, loPx, lw, lh, kThumbLo);
                            AssetThumbCache::Downscale(full.data(), fw, fh, hiPx, hw, hh, kThumbHi);
                            ok = !loPx.empty();

                            if (ok && job->writeUserSidecar) {
                                try {
                                    ImageManager::SaveRGBA8ToFile(
                                        ThumbPathFor(path, false), loPx.data(), lw, lh);
                                    if (!hiPx.empty())
                                        ImageManager::SaveRGBA8ToFile(
                                            ThumbPathFor(path, true), hiPx.data(), hw, hh);
                                } catch (...) {}
                            }
                        }
                    }
                }
            } catch (...) {
                ok = false;
            }

            // Publish under cache mutex — Poll must not observe half-written job.
            {
                std::lock_guard<std::mutex> lock(self->m_Mu);
                job->ok = ok && !loPx.empty();
                if (job->ok) {
                    job->loRgba = std::move(loPx);
                    job->loW = lw; job->loH = lh;
                    job->hiRgba = std::move(hiPx);
                    job->hiW = hw; job->hiH = hh;
                }
                job->done = true;
            }
            const double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
            if (ms > 80.0)
                Logger::Get().InfoTag("perf",
                    "AssetThumb.async " + job->key + " took " + std::to_string(ms) + " ms ok=" +
                    (ok ? "1" : "0"));
        });
    } catch (...) {
        std::lock_guard<std::mutex> lock(m_Mu);
        job->ok = false;
        job->done = true;
    }
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
            if (highQuality && cpu.rgba.empty())
                cpu = it->second.lo; // fall back to lo CPU for upload
            if (!cpu.rgba.empty()) {
                // GPU upload only on main — cheap.
                if (!gpu.srv)
                    CreateSrv(device, cpu.rgba.data(), cpu.w, cpu.h, gpu);
                if (gpu.srv) {
                    gpu.lastUse = ++m_Clock;
                    EvictIfNeeded();
                    return gpu.srv;
                }
            }
            // CPU not ready yet — may already be in flight
            for (const auto& p : m_Pending)
                if (p && p->key == key) return nullptr;
        } else {
            for (const auto& p : m_Pending)
                if (p && p->key == key) return nullptr;
        }
    }

    // Resolve path / payload without disk decode on main.
    std::string path = AssetStore::ResolvePath(key);
    if (path.empty()) {
        if (const TextureAsset* a = AssetStore::Get().Get(key))
            path = a->sourcePath;
    }

    // In-memory project payload: downscale can still be heavy — do on worker if large.
    auto payload = AssetStore::Get().GetPayload(key);
    if (payload && !payload->rgba.empty()) {
        const int pxCount = payload->w * payload->h;
        if (pxCount <= 512 * 512) {
            // Tiny: sync downscale OK
            EnsureThumbsFromRgba(key, payload->w, payload->h, payload->rgba.data(), true);
            return GetSrv(device, key, highQuality);
        }
        // Large project texture: copy for worker
        auto job = std::make_shared<Pending>();
        job->key = key;
        job->hi = true;
        job->fromRgba = true;
        job->rgba = payload->rgba; // copy for worker
        job->w = payload->w;
        job->h = payload->h;
        {
            std::lock_guard<std::mutex> lock(m_Mu);
            for (const auto& p : m_Pending)
                if (p && p->key == key) return nullptr;
            m_Pending.push_back(job);
            m_InFlight.store((int)m_Pending.size());
        }
        try {
            AssetThumbCache* self = this;
            ThreadPool::Get().Enqueue([job, self]() {
                std::vector<uint8_t> lo, hi;
                int lw = 0, lh = 0, hw = 0, hh = 0;
                AssetThumbCache::Downscale(job->rgba.data(), job->w, job->h, lo, lw, lh, kThumbLo);
                AssetThumbCache::Downscale(job->rgba.data(), job->w, job->h, hi, hw, hh, kThumbHi);
                std::lock_guard<std::mutex> lock(self->m_Mu);
                job->loRgba = std::move(lo); job->loW = lw; job->loH = lh;
                job->hiRgba = std::move(hi); job->hiW = hw; job->hiH = hh;
                job->rgba.clear(); job->rgba.shrink_to_fit();
                job->ok = !job->loRgba.empty();
                job->done = true;
            });
        } catch (...) {
            std::lock_guard<std::mutex> lock(m_Mu);
            job->ok = false;
            job->done = true;
        }
        return nullptr;
    }

    if (path.empty()) {
        // No path and no payload — mark failed so UI stops spinning.
        std::lock_guard<std::mutex> lock(m_Mu);
        m_Entries[key].failed = true;
        return nullptr;
    }

    bool writeSidecar = false;
    AssetCategory cat;
    std::string rest;
    if (ParseKey(key, cat, rest) && cat == AssetCategory::User)
        writeSidecar = true;

    EnqueueDecode(key, path, highQuality, writeSidecar);
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
        m_InFlight.store((int)m_Pending.size());
    }

    std::vector<std::shared_ptr<Pending>> defer;
    for (size_t i = 0; i < done.size(); ++i) {
        auto& job = done[i];
        if (budgetMs > 0 && n > 0) {
            double elapsed = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
            if (elapsed > budgetMs) {
                // Leave remaining completed jobs for next frame (still done=true).
                for (size_t j = i; j < done.size(); ++j)
                    defer.push_back(done[j]);
                break;
            }
        }

        if (!job->ok || (job->loRgba.empty() && job->rgba.empty())) {
            std::lock_guard<std::mutex> lock(m_Mu);
            m_Entries[job->key].failed = true;
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(m_Mu);
            auto& e = m_Entries[job->key];
            if (!job->loRgba.empty()) {
                e.lo.w = job->loW; e.lo.h = job->loH;
                e.lo.rgba = std::move(job->loRgba);
            }
            if (!job->hiRgba.empty()) {
                e.hi.w = job->hiW; e.hi.h = job->hiH;
                e.hi.rgba = std::move(job->hiRgba);
            }
            if (e.lo.rgba.empty() && !job->rgba.empty()) {
                std::vector<uint8_t> lo, hi;
                int lw = 0, lh = 0, hw = 0, hh = 0;
                Downscale(job->rgba.data(), job->w, job->h, lo, lw, lh, kThumbLo);
                Downscale(job->rgba.data(), job->w, job->h, hi, hw, hh, kThumbHi);
                e.lo.w = lw; e.lo.h = lh; e.lo.rgba = std::move(lo);
                e.hi.w = hw; e.hi.h = hh; e.hi.rgba = std::move(hi);
            }
            e.failed = e.lo.rgba.empty();
        }

        if (device) {
            std::lock_guard<std::mutex> lock(m_Mu);
            auto it = m_Entries.find(job->key);
            if (it != m_Entries.end() && !it->second.failed && !it->second.lo.rgba.empty()) {
                if (!it->second.gpuLo.srv)
                    CreateSrv(device, it->second.lo.rgba.data(), it->second.lo.w, it->second.lo.h,
                              it->second.gpuLo);
                if (it->second.gpuLo.srv)
                    it->second.gpuLo.lastUse = ++m_Clock;
                EvictIfNeeded();
            }
        }
        n++;
    }

    if (!defer.empty()) {
        std::lock_guard<std::mutex> lock(m_Mu);
        m_Pending.insert(m_Pending.end(), defer.begin(), defer.end());
        m_InFlight.store((int)m_Pending.size());
    }
    return n;
}

bool AssetThumbCache::IsBusy() const {
    return m_InFlight.load() > 0;
}

int AssetThumbCache::PendingCount() const {
    return m_InFlight.load();
}

bool AssetThumbCache::IsPending(const std::string& key) const {
    if (key.empty()) return false;
    std::lock_guard<std::mutex> lock(m_Mu);
    for (const auto& p : m_Pending)
        if (p && p->key == key) return true;
    return false;
}

bool AssetThumbCache::IsFailed(const std::string& key) const {
    if (key.empty()) return false;
    std::lock_guard<std::mutex> lock(m_Mu);
    auto it = m_Entries.find(key);
    return it != m_Entries.end() && it->second.failed;
}

void AssetThumbCache::Clear() {
    std::lock_guard<std::mutex> lock(m_Mu);
    for (auto& kv : m_Entries) {
        ReleaseGpu(kv.second.gpuLo);
        ReleaseGpu(kv.second.gpuHi);
    }
    m_Entries.clear();
    m_Pending.clear();
    m_InFlight.store(0);
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
