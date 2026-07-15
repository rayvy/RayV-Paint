#include "AssetLibraryIndex.h"
#include "AssetStore.h"
#include "../core/Logger.h"
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace fs = std::filesystem;

namespace assets {

AssetLibraryIndex& AssetLibraryIndex::Get() {
    static AssetLibraryIndex s;
    return s;
}

void AssetLibraryIndex::ClearCategory(AssetCategory cat) {
    std::lock_guard<std::mutex> lock(m_Mu);
    m_Entries.erase(
        std::remove_if(m_Entries.begin(), m_Entries.end(),
                       [cat](const AssetInfo& e) { return e.category == cat; }),
        m_Entries.end());
}

void AssetLibraryIndex::ClearAll() {
    std::lock_guard<std::mutex> lock(m_Mu);
    m_Entries.clear();
}

void AssetLibraryIndex::ScanDirectory(AssetCategory cat, const std::string& root) {
    std::error_code ec;
    if (!fs::exists(fs::u8path(root), ec)) {
        if (cat == AssetCategory::User) {
            fs::create_directories(fs::u8path(root) / "textures", ec);
        }
        return;
    }

    std::vector<AssetInfo> found;
    try {
        for (auto it = fs::recursive_directory_iterator(fs::u8path(root),
                 fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); ++it) {
            if (ec) { ec.clear(); continue; }
            if (!it->is_regular_file(ec)) continue;
            fs::path p = it->path();
            std::string name = p.filename().string();
            // Skip thumb sidecars
            if (name.find(".thumbnail") != std::string::npos) continue;

            std::string ext = p.extension().string();
            for (char& c : ext) c = (char)std::tolower((unsigned char)c);
            AssetKind kind = GuessKindFromPath(p.string());
            // Index textures (and template hooks if present)
            if (kind != AssetKind::Texture && kind != AssetKind::ExportTemplate &&
                kind != AssetKind::Preview3dTemplate && kind != AssetKind::BrushTip &&
                kind != AssetKind::SmartSource)
                continue;

            std::error_code rec;
            fs::path rel = fs::relative(p, fs::u8path(root), rec);
            if (rec) continue;
            std::string relStr = rel.generic_string();
            AssetInfo info;
            info.category = cat;
            info.kind = kind;
            info.key = MakeKey(cat, relStr);
            info.displayName = name;
            info.sourcePath = p.string();
            info.loadState = AssetLoadState::Missing;
            // Dims left 0 until load / thumb read
            found.push_back(std::move(info));
        }
    } catch (const std::exception& e) {
        Logger::Get().Warn(std::string("AssetLibraryIndex scan: ") + e.what());
    }

    {
        std::lock_guard<std::mutex> lock(m_Mu);
        m_Entries.erase(
            std::remove_if(m_Entries.begin(), m_Entries.end(),
                           [cat](const AssetInfo& e) { return e.category == cat; }),
            m_Entries.end());
        for (auto& e : found)
            m_Entries.push_back(std::move(e));
    }

    // Seed store meta for texture keys (no decode)
    for (const auto& e : found) {
        if (e.kind == AssetKind::Texture)
            AssetStore::Get().EnsureMeta(e.key, e.kind, e.displayName, e.sourcePath);
    }

    Logger::Get().Info("AssetLibraryIndex: " + std::string(CategoryDisplayName(cat)) +
                       " → " + std::to_string(found.size()) + " entries under " + root);
}

void AssetLibraryIndex::ScanCategory(AssetCategory cat) {
    switch (cat) {
    case AssetCategory::BuiltIn:
        ScanDirectory(cat, AssetStore::BuiltInRoot());
        break;
    case AssetCategory::User:
        ScanDirectory(cat, AssetStore::UserRoot());
        break;
    case AssetCategory::Project:
        // Project entries maintained via UpsertProject
        break;
    default:
        break;
    }
}

void AssetLibraryIndex::UpsertProject(const AssetInfo& info) {
    std::lock_guard<std::mutex> lock(m_Mu);
    for (auto& e : m_Entries) {
        if (e.key == info.key) {
            e = info;
            e.category = AssetCategory::Project;
            return;
        }
    }
    AssetInfo copy = info;
    copy.category = AssetCategory::Project;
    m_Entries.push_back(std::move(copy));
}

void AssetLibraryIndex::RemoveKey(const std::string& key) {
    std::lock_guard<std::mutex> lock(m_Mu);
    m_Entries.erase(
        std::remove_if(m_Entries.begin(), m_Entries.end(),
                       [&](const AssetInfo& e) { return e.key == key; }),
        m_Entries.end());
}

static bool MatchSearch(const AssetInfo& e, const std::string& q) {
    if (q.empty()) return true;
    auto lower = [](std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };
    std::string qq = lower(q);
    return lower(e.displayName).find(qq) != std::string::npos ||
           lower(e.key).find(qq) != std::string::npos;
}

std::vector<AssetInfo> AssetLibraryIndex::List(const AssetFilter& filter) const {
    std::vector<AssetInfo> out;
    std::lock_guard<std::mutex> lock(m_Mu);
    out.reserve(m_Entries.size());
    for (const auto& e : m_Entries) {
        if (e.kind != filter.kind && filter.kind != AssetKind::Unknown)
            continue;
        if (e.category == AssetCategory::BuiltIn && !filter.includeCore) continue;
        if (e.category == AssetCategory::User && !filter.includeUser) continue;
        if (e.category == AssetCategory::Project && !filter.includeProject) continue;
        if (e.category == AssetCategory::External && !filter.includeExternal) continue;
        if (!MatchSearch(e, filter.search)) continue;
        out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const AssetInfo& a, const AssetInfo& b) {
        return a.displayName < b.displayName;
    });
    return out;
}

bool AssetLibraryIndex::Find(const std::string& key, AssetInfo& out) const {
    std::lock_guard<std::mutex> lock(m_Mu);
    for (const auto& e : m_Entries) {
        if (e.key == key) {
            out = e;
            return true;
        }
    }
    return false;
}

size_t AssetLibraryIndex::Count() const {
    std::lock_guard<std::mutex> lock(m_Mu);
    return m_Entries.size();
}

} // namespace assets
