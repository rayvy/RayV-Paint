#include "AssetManager.h"
#include "AssetLibraryIndex.h"
#include "AssetStore.h"
#include "AssetThumbCache.h"
#include "../core/ImageManager.h"
#include "../core/Logger.h"
#include "../core/ThreadPool.h"
#include "../package/PackageIO.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <algorithm>

namespace fs = std::filesystem;

namespace assets {
namespace {

std::string UserTempDir() {
    try {
        fs::path p = fs::u8path(AssetStore::UserRoot()).parent_path() / "temp";
        std::error_code ec;
        fs::create_directories(p, ec);
        return p.string();
    } catch (...) {
        return "temp";
    }
}

void UpsertIndexEntry(const AssetInfo& info) {
    // LibraryIndex::UpsertProject only sets Project cat — for User we re-scan.
    if (info.category == AssetCategory::Project) {
        AssetLibraryIndex::Get().UpsertProject(info);
        return;
    }
    if (info.category == AssetCategory::User)
        AssetLibraryIndex::Get().ScanCategory(AssetCategory::User);
    else if (info.category == AssetCategory::BuiltIn)
        AssetLibraryIndex::Get().ScanCategory(AssetCategory::BuiltIn);
}

} // namespace

AssetManager& AssetManager::Get() {
    static AssetManager s;
    return s;
}

std::string AssetManager::NewUuid() {
    static std::mt19937_64 rng{
        (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count()
        ^ (uint64_t)std::random_device{}()
    };
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t a = dist(rng), b = dist(rng);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%012llx",
        (unsigned)(a >> 32),
        (unsigned)((a >> 16) & 0xFFFF),
        (unsigned)(a & 0xFFFF),
        (unsigned)(b >> 48),
        (unsigned long long)(b & 0xFFFFFFFFFFFFULL));
    return buf;
}

std::string AssetManager::UniqueUserRelPath(const std::string& filename) {
    fs::path root = fs::u8path(AssetStore::UserRoot()) / "textures";
    std::error_code ec;
    fs::create_directories(root, ec);
    fs::path name = fs::u8path(filename).filename();
    fs::path dest = root / name;
    if (!fs::exists(dest, ec))
        return (fs::path("textures") / name).generic_string();
    std::string stem = name.stem().string();
    std::string ext = name.extension().string();
    for (int i = 1; i < 1000; ++i) {
        fs::path cand = root / (stem + "_" + std::to_string(i) + ext);
        if (!fs::exists(cand, ec))
            return (fs::path("textures") / cand.filename()).generic_string();
    }
    return (fs::path("textures") / (stem + "_" + NewUuid() + ext)).generic_string();
}

void AssetManager::Startup() {
    if (m_Started.exchange(true)) return;
    std::error_code ec;
    fs::create_directories(fs::u8path(AssetStore::UserRoot()) / "textures", ec);
    fs::create_directories(fs::u8path(AssetStore::BuiltInRoot()) / "textures", ec);
    RefreshAllLibrariesAsync();
    Logger::Get().Info("AssetManager: started (Core=" + AssetStore::BuiltInRoot() +
                       ", User=" + AssetStore::UserRoot() + ")");
}

void AssetManager::Shutdown() {
    if (!m_Started.exchange(false)) return;
    AssetThumbCache::Get().Clear();
    AssetLibraryIndex::Get().ClearAll();
    m_IndexReady = false;
}

void AssetManager::Poll(ID3D11Device* device, double budgetMs) {
    AssetStore::Get().Poll(budgetMs * 0.6);
    AssetThumbCache::Get().Poll(device, budgetMs * 0.4);
}

void AssetManager::RefreshLibrary(AssetCategory cat) {
    AssetLibraryIndex::Get().ScanCategory(cat);
}

void AssetManager::RefreshAllLibrariesAsync() {
    if (m_IndexScanRunning.exchange(true)) return;
    m_IndexReady = false;
    ThreadPool::Get().Enqueue([]() {
        try {
            AssetLibraryIndex::Get().ScanCategory(AssetCategory::BuiltIn);
            AssetLibraryIndex::Get().ScanCategory(AssetCategory::User);
        } catch (...) {}
        AssetManager::Get().m_IndexReady = true;
        AssetManager::Get().m_IndexScanRunning = false;
        Logger::Get().Info("AssetManager: library index ready (" +
            std::to_string(AssetLibraryIndex::Get().Count()) + " entries)");
    });
}

std::vector<AssetInfo> AssetManager::List(const AssetFilter& f) const {
    auto list = AssetLibraryIndex::Get().List(f);
    for (auto& e : list) {
        e.loadState = AssetStore::Get().GetLoadState(e.key);
        int w = 0, h = 0;
        if (AssetStore::Get().GetDims(e.key, w, h)) {
            e.w = w; e.h = h;
        }
    }
    return list;
}

bool AssetManager::Find(const std::string& key, AssetInfo& out) const {
    if (!AssetLibraryIndex::Get().Find(key, out)) {
        if (const TextureAsset* a = AssetStore::Get().Get(key)) {
            out.key = key;
            out.category = a->id.cat;
            out.kind = a->kind;
            out.displayName = a->displayName.empty() ? key : a->displayName;
            out.sourcePath = a->sourcePath;
            out.w = a->w;
            out.h = a->h;
            out.loadState = a->state;
            return true;
        }
        return false;
    }
    out.loadState = AssetStore::Get().GetLoadState(key);
    int w = 0, h = 0;
    if (AssetStore::Get().GetDims(key, w, h)) {
        out.w = w; out.h = h;
    }
    return true;
}

std::string AssetManager::ImportFileToUser(const std::string& path) {
    if (path.empty()) return {};
    // Accept raw images or existing .rvpaf
    AssetKind gk = GuessKindFromPath(path);
    if (gk != AssetKind::Texture && gk != AssetKind::Unknown) {
        Logger::Get().Error("AssetManager: ImportFileToUser rejects non-texture: " + path);
        return {};
    }
    std::string rel;
    try {
        std::string fname = fs::path(path).filename().string();
        std::string stem = fs::path(path).stem().string();
        // Always store as .rvpaf in user library
        rel = UniqueUserRelPath(stem + ".rvpaf");
        // Fix UniqueUserRelPath if it kept wrong ext
        if (rel.size() < 6 || rel.substr(rel.size() - 6) != ".rvpaf") {
            auto dot = rel.find_last_of('.');
            if (dot != std::string::npos) rel = rel.substr(0, dot) + ".rvpaf";
            else rel += ".rvpaf";
        }
        fs::path dest = fs::u8path(AssetStore::UserRoot()) / fs::u8path(rel);
        fs::create_directories(dest.parent_path());

        std::string ext = fs::path(path).extension().string();
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext == ".rvpaf") {
            fs::copy_file(fs::u8path(path), dest, fs::copy_options::overwrite_existing);
        } else {
            // Build RVPAF from image
            std::vector<uint8_t> px;
            int w = 0, h = 0;
            if (!ImageManager::LoadImageFromFile(path, px, w, h) || w <= 0 || h <= 0)
                return {};
            // Re-read original file bytes for image.png resource
            std::vector<uint8_t> fileBytes;
            {
                std::ifstream in(fs::u8path(path), std::ios::binary);
                if (in)
                    fileBytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            // Thumbs
            std::vector<uint8_t> lo, hi;
            int lw = 0, lh = 0, hw = 0, hh = 0;
            // Use simple downscale via thumb cache helper: write temp thumbs then read
            AssetThumbCache::Get().EnsureThumbsFromRgba("tmp_import", w, h, px.data(), true);
            // Encode thumbs as PNG via ImageManager
            fs::path tmpDir = fs::u8path(AssetStore::UserRoot()).parent_path() / "temp";
            fs::create_directories(tmpDir);
            fs::path tLo = tmpDir / "import_t32.png";
            fs::path tHi = tmpDir / "import_t128.png";
            // Generate downscaled CPU and save
            auto down = [](const uint8_t* src, int sw, int sh, int maxSide,
                           std::vector<uint8_t>& dst, int& dw, int& dh) {
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
                        dst[di]=src[si]; dst[di+1]=src[si+1]; dst[di+2]=src[si+2]; dst[di+3]=src[si+3];
                    }
                }
            };
            down(px.data(), w, h, 32, lo, lw, lh);
            down(px.data(), w, h, 128, hi, hw, hh);
            ImageManager::SaveRGBA8ToFile(tLo.string(), lo.data(), lw, lh);
            ImageManager::SaveRGBA8ToFile(tHi.string(), hi.data(), hw, hh);
            std::vector<uint8_t> loPng, hiPng;
            {
                std::ifstream in(tLo, std::ios::binary);
                if (in) loPng.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            {
                std::ifstream in(tHi, std::ios::binary);
                if (in) hiPng.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            if (fileBytes.empty()) {
                // fallback: save rgba as png first
                fs::path tmpImg = tmpDir / "import_img.png";
                ImageManager::SaveRGBA8ToFile(tmpImg.string(), px.data(), w, h);
                std::ifstream in(tmpImg, std::ios::binary);
                if (in) fileBytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            rvp::Package pkg;
            rvp::BuildTexturePackage(pkg, stem, stem, fileBytes, loPng, hiPng, w, h);
            std::string err;
            if (!rvp::WritePackage(dest.string(), pkg, &err)) {
                Logger::Get().Error("ImportFileToUser package: " + err);
                return {};
            }
        }
    } catch (const std::exception& e) {
        Logger::Get().Error(std::string("AssetManager ImportFileToUser: ") + e.what());
        return {};
    }
    std::string key = MakeKey(AssetCategory::User, rel);
    std::string acq = AssetStore::Get().AcquireKey(key);
    if (acq.empty()) {
        fs::path dest = fs::u8path(AssetStore::UserRoot()) / fs::u8path(rel);
        acq = AssetStore::Get().AcquireFile(dest.string());
    }
    if (acq.empty()) return {};

    AssetInfo info;
    info.key = acq;
    info.category = AssetCategory::User;
    info.kind = AssetKind::Texture;
    info.displayName = fs::path(rel).stem().string();
    info.sourcePath = AssetStore::ResolvePath(acq);
    int w = 0, h = 0;
    AssetStore::Get().GetDims(acq, w, h);
    info.w = w; info.h = h;
    info.loadState = AssetLoadState::Ready;
    UpsertIndexEntry(info);
    return acq;
}

std::string AssetManager::ImportFileToProject(const std::string& path) {
    if (path.empty()) return {};
    if (GuessKindFromPath(path) != AssetKind::Texture) {
        Logger::Get().Error("AssetManager: ImportFileToProject rejects non-texture: " + path);
        return {};
    }
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    if (!ImageManager::LoadImageFromFile(path, px, w, h) || w <= 0 || h <= 0) {
        Logger::Get().Error("AssetManager: cannot decode for project: " + path);
        return {};
    }
    std::vector<uint8_t> fileBytes;
    try {
        std::ifstream in(fs::u8path(path), std::ios::binary);
        if (in)
            fileBytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    } catch (...) {}

    std::string name;
    try { name = fs::path(path).filename().string(); } catch (...) { name = "texture"; }
    std::string uuid = NewUuid();
    std::string mime = "image/png";
    try {
        std::string ext = fs::path(path).extension().string();
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext == ".jpg" || ext == ".jpeg") mime = "image/jpeg";
        else if (ext == ".dds") mime = "image/vnd-ms.dds";
        else if (ext == ".tga") mime = "image/tga";
        else if (ext == ".bmp") mime = "image/bmp";
    } catch (...) {}

    std::string key = AssetStore::Get().RegisterProjectTexture(
        uuid, name, w, h, std::move(px), std::move(fileBytes), mime);
    if (key.empty()) return {};

    if (auto pay = AssetStore::Get().GetPayload(key))
        AssetThumbCache::Get().EnsureThumbsFromRgba(key, w, h, pay->rgba.data(), true);

    AssetInfo info;
    info.key = key;
    info.category = AssetCategory::Project;
    info.kind = AssetKind::Texture;
    info.displayName = name;
    info.w = w; info.h = h;
    info.loadState = AssetLoadState::Ready;
    UpsertIndexEntry(info);
    return key;
}

std::string AssetManager::RegisterProjectRgba(const std::string& name, int w, int h,
                                              std::vector<uint8_t> rgba) {
    if (w <= 0 || h <= 0 || rgba.size() < (size_t)w * h * 4) return {};
    std::string uuid = NewUuid();
    std::string key = AssetStore::Get().RegisterProjectTexture(
        uuid, name.empty() ? "layer" : name, w, h, std::move(rgba), {}, "image/rgba8");
    if (key.empty()) return {};
    if (auto pay = AssetStore::Get().GetPayload(key))
        AssetThumbCache::Get().EnsureThumbsFromRgba(key, w, h, pay->rgba.data(), true);

    AssetInfo info;
    info.key = key;
    info.category = AssetCategory::Project;
    info.kind = AssetKind::Texture;
    info.displayName = name.empty() ? uuid : name;
    info.w = w; info.h = h;
    info.loadState = AssetLoadState::Ready;
    UpsertIndexEntry(info);
    return key;
}

AssetLoadState AssetManager::RequestLoad(const std::string& key) {
    return AssetStore::Get().RequestLoad(key);
}

bool AssetManager::AddRef(const std::string& key) {
    return AssetStore::Get().AddRef(key);
}

void AssetManager::Release(const std::string& key) {
    AssetStore::Get().Release(key);
}

const TextureAsset* AssetManager::TryGet(const std::string& key) const {
    const TextureAsset* a = AssetStore::Get().Get(key);
    if (!a || a->state != AssetLoadState::Ready || !a->payload) return nullptr;
    return a;
}

std::shared_ptr<const TexturePayload> AssetManager::GetPayload(const std::string& key) const {
    return AssetStore::Get().GetPayload(key);
}

AssetLoadState AssetManager::GetLoadState(const std::string& key) const {
    return AssetStore::Get().GetLoadState(key);
}

bool AssetManager::GetDims(const std::string& key, int& w, int& h) const {
    return AssetStore::Get().GetDims(key, w, h);
}

bool AssetManager::IsKindAllowed(const std::string& key, AssetKind required) const {
    AssetKind k = GetKind(key);
    if (k == AssetKind::Unknown) {
        AssetCategory cat;
        std::string rest;
        if (ParseKey(key, cat, rest))
            k = GuessKindFromPath(rest);
    }
    return k == required;
}

AssetKind AssetManager::GetKind(const std::string& key) const {
    AssetInfo info;
    if (Find(key, info)) return info.kind;
    if (const TextureAsset* a = AssetStore::Get().Get(key))
        return a->kind;
    return AssetKind::Unknown;
}

std::vector<AssetManager::ProjectAssetBlob> AssetManager::CollectProjectBlobs(
    const std::vector<std::string>& keys) const {
    std::vector<ProjectAssetBlob> out;
    for (const std::string& key : keys) {
        if (key.size() < 5 || key.compare(0, 5, "proj:") != 0)
            continue;
        TextureAsset snap;
        if (!AssetStore::Get().SnapshotForPack(key, snap))
            continue;
        ProjectAssetBlob b;
        b.key = key;
        b.name = snap.displayName;
        b.kind = KindName(snap.kind);
        b.mime = snap.mime.empty() ? "image/png" : snap.mime;
        b.w = snap.w;
        b.h = snap.h;
        if (!snap.fileBytes.empty()) {
            b.bytes = snap.fileBytes;
            b.isRgba = false;
        } else if (snap.payload && !snap.payload->rgba.empty()) {
            b.bytes = snap.payload->rgba;
            b.isRgba = true;
            b.mime = "image/rgba8";
        } else if (!snap.rgba.empty()) {
            b.bytes = snap.rgba;
            b.isRgba = true;
            b.mime = "image/rgba8";
        } else {
            continue;
        }
        out.push_back(std::move(b));
    }
    return out;
}

bool AssetManager::LoadProjectBlobs(const std::vector<ProjectAssetBlob>& blobs) {
    for (const auto& b : blobs) {
        if (b.key.size() < 5 || b.key.compare(0, 5, "proj:") != 0) continue;
        std::string uuid = b.key.substr(5);
        std::vector<uint8_t> rgba;
        int w = b.w, h = b.h;
        std::vector<uint8_t> fileBytes;

        if (b.isRgba || b.mime == "image/rgba8") {
            if (w <= 0 || h <= 0 || (int)b.bytes.size() < w * h * 4) continue;
            rgba = b.bytes;
        } else {
            fileBytes = b.bytes;
            try {
                fs::path tmp = fs::u8path(UserTempDir()) / ("rayv_asset_" + uuid + ".bin");
                {
                    std::ofstream out(tmp, std::ios::binary);
                    out.write(reinterpret_cast<const char*>(b.bytes.data()),
                              (std::streamsize)b.bytes.size());
                }
                if (!ImageManager::LoadImageFromFile(tmp.string(), rgba, w, h)) {
                    std::error_code ec;
                    fs::remove(tmp, ec);
                    if ((int)b.bytes.size() == b.w * b.h * 4) {
                        rgba = b.bytes;
                        w = b.w; h = b.h;
                    } else {
                        Logger::Get().Error("AssetManager: failed to decode project blob " + b.key);
                        continue;
                    }
                } else {
                    std::error_code ec;
                    fs::remove(tmp, ec);
                }
            } catch (...) {
                if ((int)b.bytes.size() == b.w * b.h * 4) {
                    rgba = b.bytes;
                    w = b.w; h = b.h;
                } else continue;
            }
        }
        if (rgba.empty() || w <= 0 || h <= 0) continue;

        std::string key = AssetStore::Get().RegisterProjectTexture(
            uuid, b.name, w, h, std::move(rgba), std::move(fileBytes), b.mime);
        // Drop the initial ref from Register — layers will AddRef/Acquire on bind.
        if (!key.empty())
            AssetStore::Get().Release(key);

        if (auto pay = AssetStore::Get().GetPayload(key))
            AssetThumbCache::Get().EnsureThumbsFromRgba(key, w, h, pay->rgba.data(), true);

        AssetInfo info;
        info.key = key.empty() ? b.key : key;
        info.category = AssetCategory::Project;
        info.kind = KindFromName(b.kind);
        if (info.kind == AssetKind::Unknown) info.kind = AssetKind::Texture;
        info.displayName = b.name;
        info.w = w; info.h = h;
        info.loadState = AssetLoadState::Ready;
        UpsertIndexEntry(info);
    }
    return true;
}

void AssetManager::ClearProjectSession() {
    AssetLibraryIndex::Get().ClearCategory(AssetCategory::Project);
    AssetStore::Get().ClearProject();
}

std::string AssetManager::DisplayName(const std::string& key) const {
    AssetInfo info;
    if (Find(key, info) && !info.displayName.empty())
        return info.displayName;
    if (const TextureAsset* a = AssetStore::Get().Get(key)) {
        if (!a->displayName.empty()) return a->displayName;
        if (!a->sourcePath.empty()) {
            try { return fs::path(a->sourcePath).filename().string(); }
            catch (...) {}
        }
    }
    AssetCategory cat;
    std::string rest;
    if (ParseKey(key, cat, rest)) {
        try { return fs::path(rest).filename().string(); }
        catch (...) { return rest; }
    }
    return key;
}

ID3D11ShaderResourceView* AssetManager::GetThumbSrv(ID3D11Device* device, const std::string& key,
                                                    bool highQuality) {
    return AssetThumbCache::Get().GetSrv(device, key, highQuality);
}

} // namespace assets
