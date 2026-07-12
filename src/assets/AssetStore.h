#pragma once
#include "AssetTypes.h"
#include <mutex>
#include <string>
#include <unordered_map>

namespace assets {

// Global shared texture cache. Layers hold keys; CPU decode once per path.
class AssetStore {
public:
    static AssetStore& Get();

    // Library roots (created on demand).
    static std::string BuiltInRoot(); // {exe}/assets
    static std::string UserRoot();    // userdir/assets

    // Load (or ref) texture from absolute/relative filesystem path.
    // Returns store key (empty on failure). Increments refcount.
    std::string AcquireFile(const std::string& filepath);

    // Increment ref if key already resident; returns false if missing.
    bool AddRef(const std::string& key);

    // Decrement ref. When 0, CPU blob is freed (can re-acquire later).
    void Release(const std::string& key);

    // Non-owning peek. Valid until Release drops last ref / Clear.
    const TextureAsset* Get(const std::string& key) const;

    // Drop all zero-ref entries (and optionally force-clear).
    void CollectGarbage();
    void Clear();

    // Path helpers
    static std::string NormalizePath(const std::string& path);
    static AssetId MakeExternalId(const std::string& absolutePath);

private:
    AssetStore() = default;
    TextureAsset* FindMut(const std::string& key);

    mutable std::mutex m_Mutex;
    std::unordered_map<std::string, TextureAsset> m_Textures;
};

} // namespace assets
