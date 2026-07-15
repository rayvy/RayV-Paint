#pragma once
#include "AssetTypes.h"
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace assets {

// Shared texture payload cache. Layers hold keys; decode once per key.
// Prefer AssetManager for tools; AssetStore remains the low-level cache.
class AssetStore {
public:
    static AssetStore& Get();

    // Library roots (created on demand for user).
    static std::string BuiltInRoot(); // {exe}/assets  (Core)
    static std::string UserRoot();    // userdir/assets
    static std::string CoreRoot() { return BuiltInRoot(); }

    // Resolve key → absolute filesystem path (empty for pure project blobs).
    static std::string ResolvePath(const std::string& key);

    // --- Sync acquire (still used by migration / import paths) ---
    // Load (or ref) texture from absolute/relative filesystem path → ext: key.
    std::string AcquireFile(const std::string& filepath);

    // Acquire by library/project key (loads from disk or uses existing project entry).
    // Increments refcount. Returns empty on hard failure.
    std::string AcquireKey(const std::string& key);

    // Register already-decoded project/session texture under proj:uuid.
    // Takes ownership of rgba; increments ref to 1.
    std::string RegisterProjectTexture(const std::string& uuid,
                                       const std::string& displayName,
                                       int w, int h,
                                       std::vector<uint8_t> rgba,
                                       std::vector<uint8_t> fileBytes = {},
                                       const std::string& mime = "image/png");

    // Ensure entry exists for key with metadata only (index scan / pending).
    void EnsureMeta(const std::string& key, AssetKind kind,
                    const std::string& displayName, const std::string& sourcePath,
                    int w = 0, int h = 0);

    bool AddRef(const std::string& key);
    void Release(const std::string& key);

    // Non-owning peek (legacy). Prefer GetPayload for sample safety.
    const TextureAsset* Get(const std::string& key) const;

    // Shared payload pin — safe across concurrent Release of other assets.
    std::shared_ptr<const TexturePayload> GetPayload(const std::string& key) const;

    AssetLoadState GetLoadState(const std::string& key) const;
    bool GetDims(const std::string& key, int& outW, int& outH) const;

    // Async: request full decode if not Ready. Returns current state.
    AssetLoadState RequestLoad(const std::string& key);
    // Main thread: commit finished async loads (budget soft).
    int Poll(double budgetMs = 4.0);

    // Enumerate project-category assets currently in store (for browser + save).
    void ListProjectKeys(std::vector<std::string>& out) const;
    // Snapshot of project asset for .rayp packing (fileBytes or rgba).
    bool SnapshotForPack(const std::string& key, TextureAsset& outCopy) const;

    void CollectGarbage();
    void Clear();
    // Drop only project-category entries (on new document / after load rehydrate).
    void ClearProject();

    static std::string NormalizePath(const std::string& path);
    static AssetId MakeExternalId(const std::string& absolutePath);
    static AssetId MakeIdFromKey(const std::string& key);

private:
    AssetStore() = default;
    TextureAsset* FindMut(const std::string& key);
    bool LoadFileInto(TextureAsset& asset, const std::string& filepath);
    void CommitPayload(TextureAsset& asset, int w, int h, std::vector<uint8_t> rgba);

    mutable std::mutex m_Mutex;
    std::unordered_map<std::string, TextureAsset> m_Textures;

    // Async decode pipeline
    struct PendingLoad {
        std::string key;
        std::string path;
        bool done = false;
        bool ok = false;
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
        std::string error;
    };
    std::vector<std::shared_ptr<PendingLoad>> m_Pending;
    int m_InFlightLoads = 0;
    static constexpr int kMaxInFlight = 2;
};

} // namespace assets
