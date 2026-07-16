#pragma once
#include "AssetTypes.h"
#include <d3d11.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace assets {

// Public façade for tools and UI. Prefer this over AssetStore directly.
class AssetManager {
public:
    static AssetManager& Get();

    void Startup();
    void Shutdown();
    // Main thread once per frame.
    void Poll(ID3D11Device* device, double budgetMs = 4.0);

    // Index
    void RefreshLibrary(AssetCategory cat);
    void RefreshAllLibrariesAsync();
    std::vector<AssetInfo> List(const AssetFilter& f) const;
    bool Find(const std::string& key, AssetInfo& out) const;

    // Import
    // Copy file into user library + thumbs → user: key (AddRef=1)
    std::string ImportFileToUser(const std::string& path);
    // Decode into project session blob → proj:uuid key (AddRef=1)
    std::string ImportFileToProject(const std::string& path);
    // Register RGBA as project texture (convert layer → asset)
    std::string RegisterProjectRgba(const std::string& name, int w, int h,
                                    std::vector<uint8_t> rgba);

    // Load / ref
    AssetLoadState RequestLoad(const std::string& key);
    bool AddRef(const std::string& key);
    void Release(const std::string& key);
    const TextureAsset* TryGet(const std::string& key) const;
    std::shared_ptr<const TexturePayload> GetPayload(const std::string& key) const;
    AssetLoadState GetLoadState(const std::string& key) const;
    bool GetDims(const std::string& key, int& w, int& h) const;

    // Kind gate
    bool IsKindAllowed(const std::string& key, AssetKind required) const;
    AssetKind GetKind(const std::string& key) const;

    // Project serialize helpers
    struct ProjectAssetBlob {
        std::string key;
        std::string name;
        std::string kind;
        std::string mime;
        int w = 0, h = 0;
        std::vector<uint8_t> bytes; // file bytes or raw RGBA
        bool isRgba = false;        // if true, bytes are raw RGBA w*h*4
    };
    std::vector<ProjectAssetBlob> CollectProjectBlobs(const std::vector<std::string>& keys) const;
    // Rehydrate after ClearProject; each blob becomes Ready + index entry.
    bool LoadProjectBlobs(const std::vector<ProjectAssetBlob>& blobs);
    void ClearProjectSession();

    std::string DisplayName(const std::string& key) const;

    ID3D11ShaderResourceView* GetThumbSrv(ID3D11Device* device, const std::string& key,
                                          bool highQuality = false);

    bool IsIndexReady() const { return m_IndexReady.load(); }
    bool IsIndexScanning() const { return m_IndexScanRunning.load(); }
    // True while library scan or thumb decode/upload is in flight (footer spinner).
    bool IsBusy() const;
    int ThumbPendingCount() const;
    bool IsThumbPending(const std::string& key) const;
    bool IsThumbFailed(const std::string& key) const;

    // Non-blocking import: decode/package on ThreadPool. Callback on main via Poll result queue.
    // Returns true if job started. UI stays free; Asset Browser refreshes when ready.
    bool ImportFileToUserAsync(const std::string& path);
    bool ImportFileToProjectAsync(const std::string& path);

private:
    AssetManager() = default;
    static std::string NewUuid();
    static std::string UniqueUserRelPath(const std::string& filename);

    std::atomic<bool> m_Started{false};
    std::atomic<bool> m_IndexReady{false};
    std::atomic<bool> m_IndexScanRunning{false};
};

} // namespace assets
