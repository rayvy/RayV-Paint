#include "AssetStore.h"
#include "../core/ConfigManager.h"
#include "../core/ImageManager.h"
#include "../core/Logger.h"
#include <algorithm>
#include <cctype>
#include <filesystem>

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
        // Lowercase drive letter on Windows for stable keys
        if (s.size() >= 2 && s[1] == ':')
            s[0] = (char)std::tolower((unsigned char)s[0]);
        // Unify separators in key space
        std::replace(s.begin(), s.end(), '\\', '/');
        return s;
    } catch (...) {
        return path;
    }
}

AssetId AssetStore::MakeExternalId(const std::string& absolutePath) {
    AssetId id;
    id.cat = AssetCategory::External;
    id.key = "ext:" + NormalizePath(absolutePath);
    return id;
}

TextureAsset* AssetStore::FindMut(const std::string& key) {
    auto it = m_Textures.find(key);
    if (it == m_Textures.end()) return nullptr;
    return &it->second;
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
        // Prefer existing path if norm doesn't exist (unicode edge cases)
        if (!fs::exists(fs::u8path(norm)))
            norm = filepath;
        else
            norm = NormalizePath(norm);
    } catch (...) {
        Logger::Get().Error("AssetStore: bad path: " + filepath);
        return {};
    }

    AssetId id = MakeExternalId(norm);
    const std::string& key = id.key;

    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Textures.find(key);
    if (it != m_Textures.end()) {
        it->second.refCount++;
        return key;
    }

    std::vector<uint8_t> px;
    int tw = 0, th = 0;
    // Load from original filepath first (best for non-normalized open)
    std::string loadPath = filepath;
    if (!ImageManager::LoadImageFromFile(loadPath, px, tw, th)) {
        if (loadPath != norm && !ImageManager::LoadImageFromFile(norm, px, tw, th)) {
            Logger::Get().Error("AssetStore: decode failed: " + filepath);
            return {};
        }
    }
    if (tw <= 0 || th <= 0 || px.empty()) {
        Logger::Get().Error("AssetStore: empty image: " + filepath);
        return {};
    }

    TextureAsset asset;
    asset.id = id;
    asset.sourcePath = norm;
    asset.w = tw;
    asset.h = th;
    asset.rgba = std::move(px);
    asset.refCount = 1;
    m_Textures.emplace(key, std::move(asset));

    Logger::Get().Info("AssetStore: acquired " + std::to_string(tw) + "x" + std::to_string(th) +
                       " key=" + key + " (cache size " + std::to_string(m_Textures.size()) + ")");
    return key;
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
    if (it->second.refCount == 0) {
        // Free CPU blob immediately — Fill lag path; re-decode on next Acquire.
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

void AssetStore::CollectGarbage() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    for (auto it = m_Textures.begin(); it != m_Textures.end();) {
        if (it->second.refCount <= 0)
            it = m_Textures.erase(it);
        else
            ++it;
    }
}

void AssetStore::Clear() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Textures.clear();
}

} // namespace assets
