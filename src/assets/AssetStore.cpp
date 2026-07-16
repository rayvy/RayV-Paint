#include "AssetStore.h"
#include "../core/ConfigManager.h"
#include "../core/ImageManager.h"
#include "../core/Logger.h"
#include "../core/ThreadPool.h"
#include "../package/PackageIO.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stb_image.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace fs = std::filesystem;

namespace assets {

AssetStore& AssetStore::Get() {
    static AssetStore s;
    return s;
}

std::string AssetStore::BuiltInRoot() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return "assets";
    fs::path p(buf);
    return (p.parent_path() / "assets").string();
#else
    return "assets";
#endif
}

std::string AssetStore::UserRoot() {
    return (fs::path(ConfigManager::GetUserDirectory()) / "assets").string();
}

std::string AssetStore::NormalizePath(const std::string& path) {
    if (path.empty()) return {};
    try {
        fs::path p = fs::u8path(path);
        if (p.is_relative())
            p = fs::absolute(p);
        p = p.lexically_normal();
        std::string s = p.string();
        if (s.size() >= 2 && s[1] == ':')
            s[0] = (char)std::tolower((unsigned char)s[0]);
        std::replace(s.begin(), s.end(), '\\', '/');
        return s;
    } catch (...) {
        return path;
    }
}

AssetId AssetStore::MakeExternalId(const std::string& absolutePath) {
    AssetId id;
    id.cat = AssetCategory::External;
    id.key = MakeKey(AssetCategory::External, NormalizePath(absolutePath));
    return id;
}

AssetId AssetStore::MakeIdFromKey(const std::string& key) {
    AssetId id;
    std::string rest;
    if (!ParseKey(key, id.cat, rest)) {
        id.cat = AssetCategory::External;
        id.key = key;
        return id;
    }
    id.key = key;
    // Normalize legacy builtin: → core:
    if (key.size() >= 8 && key.compare(0, 8, "builtin:") == 0)
        id.key = MakeKey(AssetCategory::BuiltIn, rest);
    return id;
}

std::string AssetStore::ResolvePath(const std::string& key) {
    AssetCategory cat;
    std::string rest;
    if (!ParseKey(key, cat, rest) || rest.empty())
        return {};
    try {
        switch (cat) {
        case AssetCategory::BuiltIn:
            return (fs::path(BuiltInRoot()) / fs::u8path(rest)).lexically_normal().string();
        case AssetCategory::User:
            return (fs::path(UserRoot()) / fs::u8path(rest)).lexically_normal().string();
        case AssetCategory::External:
            return rest;
        case AssetCategory::Project:
            return {}; // memory / packed only
        }
    } catch (...) {}
    return {};
}

TextureAsset* AssetStore::FindMut(const std::string& key) {
    auto it = m_Textures.find(key);
    if (it == m_Textures.end()) return nullptr;
    return &it->second;
}

void AssetStore::CommitPayload(TextureAsset& asset, int w, int h, std::vector<uint8_t> rgba) {
    asset.w = w;
    asset.h = h;
    asset.rgba = rgba; // keep legacy field in sync for old callers
    auto pay = std::make_shared<TexturePayload>();
    pay->w = w;
    pay->h = h;
    pay->rgba = std::move(rgba);
    asset.payload = std::move(pay);
    asset.state = AssetLoadState::Ready;
}

static bool DecodeImageMemory(const uint8_t* data, size_t size, std::vector<uint8_t>& px, int& tw, int& th) {
    tw = th = 0;
    px.clear();
    if (!data || size == 0) return false;
    int w = 0, h = 0, n = 0;
    stbi_uc* img = stbi_load_from_memory(data, (int)size, &w, &h, &n, 4);
    if (!img || w <= 0 || h <= 0) {
        if (img) stbi_image_free(img);
        return false;
    }
    px.assign(img, img + (size_t)w * h * 4);
    stbi_image_free(img);
    tw = w;
    th = h;
    return true;
}

// Decode texture from disk path: raw image OR .rvpaf package (thread-safe, no store).
static bool DecodeTextureFromPath(const std::string& filepath, std::vector<uint8_t>& px, int& tw, int& th) {
    tw = th = 0;
    px.clear();
    std::string ext;
    try {
        ext = fs::path(filepath).extension().string();
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    } catch (...) {}
    if (ext == ".rvpaf") {
        rvp::Package pkg;
        if (!rvp::ReadPackage(filepath, pkg, nullptr) || pkg.format != rvp::PackageFormat::RVPAF)
            return false;
        const std::vector<uint8_t>* img = pkg.Get(rvp::paths::kImagePng);
        if (!img) img = pkg.Get(rvp::paths::kImageDds);
        if (!img || img->empty()) return false;
        if (DecodeImageMemory(img->data(), img->size(), px, tw, th))
            return true;
        // Fallback: ImageManager via temp (DDS)
        try {
            fs::path tmp = fs::temp_directory_path() / "rayv_rvpaf_decode.bin";
            {
                std::ofstream o(tmp, std::ios::binary);
                o.write(reinterpret_cast<const char*>(img->data()), (std::streamsize)img->size());
            }
            bool ok = ImageManager::LoadImageFromFile(tmp.string(), px, tw, th);
            std::error_code ec; fs::remove(tmp, ec);
            return ok && tw > 0 && th > 0 && !px.empty();
        } catch (...) {
            return false;
        }
    }
    return ImageManager::LoadImageFromFile(filepath, px, tw, th) && tw > 0 && th > 0 && !px.empty();
}

bool AssetStore::LoadFileInto(TextureAsset& asset, const std::string& filepath) {
    std::vector<uint8_t> px;
    int tw = 0, th = 0;
    std::string ext;
    try {
        ext = fs::path(filepath).extension().string();
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    } catch (...) {}

    if (!DecodeTextureFromPath(filepath, px, tw, th)) {
        asset.state = AssetLoadState::Failed;
        return false;
    }
    try {
        std::ifstream in(fs::u8path(filepath), std::ios::binary);
        if (in) {
            asset.fileBytes.assign(std::istreambuf_iterator<char>(in),
                                   std::istreambuf_iterator<char>());
        }
    } catch (...) {}
    if (ext == ".rvpaf") {
        asset.mime = "application/rvpaf";
        rvp::Package pkg;
        if (rvp::ReadPackage(filepath, pkg, nullptr)) {
            try {
                auto j = nlohmann::json::parse(pkg.manifestJson);
                if (j.contains("displayName"))
                    asset.displayName = j["displayName"].get<std::string>();
            } catch (...) {}
        }
    } else {
        asset.mime = "image/png";
    }
    asset.kind = AssetKind::Texture;

    asset.sourcePath = NormalizePath(filepath);
    if (asset.displayName.empty()) {
        try { asset.displayName = fs::path(filepath).stem().string(); }
        catch (...) { asset.displayName = filepath; }
    }
    CommitPayload(asset, tw, th, std::move(px));
    return true;
}

std::string AssetStore::AcquireFile(const std::string& filepath) {
    if (filepath.empty()) return {};
    std::string norm;
    try {
        norm = NormalizePath(filepath);
        if (!fs::exists(fs::u8path(norm)) && !fs::exists(fs::u8path(filepath))) {
            Logger::Get().Error("AssetStore: file not found: " + filepath);
            return {};
        }
        if (!fs::exists(fs::u8path(norm)))
            norm = filepath;
        else
            norm = NormalizePath(norm);
    } catch (...) {
        Logger::Get().Error("AssetStore: bad path: " + filepath);
        return {};
    }

    // Prefer library-relative key if under Core/User
    std::string key;
    try {
        fs::path fileP = fs::u8path(norm);
        fs::path core = fs::u8path(BuiltInRoot());
        fs::path user = fs::u8path(UserRoot());
        auto under = [&](const fs::path& root) -> std::string {
            std::error_code ec;
            fs::path rel = fs::relative(fileP, root, ec);
            if (ec || rel.empty() || rel.string().find("..") != std::string::npos)
                return {};
            std::string r = rel.generic_string();
            return r;
        };
        std::string relCore = under(core);
        if (!relCore.empty())
            key = MakeKey(AssetCategory::BuiltIn, relCore);
        else {
            std::string relUser = under(user);
            if (!relUser.empty())
                key = MakeKey(AssetCategory::User, relUser);
        }
    } catch (...) {}
    if (key.empty())
        key = MakeExternalId(norm).key;

    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Textures.find(key);
    if (it != m_Textures.end()) {
        if (it->second.state == AssetLoadState::Ready && it->second.payload) {
            it->second.refCount++;
            return key;
        }
        // Reload if failed/missing payload
        if (LoadFileInto(it->second, norm)) {
            it->second.refCount = std::max(1, it->second.refCount + 1);
            return key;
        }
        return {};
    }

    TextureAsset asset;
    asset.id = MakeIdFromKey(key);
    asset.kind = AssetKind::Texture;
    if (!LoadFileInto(asset, norm)) {
        Logger::Get().Error("AssetStore: decode failed: " + filepath);
        return {};
    }
    asset.refCount = 1;
    m_Textures.emplace(key, std::move(asset));
    Logger::Get().Info("AssetStore: acquired " + std::to_string(
        m_Textures[key].w) + "x" + std::to_string(m_Textures[key].h) +
        " key=" + key);
    return key;
}

std::string AssetStore::AcquireKey(const std::string& key) {
    if (key.empty()) return {};
    AssetId id = MakeIdFromKey(key);
    const std::string& k = id.key;

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        auto it = m_Textures.find(k);
        if (it != m_Textures.end()) {
            if (it->second.state == AssetLoadState::Ready && it->second.payload) {
                it->second.refCount++;
                return k;
            }
            if (it->second.id.cat == AssetCategory::Project) {
                // Project blob without payload cannot re-load from disk
                if (it->second.state == AssetLoadState::Failed) return {};
                it->second.refCount++;
                // try RequestLoad path below without double-count
                it->second.refCount--;
            }
        }
    }

    std::string path = ResolvePath(k);
    if (!path.empty())
        return AcquireFile(path);

    // Project: must already be registered
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Textures.find(k);
    if (it == m_Textures.end()) return {};
    if (it->second.state != AssetLoadState::Ready || !it->second.payload)
        return {};
    it->second.refCount++;
    return k;
}

std::string AssetStore::RegisterProjectTexture(const std::string& uuid,
                                               const std::string& displayName,
                                               int w, int h,
                                               std::vector<uint8_t> rgba,
                                               std::vector<uint8_t> fileBytes,
                                               const std::string& mime) {
    if (uuid.empty() || w <= 0 || h <= 0 || rgba.size() < (size_t)w * h * 4)
        return {};
    std::string key = MakeKey(AssetCategory::Project, uuid);
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Textures.find(key);
    if (it != m_Textures.end()) {
        CommitPayload(it->second, w, h, std::move(rgba));
        if (!fileBytes.empty()) it->second.fileBytes = std::move(fileBytes);
        if (!mime.empty()) it->second.mime = mime;
        if (!displayName.empty()) it->second.displayName = displayName;
        it->second.refCount = std::max(1, it->second.refCount);
        return key;
    }
    TextureAsset asset;
    asset.id = MakeIdFromKey(key);
    asset.kind = AssetKind::Texture;
    asset.displayName = displayName.empty() ? uuid : displayName;
    asset.mime = mime.empty() ? "image/png" : mime;
    asset.fileBytes = std::move(fileBytes);
    CommitPayload(asset, w, h, std::move(rgba));
    asset.refCount = 1;
    m_Textures.emplace(key, std::move(asset));
    return key;
}

void AssetStore::EnsureMeta(const std::string& key, AssetKind kind,
                            const std::string& displayName, const std::string& sourcePath,
                            int w, int h) {
    if (key.empty()) return;
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Textures.find(key);
    if (it != m_Textures.end()) {
        if (!displayName.empty()) it->second.displayName = displayName;
        if (!sourcePath.empty()) it->second.sourcePath = sourcePath;
        if (w > 0) it->second.w = w;
        if (h > 0) it->second.h = h;
        it->second.kind = kind;
        return;
    }
    TextureAsset asset;
    asset.id = MakeIdFromKey(key);
    asset.kind = kind;
    asset.displayName = displayName;
    asset.sourcePath = sourcePath;
    asset.w = w;
    asset.h = h;
    asset.state = AssetLoadState::Missing;
    asset.refCount = 0;
    m_Textures.emplace(key, std::move(asset));
}

bool AssetStore::AddRef(const std::string& key) {
    if (key.empty()) return false;
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto* a = FindMut(key);
    if (!a) return false;
    a->refCount++;
    return true;
}

void AssetStore::Release(const std::string& key) {
    if (key.empty()) return;
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Textures.find(key);
    if (it == m_Textures.end()) return;
    it->second.refCount = std::max(0, it->second.refCount - 1);
    if (it->second.refCount == 0 && it->second.id.cat != AssetCategory::Project) {
        // Keep project assets for browser until ClearProject; free heavy payload for others.
        it->second.payload.reset();
        it->second.rgba.clear();
        if (it->second.state == AssetLoadState::Ready)
            it->second.state = AssetLoadState::Missing;
        // External with no meta purpose: erase
        if (it->second.id.cat == AssetCategory::External)
            m_Textures.erase(it);
    }
}

const TextureAsset* AssetStore::Get(const std::string& key) const {
    if (key.empty()) return nullptr;
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Textures.find(key);
    if (it == m_Textures.end()) return nullptr;
    return &it->second;
}

std::shared_ptr<const TexturePayload> AssetStore::GetPayload(const std::string& key) const {
    if (key.empty()) return nullptr;
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Textures.find(key);
    if (it == m_Textures.end()) return nullptr;
    return it->second.payload;
}

AssetLoadState AssetStore::GetLoadState(const std::string& key) const {
    if (key.empty()) return AssetLoadState::Missing;
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Textures.find(key);
    if (it == m_Textures.end()) return AssetLoadState::Missing;
    return it->second.state;
}

bool AssetStore::GetDims(const std::string& key, int& outW, int& outH) const {
    outW = outH = 0;
    if (key.empty()) return false;
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Textures.find(key);
    if (it == m_Textures.end()) return false;
    outW = it->second.w;
    outH = it->second.h;
    return outW > 0 && outH > 0;
}

AssetLoadState AssetStore::RequestLoad(const std::string& key) {
    if (key.empty()) return AssetLoadState::Missing;
    AssetId id = MakeIdFromKey(key);
    const std::string& k = id.key;

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        auto it = m_Textures.find(k);
        if (it != m_Textures.end()) {
            if (it->second.state == AssetLoadState::Ready && it->second.payload)
                return AssetLoadState::Ready;
            if (it->second.state == AssetLoadState::Failed)
                return AssetLoadState::Failed;
            if (it->second.state == AssetLoadState::Pending)
                return AssetLoadState::Pending;
        } else {
            // Create meta shell
            TextureAsset asset;
            asset.id = id;
            asset.kind = AssetKind::Texture;
            asset.state = AssetLoadState::Missing;
            m_Textures.emplace(k, std::move(asset));
        }

        // Project without payload cannot load from path
        if (id.cat == AssetCategory::Project) {
            auto* a = FindMut(k);
            if (a && a->payload) {
                a->state = AssetLoadState::Ready;
                return AssetLoadState::Ready;
            }
            if (a) a->state = AssetLoadState::Failed;
            return AssetLoadState::Failed;
        }

        std::string path = ResolvePath(k);
        if (path.empty()) {
            // try sourcePath on entry
            auto* a = FindMut(k);
            if (a && !a->sourcePath.empty()) path = a->sourcePath;
        }
        if (path.empty() || !fs::exists(fs::u8path(path))) {
            auto* a = FindMut(k);
            if (a) a->state = AssetLoadState::Missing;
            return AssetLoadState::Missing;
        }

        // Already queued?
        for (const auto& p : m_Pending) {
            if (p && p->key == k)
                return AssetLoadState::Pending;
        }
        if (m_InFlightLoads >= kMaxInFlight) {
            auto* a = FindMut(k);
            if (a && a->state != AssetLoadState::Ready)
                a->state = AssetLoadState::Pending;
            return AssetLoadState::Pending;
        }

        auto job = std::make_shared<PendingLoad>();
        job->key = k;
        job->path = path;
        m_Pending.push_back(job);
        auto* a = FindMut(k);
        if (a) {
            a->state = AssetLoadState::Pending;
            if (a->sourcePath.empty()) a->sourcePath = path;
        }
        m_InFlightLoads++;

        ThreadPool::Get().Enqueue([job]() {
            std::vector<uint8_t> px;
            int tw = 0, th = 0;
            bool ok = DecodeTextureFromPath(job->path, px, tw, th);
            job->ok = ok;
            if (ok) {
                job->w = tw;
                job->h = th;
                job->rgba = std::move(px);
            } else {
                job->error = "decode failed";
            }
            job->done = true;
        });
        return AssetLoadState::Pending;
    }
}

int AssetStore::Poll(double budgetMs) {
    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();
    int committed = 0;

    std::vector<std::shared_ptr<PendingLoad>> done;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        for (auto it = m_Pending.begin(); it != m_Pending.end();) {
            if (*it && (*it)->done) {
                done.push_back(*it);
                it = m_Pending.erase(it);
                m_InFlightLoads = std::max(0, m_InFlightLoads - 1);
            } else {
                ++it;
            }
        }
    }

    for (auto& job : done) {
        if (budgetMs > 0) {
            double elapsed = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
            if (elapsed > budgetMs && committed > 0) {
                // re-queue remaining for next frame
                std::lock_guard<std::mutex> lock(m_Mutex);
                m_Pending.push_back(job);
                // don't count as in-flight again
                break;
            }
        }
        std::lock_guard<std::mutex> lock(m_Mutex);
        auto* a = FindMut(job->key);
        if (!a) continue;
        if (job->ok) {
            CommitPayload(*a, job->w, job->h, std::move(job->rgba));
            if (a->sourcePath.empty()) a->sourcePath = job->path;
            if (a->displayName.empty()) {
                try { a->displayName = fs::path(job->path).filename().string(); }
                catch (...) {}
            }
            committed++;
        } else {
            a->state = AssetLoadState::Failed;
            Logger::Get().Error("AssetStore async load failed: " + job->key +
                                " (" + job->error + ")");
        }
    }

    // Kick more loads if capacity free (meta entries still Missing with path)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_InFlightLoads < kMaxInFlight) {
            for (auto& kv : m_Textures) {
                if (m_InFlightLoads >= kMaxInFlight) break;
                auto& a = kv.second;
                if (a.state != AssetLoadState::Missing && a.state != AssetLoadState::Pending)
                    continue;
                if (a.payload) continue;
                if (a.id.cat == AssetCategory::Project) continue;
                // Only auto-start if already marked Pending by a prior RequestLoad
                if (a.state != AssetLoadState::Pending) continue;
                bool queued = false;
                for (const auto& p : m_Pending)
                    if (p && p->key == kv.first) { queued = true; break; }
                if (queued) continue;
                std::string path = a.sourcePath.empty() ? ResolvePath(kv.first) : a.sourcePath;
                if (path.empty()) continue;
                auto job = std::make_shared<PendingLoad>();
                job->key = kv.first;
                job->path = path;
                m_Pending.push_back(job);
                m_InFlightLoads++;
                ThreadPool::Get().Enqueue([job]() {
                    std::vector<uint8_t> px;
                    int tw = 0, th = 0;
                    bool ok = DecodeTextureFromPath(job->path, px, tw, th);
                    job->ok = ok;
                    if (ok) {
                        job->w = tw; job->h = th;
                        job->rgba = std::move(px);
                    }
                    job->done = true;
                });
            }
        }
    }
    return committed;
}

void AssetStore::ListProjectKeys(std::vector<std::string>& out) const {
    out.clear();
    std::lock_guard<std::mutex> lock(m_Mutex);
    for (const auto& kv : m_Textures) {
        if (kv.second.id.cat == AssetCategory::Project)
            out.push_back(kv.first);
    }
}

bool AssetStore::SnapshotForPack(const std::string& key, TextureAsset& outCopy) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Textures.find(key);
    if (it == m_Textures.end()) return false;
    outCopy = it->second;
    // Deep-copy payload data
    if (it->second.payload) {
        auto p = std::make_shared<TexturePayload>(*it->second.payload);
        outCopy.payload = p;
        outCopy.rgba = p->rgba;
        outCopy.w = p->w;
        outCopy.h = p->h;
    }
    return true;
}

void AssetStore::CollectGarbage() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    for (auto it = m_Textures.begin(); it != m_Textures.end();) {
        if (it->second.refCount <= 0 && it->second.id.cat == AssetCategory::External)
            it = m_Textures.erase(it);
        else
            ++it;
    }
}

void AssetStore::Clear() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Textures.clear();
    m_Pending.clear();
    m_InFlightLoads = 0;
}

void AssetStore::ClearProject() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    for (auto it = m_Textures.begin(); it != m_Textures.end();) {
        if (it->second.id.cat == AssetCategory::Project)
            it = m_Textures.erase(it);
        else
            ++it;
    }
}

} // namespace assets
