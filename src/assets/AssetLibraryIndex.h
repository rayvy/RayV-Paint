#pragma once
#include "AssetTypes.h"
#include <mutex>
#include <string>
#include <vector>

namespace assets {

// Lightweight catalog of library files (Core / User / Project session).
// Scan does not full-decode textures — only names, paths, kinds, optional dims from thumbs.
class AssetLibraryIndex {
public:
    static AssetLibraryIndex& Get();

    void ClearCategory(AssetCategory cat);
    void ClearAll();

    // Synchronous scan of one category (call from worker or after Startup).
    void ScanCategory(AssetCategory cat);

    // Project session entries (in-memory)
    void UpsertProject(const AssetInfo& info);
    void RemoveKey(const std::string& key);

    std::vector<AssetInfo> List(const AssetFilter& filter) const;
    bool Find(const std::string& key, AssetInfo& out) const;
    size_t Count() const;

private:
    AssetLibraryIndex() = default;
    void ScanDirectory(AssetCategory cat, const std::string& root);

    mutable std::mutex m_Mu;
    std::vector<AssetInfo> m_Entries;
};

} // namespace assets
