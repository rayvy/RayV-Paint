#include "FileExplorer.h"
#include "../core/ProjectManager.h"
#include "../core/Logger.h"
#include "../core/PathUtil.h"
#include "../core/ImageManager.h"
#include "../core/DdsHelper.h"
#include "../core/ConfigManager.h"
#include "../texset/TextureSetIO.h"
#include "widgets/UiTooltip.h"
#include "widgets/UiDropdown.h"
#include "style/UiTokens.h"
#include "EditorPanels.h"

#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <unordered_map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")
#endif

namespace fs = std::filesystem;
namespace UI {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string DefaultStartDir() {
    try {
        return PathUtil::WideToUtf8(fs::current_path().wstring());
    } catch (...) {
        return ".";
    }
}

static std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static bool IsImageExt(const std::string& ext) {
    std::string e = ToLower(ext);
    if (!e.empty() && e[0] != '.') e = "." + e;
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga" ||
           e == ".bmp" || e == ".dds";
}

static std::string JoinPath(const std::string& a, const std::string& b) {
    try {
        return PathUtil::WideToUtf8((PathUtil::FromUtf8(a) / PathUtil::Utf8ToWide(b)).wstring());
    } catch (...) {
        return a + "/" + b;
    }
}

static std::string ParentDir(const std::string& dir) {
    try {
        fs::path p = PathUtil::FromUtf8(dir);
        if (p.has_parent_path() && p.parent_path() != p)
            return PathUtil::WideToUtf8(p.parent_path().wstring());
    } catch (...) {}
    return dir;
}

static std::string FormatSize(uint64_t bytes) {
    char buf[48];
    if (bytes < 1024ull)
        std::snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)bytes);
    else if (bytes < 1024ull * 1024ull)
        std::snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    else if (bytes < 1024ull * 1024ull * 1024ull)
        std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
    else
        std::snprintf(buf, sizeof(buf), "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

static std::string FormatTime(int64_t epochSec) {
    if (epochSec <= 0) return "—";
    std::time_t t = (std::time_t)epochSec;
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    return buf;
}

texset::MapKind GuessMapKindFromFilename(const std::string& filename) {
    std::string fl = ToLower(filename);
    if (fl.find("lightmap") != std::string::npos || fl.find("light_map") != std::string::npos ||
        fl.find("_light") != std::string::npos)
        return texset::MapKind::LightMap;
    if (fl.find("materialmap") != std::string::npos || fl.find("material_map") != std::string::npos ||
        fl.find("_material") != std::string::npos)
        return texset::MapKind::MaterialMap;
    if (fl.find("normalmap") != std::string::npos || fl.find("normal_map") != std::string::npos ||
        fl.find("_normal") != std::string::npos || fl.find("_nml") != std::string::npos)
        return texset::MapKind::NormalMap;
    if (fl.find("glow") != std::string::npos || fl.find("emiss") != std::string::npos)
        return texset::MapKind::GlowMap;
    if (fl.find("extra") != std::string::npos)
        return texset::MapKind::ExtraMap;
    if (fl.find("wengine") != std::string::npos || fl.find("fxmap") != std::string::npos)
        return texset::MapKind::WengineFX;
    return texset::MapKind::Diffuse;
}

// ---------------------------------------------------------------------------
// Directory listing
// ---------------------------------------------------------------------------

struct FsEntry {
    std::string name;
    std::string fullPath;
    bool isDir = false;
    bool isImage = false;
    uint64_t size = 0;
    int64_t mtime = 0;
    std::string typeLabel; // "Folder" / "PNG" / "DDS" …
};

static int64_t FileTimeToEpoch(const fs::file_time_type& ft) {
    try {
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        return (int64_t)std::chrono::system_clock::to_time_t(sctp);
    } catch (...) {
        return 0;
    }
}

static void ListDirectory(const std::string& dir, bool showHidden, std::vector<FsEntry>& out) {
    out.clear();
    std::error_code ec;
    fs::path p = PathUtil::FromUtf8(dir);
    if (!fs::exists(p, ec) || !fs::is_directory(p, ec)) return;

    for (auto& ent : fs::directory_iterator(p, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) { ec.clear(); continue; }
        std::string name = PathUtil::WideToUtf8(ent.path().filename().wstring());
        if (name.empty()) continue;
        if (!showHidden && name[0] == '.') continue;

        FsEntry e;
        e.name = name;
        e.fullPath = PathUtil::WideToUtf8(ent.path().wstring());
        e.isDir = ent.is_directory(ec);
        if (ec) { ec.clear(); continue; }

        if (!e.isDir) {
            auto st = ent.file_size(ec);
            e.size = ec ? 0 : (uint64_t)st;
            ec.clear();
            std::string ext;
            try { ext = ent.path().extension().string(); } catch (...) {}
            e.isImage = IsImageExt(ext);
            std::string el = ToLower(ext);
            if (!el.empty() && el[0] == '.') el = el.substr(1);
            if (el == "dds") {
                // Header-only format sniff (BC7, R8G8, BC6H, …)
                std::string sniff = DdsHelper::SniffFormatLabel(e.fullPath);
                e.typeLabel = sniff.empty() ? "DDS" : sniff;
            } else {
                for (auto& c : el) c = (char)std::toupper((unsigned char)c);
                e.typeLabel = el.empty() ? "File" : el;
            }
        } else {
            e.typeLabel = "Folder";
        }

        try {
            auto ft = ent.last_write_time(ec);
            if (!ec) e.mtime = FileTimeToEpoch(ft);
        } catch (...) {}
        out.push_back(std::move(e));
    }
}

static void SortEntries(std::vector<FsEntry>& entries, ExplorerSortBy by, bool asc) {
    std::sort(entries.begin(), entries.end(), [&](const FsEntry& a, const FsEntry& b) {
        // Folders first always
        if (a.isDir != b.isDir) return a.isDir > b.isDir;
        int cmp = 0;
        switch (by) {
        case ExplorerSortBy::Date:
            cmp = (a.mtime < b.mtime) ? -1 : (a.mtime > b.mtime ? 1 : 0);
            break;
        case ExplorerSortBy::Size:
            cmp = (a.size < b.size) ? -1 : (a.size > b.size ? 1 : 0);
            break;
        case ExplorerSortBy::Type:
            cmp = ToLower(a.typeLabel).compare(ToLower(b.typeLabel));
            break;
        case ExplorerSortBy::Name:
        default:
            cmp = ToLower(a.name).compare(ToLower(b.name));
            break;
        }
        if (cmp == 0) cmp = ToLower(a.name).compare(ToLower(b.name));
        return asc ? (cmp < 0) : (cmp > 0);
    });
}

// ---------------------------------------------------------------------------
// Bookmarks + drives
// ---------------------------------------------------------------------------

struct Bookmark {
    std::string label;
    std::string path;
    bool isDrive = false;
};

#ifdef _WIN32
static std::string KnownFolderPath(REFKNOWNFOLDERID id) {
    PWSTR w = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &w)) && w) {
        std::string u8 = PathUtil::WideToUtf8(w);
        CoTaskMemFree(w);
        return u8;
    }
    return {};
}
#endif

static void BuildBookmarks(std::vector<Bookmark>& out) {
    out.clear();
#ifdef _WIN32
    struct { REFKNOWNFOLDERID id; const char* lab; } kfs[] = {
        { FOLDERID_Desktop,   "Desktop" },
        { FOLDERID_Documents, "Documents" },
        { FOLDERID_Downloads, "Downloads" },
        { FOLDERID_Pictures,  "Pictures" },
        { FOLDERID_Videos,    "Videos" },
        { FOLDERID_Music,     "Music" },
    };
    for (auto& k : kfs) {
        std::string p = KnownFolderPath(k.id);
        if (!p.empty()) out.push_back({ k.lab, p, false });
    }
    // Logical drives
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        char letter = (char)('A' + i);
        char root[4] = { letter, ':', '\\', 0 };
        UINT t = GetDriveTypeA(root);
        if (t == DRIVE_NO_ROOT_DIR) continue;
        const char* kind = "Drive";
        if (t == DRIVE_REMOVABLE) kind = "Removable";
        else if (t == DRIVE_CDROM) kind = "CD/DVD";
        else if (t == DRIVE_REMOTE) kind = "Network";
        else if (t == DRIVE_FIXED) kind = "Local Disk";
        char lab[32];
        std::snprintf(lab, sizeof(lab), "%c: (%s)", letter, kind);
        out.push_back({ lab, std::string(root), true });
    }
#else
    out.push_back({ "Home", DefaultStartDir(), false });
    out.push_back({ "Root", "/", true });
#endif
}

// ---------------------------------------------------------------------------
// Thumbnail cache (lazy, capped)
// ---------------------------------------------------------------------------

struct Thumb {
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11Texture2D* tex = nullptr;
    bool failed = false;
    uint64_t lastUse = 0;
};

static std::unordered_map<std::string, Thumb> g_Thumbs;
static uint64_t g_ThumbClock = 1;
static constexpr size_t kMaxThumbs = 64;
static constexpr int kThumbMaxPx = 128;

static void ReleaseThumb(Thumb& t) {
    if (t.srv) { t.srv->Release(); t.srv = nullptr; }
    if (t.tex) { t.tex->Release(); t.tex = nullptr; }
}

static void ClearAllThumbs() {
    for (auto& kv : g_Thumbs) ReleaseThumb(kv.second);
    g_Thumbs.clear();
}

static void EvictThumbsIfNeeded() {
    while (g_Thumbs.size() > kMaxThumbs) {
        auto oldest = g_Thumbs.end();
        uint64_t best = UINT64_MAX;
        for (auto it = g_Thumbs.begin(); it != g_Thumbs.end(); ++it) {
            if (it->second.lastUse < best) { best = it->second.lastUse; oldest = it; }
        }
        if (oldest == g_Thumbs.end()) break;
        ReleaseThumb(oldest->second);
        g_Thumbs.erase(oldest);
    }
}

static void DownscaleRGBA(const uint8_t* src, int sw, int sh,
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
            dst[di+0]=src[si+0]; dst[di+1]=src[si+1]; dst[di+2]=src[si+2]; dst[di+3]=src[si+3];
        }
    }
}

static bool CreateThumbSrv(ID3D11Device* device, const uint8_t* rgba, int w, int h, Thumb& out) {
    if (!device || !rgba || w <= 0 || h <= 0) return false;
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

static ID3D11ShaderResourceView* GetThumb(ID3D11Device* device, const std::string& path, bool wantLoad) {
    if (!device || path.empty()) return nullptr;
    auto it = g_Thumbs.find(path);
    if (it != g_Thumbs.end()) {
        it->second.lastUse = ++g_ThumbClock;
        return it->second.failed ? nullptr : it->second.srv;
    }
    if (!wantLoad) return nullptr;

    Thumb t;
    t.lastUse = ++g_ThumbClock;

    std::string ext;
    try { ext = fs::path(PathUtil::Utf8ToWide(path)).extension().string(); } catch (...) {}
    ext = ToLower(ext);

    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    bool ok = false;

    if (ext == ".dds") {
        DdsImage img;
        if (DdsHelper::LoadDDS(path, img) && img.width > 0 && img.height > 0 && !img.pixels.empty()) {
            std::vector<uint8_t> full((size_t)img.width * img.height * 4);
            for (int i = 0, n = img.width * img.height; i < n; ++i) {
                full[i*4+0] = (uint8_t)std::clamp(img.pixels[i*4+0] * 255.f, 0.f, 255.f);
                full[i*4+1] = (uint8_t)std::clamp(img.pixels[i*4+1] * 255.f, 0.f, 255.f);
                full[i*4+2] = (uint8_t)std::clamp(img.pixels[i*4+2] * 255.f, 0.f, 255.f);
                full[i*4+3] = (uint8_t)std::clamp(img.pixels[i*4+3] * 255.f, 0.f, 255.f);
            }
            DownscaleRGBA(full.data(), img.width, img.height, rgba, w, h, kThumbMaxPx);
            ok = true;
        }
    } else {
        std::vector<uint8_t> full;
        int fw = 0, fh = 0;
        if (ImageManager::LoadImageFromFile(path, full, fw, fh) && fw > 0 && fh > 0) {
            DownscaleRGBA(full.data(), fw, fh, rgba, w, h, kThumbMaxPx);
            ok = true;
        }
    }

    if (ok && CreateThumbSrv(device, rgba.data(), w, h, t)) {
        EvictThumbsIfNeeded();
        g_Thumbs[path] = t;
        return g_Thumbs[path].srv;
    }
    t.failed = true;
    g_Thumbs[path] = t;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Navigation helpers
// ---------------------------------------------------------------------------

static void NavigateTo(FileExplorerState& st, const std::string& dir) {
    if (dir.empty() || dir == st.currentDir) return;
    if (!st.currentDir.empty())
        st.backStack.push_back(st.currentDir);
    st.forwardStack.clear();
    st.currentDir = dir;
    st.selectedPath.clear();
}

static void NavigateUp(FileExplorerState& st) {
    NavigateTo(st, ParentDir(st.currentDir));
}

static void NavigateBack(FileExplorerState& st) {
    if (st.backStack.empty()) return;
    st.forwardStack.push_back(st.currentDir);
    st.currentDir = st.backStack.back();
    st.backStack.pop_back();
    st.selectedPath.clear();
}

static void NavigateForward(FileExplorerState& st) {
    if (st.forwardStack.empty()) return;
    st.backStack.push_back(st.currentDir);
    st.currentDir = st.forwardStack.back();
    st.forwardStack.pop_back();
    st.selectedPath.clear();
}

// Multi-select helpers
static bool IsMultiSelected(const FileExplorerState& st, const std::string& path) {
    for (const auto& p : st.multiSelect)
        if (ToLower(p) == ToLower(path)) return true;
    return false;
}

static void ToggleMulti(FileExplorerState& st, const std::string& path, bool isImage) {
    if (!isImage) return;
    auto it = std::find_if(st.multiSelect.begin(), st.multiSelect.end(),
        [&](const std::string& p) { return ToLower(p) == ToLower(path); });
    if (it != st.multiSelect.end()) {
        st.multiSelect.erase(it);
        st.importBatch.erase(
            std::remove_if(st.importBatch.begin(), st.importBatch.end(),
                [&](const ImportBatchItem& b) { return ToLower(b.path) == ToLower(path); }),
            st.importBatch.end());
    } else {
        st.multiSelect.push_back(path);
        ImportBatchItem item;
        item.path = path;
        item.kind = GuessMapKindFromFilename(path);
        st.importBatch.push_back(item);
    }
    st.selectedPath = path;
}

static void SelectSingle(FileExplorerState& st, const std::string& path, bool isImage) {
    st.selectedPath = path;
    st.multiSelect.clear();
    st.importBatch.clear();
    if (isImage) {
        st.multiSelect.push_back(path);
        ImportBatchItem item;
        item.path = path;
        item.kind = GuessMapKindFromFilename(path);
        // For single import mode, prefer explicit importMapKind if user set it
        if (st.mode == FileExplorerMode::ImportTexture && !st.importMultiSelect)
            item.kind = st.importMapKind;
        st.importBatch.push_back(item);
    }
}

static ImportBatchItem* FindBatch(FileExplorerState& st, const std::string& path) {
    for (auto& b : st.importBatch)
        if (ToLower(b.path) == ToLower(path)) return &b;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Open / Close
// ---------------------------------------------------------------------------

void FileExplorerOpen(FileExplorerState& st, FileExplorerMode mode, const std::string& startDir) {
    st.open = true;
    st.mode = mode;
    st.status.clear();
    st.selectedPath.clear();
    st.multiSelect.clear();
    st.importBatch.clear();
    st.backStack.clear();
    st.forwardStack.clear();
    if (!startDir.empty())
        st.currentDir = startDir;
    else if (st.currentDir.empty())
        st.currentDir = DefaultStartDir();

    if (mode == FileExplorerMode::ProjectCreate && st.projectType == 1 && st.templateIdx == 0)
        st.templateIdx = 1;
    if (mode == FileExplorerMode::ImportTexture || mode == FileExplorerMode::ProjectCreate)
        st.importMultiSelect = true;
    if (mode == FileExplorerMode::ExportTemplate || mode == FileExplorerMode::PickFolder) {
        if (!st.exportRoot[0])
            std::snprintf(st.exportRoot, sizeof(st.exportRoot), "%s", st.currentDir.c_str());
    }
    if (mode == FileExplorerMode::SaveProject) {
        std::snprintf(st.filterExt, sizeof(st.filterExt), ".rayp");
        if (!st.saveFileName[0])
            std::snprintf(st.saveFileName, sizeof(st.saveFileName), "project.rayp");
    } else if (mode == FileExplorerMode::OpenProject) {
        std::snprintf(st.filterExt, sizeof(st.filterExt), ".rayp");
    } else if (mode == FileExplorerMode::LoadConfig || mode == FileExplorerMode::SaveConfig) {
        std::snprintf(st.filterExt, sizeof(st.filterExt), ".json");
        if (mode == FileExplorerMode::SaveConfig && !st.saveFileName[0])
            std::snprintf(st.saveFileName, sizeof(st.saveFileName), "config.json");
    } else if (mode == FileExplorerMode::AdvancedExport) {
        // Prefer existing export path basename
        std::string exp;
        // filled from canvas when drawing; default:
        if (!st.saveFileName[0])
            std::snprintf(st.saveFileName, sizeof(st.saveFileName), "export.dds");
    }
}

bool FileExplorerApplyAdvancedExport(FileExplorerState& st, Canvas& canvas) {
    std::string name = st.saveFileName;
    if (name.empty()) name = "export.dds";
    if (name.find('.') == std::string::npos) name += ".dds";
    std::string full = JoinPath(st.currentDir, name);
    canvas.SetExportPath(full);

    std::string ext;
    try {
        ext = ToLower(fs::path(PathUtil::Utf8ToWide(name)).extension().string());
        if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
    } catch (...) {}

    bool ok = false;
    if (ext == "dds") {
        ok = canvas.SaveCanvasCompressed(
            full,
            canvas.GetExportFormat(),
            canvas.GetExportGenerateMipMaps(),
            canvas.GetExportMipFilter(),
            canvas.GetExportCompressionSpeed());
    } else {
        ok = canvas.SaveCanvasStandard(full, canvas.GetExportIccPreset());
    }
    st.status = ok ? ("Exported " + full) : "Export failed";
    st.selectedPath = full;
    return ok;
}

void FileExplorerClose(FileExplorerState& st) {
    st.open = false;
}

// ---------------------------------------------------------------------------
// Apply actions
// ---------------------------------------------------------------------------

bool FileExplorerApplyProjectCreate(FileExplorerState& st, ID3D11Device* device) {
    if (!device) {
        st.status = "No GPU device";
        return false;
    }
    auto& pm = ProjectManager::Get();

    Project* proj = pm.ActiveProject();
    if (!proj || !proj->IsBlank()) {
        int id = pm.CreateEmptyProject();
        if (id < 0) {
            st.status = "Failed to create project tab";
            return false;
        }
        proj = pm.FindProject(id);
    }
    if (!proj || !proj->canvas) {
        st.status = "No project";
        return false;
    }
    Canvas& canvas = *proj->canvas;

    const char* templates[] = { "Default", "ZZZ", "GI" };
    int ti = std::clamp(st.templateIdx, 0, 2);

    if (st.projectType == 0) {
        canvas.SetProjectType(Canvas::ProjectType::Simple);
        if (st.baseDiffusePath[0]) {
            if (!canvas.LoadImageToLayer(device, st.baseDiffusePath)) {
                st.status = "Failed to load image";
                return false;
            }
            canvas.SetProjectType(Canvas::ProjectType::Simple);
        }
        proj->textureSets.sets.clear();
        proj->textureSets.activeSetId = -1;
        proj->textureSets.nextId = 1;
        proj->textureSets.EnsureSimpleDefault();
        if (texset::TextureSet* s = proj->textureSets.Active())
            s->name = st.projectName;
        proj->SyncTextureSetsFromCanvas();
        st.status = "Simple project ready";
        return true;
    }

    if (st.projectType == 2) {
        canvas.SetProjectType(Canvas::ProjectType::AdvancedModMode);
        proj->ApplyActiveSetTemplate(templates[ti]);
        if (texset::TextureSet* s = proj->textureSets.Active())
            s->name = st.projectName;
        st.status = "Advanced Mod Mode project (bind INI in Mod Setup)";
        return true;
    }

    // Advanced: either base + auto siblings, or explicit multi-batch
    if (!st.autoPullSiblingMaps && !st.importBatch.empty()) {
        // Manual multi-map assignment
        std::string diffusePath = st.baseDiffusePath;
        if (diffusePath.empty()) {
            for (const auto& b : st.importBatch)
                if (b.kind == texset::MapKind::Diffuse) { diffusePath = b.path; break; }
        }
        if (diffusePath.empty() && !st.importBatch.empty())
            diffusePath = st.importBatch[0].path;

        if (diffusePath.empty()) {
            st.status = "Select at least one texture (Diffuse preferred)";
            return false;
        }

        canvas.SetProjectType(Canvas::ProjectType::Advanced);
        proj->ApplyActiveSetTemplate(templates[ti]);
        if (texset::TextureSet* s = proj->textureSets.Active())
            s->name = st.projectName;

        int n = 0;
        // Load diffuse first for canvas size
        if (proj->ImportMapFile(texset::MapKind::Diffuse, diffusePath))
            ++n;
        for (const auto& b : st.importBatch) {
            if (ToLower(b.path) == ToLower(diffusePath) && b.kind == texset::MapKind::Diffuse)
                continue;
            if (proj->ImportMapFile(b.kind, b.path))
                ++n;
        }
        canvas.SetActiveSetMaps(
            proj->textureSets.Active() ? proj->textureSets.Active()->maps
                                       : std::vector<texset::MapSlot>{});
        canvas.SetViewMapKind(texset::MapKind::Diffuse);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Advanced project: %d map(s) loaded (manual)", n);
        st.status = buf;
        return n > 0;
    }

    if (!st.baseDiffusePath[0]) {
        st.status = "Select a base Diffuse texture";
        return false;
    }

    canvas.SetProjectType(Canvas::ProjectType::Advanced);
    int n = proj->SetupAdvancedFromBaseTexture(
        device, st.baseDiffusePath, templates[ti], st.projectName);
    if (n <= 0) {
        st.status = "Advanced setup failed — check base path / logs";
        return false;
    }

    if (!st.autoPullSiblingMaps) {
        if (texset::TextureSet* set = proj->textureSets.Active()) {
            for (auto& m : set->maps) {
                if (m.kind != texset::MapKind::Diffuse)
                    set->DisableMap(m.kind);
            }
        }
    }

    canvas.SetActiveSetMaps(
        proj->textureSets.Active() ? proj->textureSets.Active()->maps
                                   : std::vector<texset::MapSlot>{});
    canvas.SetViewMapKind(texset::MapKind::Diffuse);

    char buf[128];
    std::snprintf(buf, sizeof(buf), "Advanced project: %d map(s) loaded", n);
    st.status = buf;
    Logger::Get().InfoTag("project", st.status + " base=" + std::string(st.baseDiffusePath));
    return true;
}

bool FileExplorerApplyImport(FileExplorerState& st, Project* project, Canvas& canvas,
                             ID3D11Device* device) {
    if (!project) return false;

    // Multi-batch path
    if (!st.importBatch.empty() && (st.importMultiSelect || st.importBatch.size() > 1)
        && !st.importRemapMode) {
        int okN = 0;
        for (const auto& b : st.importBatch) {
            if (b.path.empty()) continue;
            if (b.kind == texset::MapKind::Diffuse && device) {
                // If no canvas size yet, full load
                if (canvas.GetWidth() <= 0) {
                    if (canvas.LoadImageToLayer(device, b.path)) {
                        if (canvas.GetProjectType() == Canvas::ProjectType::Simple &&
                            project->textureSets.Active() &&
                            project->textureSets.Active()->maps.size() > 1)
                            canvas.SetProjectType(Canvas::ProjectType::Advanced);
                        project->SyncTextureSetsFromCanvas();
                        ++okN;
                    }
                    continue;
                }
            }
            texset::ChannelRole solo = st.importExtractSolo ? st.importSoloRole : texset::ChannelRole::None;
            if (project->ImportMapFile(b.kind, b.path, solo))
                ++okN;
        }
        if (okN > 0 && canvas.GetProjectType() == Canvas::ProjectType::Simple)
            canvas.SetProjectType(Canvas::ProjectType::Advanced);
        if (texset::TextureSet* set = project->textureSets.Active())
            canvas.SetActiveSetMaps(set->maps);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "Imported %d map(s)", okN);
        st.status = buf;
        return okN > 0;
    }

    if (st.selectedPath.empty() && st.importBatch.empty()) return false;
    const std::string path = !st.selectedPath.empty() ? st.selectedPath
        : (st.importBatch.empty() ? std::string() : st.importBatch[0].path);
    if (path.empty()) return false;

    // Remap: source RGBA → arbitrary dest map + channel (can split across maps)
    if (st.importRemapMode) {
        if (!device) { st.status = "No GPU"; return false; }
        texset::TextureSet* set = project->textureSets.Active();
        if (!set) { project->textureSets.EnsureSimpleDefault(); set = project->textureSets.Active(); }
        if (!set) return false;

        std::vector<uint8_t> src;
        int sw = 0, sh = 0;
        std::string ext;
        try { ext = ToLower(fs::path(PathUtil::Utf8ToWide(path)).extension().string()); } catch (...) {}
        if (ext == ".dds") {
            DdsImage img;
            if (!DdsHelper::LoadDDS(path, img) || img.width <= 0) {
                st.status = "DDS load failed";
                return false;
            }
            sw = img.width; sh = img.height;
            src.resize((size_t)sw * sh * 4);
            for (int i = 0, n = sw * sh; i < n; ++i) {
                src[i*4+0] = (uint8_t)std::clamp(img.pixels[i*4+0] * 255.f, 0.f, 255.f);
                src[i*4+1] = (uint8_t)std::clamp(img.pixels[i*4+1] * 255.f, 0.f, 255.f);
                src[i*4+2] = (uint8_t)std::clamp(img.pixels[i*4+2] * 255.f, 0.f, 255.f);
                src[i*4+3] = (uint8_t)std::clamp(img.pixels[i*4+3] * 255.f, 0.f, 255.f);
            }
        } else if (!ImageManager::LoadImageFromFile(path, src, sw, sh)) {
            st.status = "Image load failed";
            return false;
        }

        // Group routes by dest map
        bool anyRoute = false;
        for (int s = 0; s < 4; ++s)
            if (st.remapRoutes[s].enabled) anyRoute = true;
        if (!anyRoute) {
            st.status = "Enable at least one remap route";
            return false;
        }

        std::unordered_map<int, std::vector<float>> destBuf; // mapKind → rgba32f
        auto ensureBuf = [&](int mk) -> std::vector<float>& {
            auto it = destBuf.find(mk);
            if (it == destBuf.end()) {
                std::vector<float> z((size_t)sw * sh * 4, 0.f);
                // default A=1 for empty slots
                for (size_t i = 3; i < z.size(); i += 4) z[i] = 1.f;
                destBuf[mk] = std::move(z);
                return destBuf[mk];
            }
            return it->second;
        };

        for (int sc = 0; sc < 4; ++sc) {
            const RemapRoute& r = st.remapRoutes[sc];
            if (!r.enabled) continue;
            int dc = std::clamp(r.destChan, 0, 3);
            auto& buf = ensureBuf((int)r.destMap);
            for (int i = 0, n = sw * sh; i < n; ++i) {
                buf[(size_t)i * 4 + dc] = src[(size_t)i * 4 + sc] / 255.f;
            }
        }

        if (canvas.GetProjectType() == Canvas::ProjectType::Simple)
            canvas.SetProjectType(Canvas::ProjectType::Advanced);

        int mapsN = 0;
        std::string baseName;
        try {
            baseName = PathUtil::WideToUtf8(fs::path(PathUtil::Utf8ToWide(path)).stem().wstring());
        } catch (...) { baseName = "Remap"; }

        for (auto& kv : destBuf) {
            texset::MapKind mk = (texset::MapKind)kv.first;
            std::string lname = baseName + "_" + texset::MapKindName(mk);
            canvas.CreateLayerFromPixels(device, lname, kv.second, sw, sh);
            if (!canvas.GetLayers().empty()) {
                auto& L = canvas.GetLayers().back();
                L.workSpace = texset::LayerWorkSpace{};
                L.workSpace.mapMask = 0;
                L.workSpace.SetMap(mk, true);
                L.workSpace.channelWriteMask = 0xF;
            }
            set->EnableMap(mk, sw, sh, path);
            ++mapsN;
        }
        canvas.SetActiveSetMaps(set->maps);
        canvas.SetDocumentModified(true);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "Remapped into %d map(s)", mapsN);
        st.status = buf;
        return mapsN > 0;
    }

    texset::MapKind kind = st.importMapKind;
    if (ImportBatchItem* b = FindBatch(st, path))
        kind = b->kind;

    if (kind == texset::MapKind::Diffuse && device) {
        bool ok = canvas.LoadImageToLayer(device, path);
        if (canvas.GetProjectType() == Canvas::ProjectType::Simple &&
            project->textureSets.Active() &&
            project->textureSets.Active()->maps.size() > 1) {
            canvas.SetProjectType(Canvas::ProjectType::Advanced);
        }
        project->SyncTextureSetsFromCanvas();
        st.status = ok ? "Imported Diffuse" : "Import failed";
        return ok;
    }

    texset::ChannelRole solo = st.importExtractSolo ? st.importSoloRole : texset::ChannelRole::None;
    bool ok = project->ImportMapFile(kind, path, solo);
    if (ok && canvas.GetProjectType() == Canvas::ProjectType::Simple)
        canvas.SetProjectType(Canvas::ProjectType::Advanced);
    if (texset::TextureSet* set = project->textureSets.Active())
        canvas.SetActiveSetMaps(set->maps);
    st.status = ok ? "Imported map" : "Import failed";
    return ok;
}

bool FileExplorerApplySaveProject(FileExplorerState& st, Project* project, Canvas& canvas) {
    std::string name = st.saveFileName;
    if (name.empty()) name = "project.rayp";
    if (name.find('.') == std::string::npos) name += ".rayp";
    std::string full = JoinPath(st.currentDir, name);
    if (project) project->InjectTextureSetsIntoCanvas();
    bool ok = canvas.SaveCanvasRayp(full);
    st.status = ok ? ("Saved " + full) : "Save failed";
    st.selectedPath = full;
    return ok;
}

bool FileExplorerApplyOpenProject(FileExplorerState& st, ID3D11Device* device) {
    if (st.selectedPath.empty()) {
        st.status = "Select a .rayp file";
        return false;
    }
    std::string path = PathUtil::NormalizeToUtf8Path(st.selectedPath);
    const int id = ProjectManager::Get().ActivateOrPrepareOpen(path);
    if (id < 0) { st.status = "Open failed"; return false; }
    Project* p = ProjectManager::Get().FindProject(id);
    if (!p || !p->canvas) { st.status = "No project"; return false; }
    UI::TriggerBackgroundOpenDocument(path, device, *p->canvas);
    st.status = "Opening " + path;
    return true;
}

bool FileExplorerApplyLoadConfig(FileExplorerState& st) {
    if (st.selectedPath.empty()) { st.status = "Select a config file"; return false; }
    bool ok = ConfigManager::Get().Load(st.selectedPath);
    st.status = ok ? "Config loaded" : "Load config failed";
    return ok;
}

bool FileExplorerApplySaveConfig(FileExplorerState& st) {
    std::string name = st.saveFileName;
    if (name.empty()) name = "config.json";
    if (name.find('.') == std::string::npos) name += ".json";
    std::string full = JoinPath(st.currentDir, name);
    bool ok = ConfigManager::Get().Save(full);
    st.status = ok ? ("Config saved to " + full) : "Save config failed";
    if (ok) st.selectedPath = full;
    return ok;
}

bool FileExplorerApplyExportTemplate(FileExplorerState& st, Project* project) {
    if (!project) return false;

    // Prefer exportRoot; fall back to current folder selection
    std::string root = st.exportRoot;
    if (root.empty()) root = st.currentDir;
    if (root.empty()) {
        st.status = "Pick an export folder";
        return false;
    }

    // Ensure directory exists
    try {
        fs::create_directories(PathUtil::FromUtf8(root));
    } catch (...) {}

    std::string pattern = st.namePattern[0] ? st.namePattern : "{set}{suffix}";

    auto applySet = [&](texset::TextureSet& set) {
        for (auto& m : set.maps) {
            if (!m.enabled) continue;
            std::string name = pattern;
            auto replaceAll = [](std::string& s, const std::string& a, const std::string& b) {
                size_t pos = 0;
                while ((pos = s.find(a, pos)) != std::string::npos) {
                    s.replace(pos, a.size(), b);
                    pos += b.size();
                }
            };
            replaceAll(name, "{set}", set.name);
            replaceAll(name, "{map}", texset::MapKindName(m.kind));
            replaceAll(name, "{suffix}", m.nameSuffix.empty()
                ? (std::string("_") + texset::MapKindName(m.kind)) : m.nameSuffix);
            std::string ext = "png";
            try {
                fs::path out = PathUtil::FromUtf8(root) / PathUtil::Utf8ToWide(name + "." + ext);
                m.exportPath = PathUtil::WideToUtf8(out.wstring());
            } catch (...) {
                m.exportPath = root + "/" + name + "." + ext;
            }
        }
    };

    if (st.exportAllSets) {
        for (auto& s : project->textureSets.sets)
            applySet(s);
    } else if (texset::TextureSet* a = project->textureSets.Active()) {
        applySet(*a);
    }

    if (project->canvas) {
        project->canvas->SetExportPath(JoinPath(root, "export"));
        project->canvas->SetDocumentModified(true);
        if (st.exportAndRun) {
            int n = project->QuickExportAllMaps(root);
            char buf[96];
            std::snprintf(buf, sizeof(buf), "Exported %d map(s) → %s", n, root.c_str());
            st.status = buf;
            return n > 0;
        }
    }
    st.status = std::string("Export folder set: ") + root;
    return true;
}

// ---------------------------------------------------------------------------
// UI drawing
// ---------------------------------------------------------------------------

static float IconCellSize(ExplorerViewMode m) {
    switch (m) {
    case ExplorerViewMode::SmallIcons:  return 48.f;
    case ExplorerViewMode::MediumIcons: return 88.f;
    case ExplorerViewMode::LargeIcons:  return 140.f;
    default: return 0.f;
    }
}

static void DrawFolderGlyph(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 col) {
    float w = p1.x - p0.x, h = p1.y - p0.y;
    float tabH = h * 0.22f;
    float tabW = w * 0.38f;
    ImVec2 pts[6] = {
        { p0.x, p0.y + tabH },
        { p0.x, p1.y },
        { p1.x, p1.y },
        { p1.x, p0.y + tabH },
        { p0.x + tabW + 2.f, p0.y + tabH },
        { p0.x + tabW * 0.85f, p0.y },
    };
    // Simple folder shape
    dl->AddRectFilled(ImVec2(p0.x, p0.y + tabH), p1, col, 3.f);
    dl->AddRectFilled(ImVec2(p0.x, p0.y), ImVec2(p0.x + tabW, p0.y + tabH + 2.f), col, 2.f);
    (void)pts;
}

static void DrawFileGlyph(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 col, const char* ext) {
    dl->AddRectFilled(p0, p1, col, 3.f);
    dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 40), 3.f);
    if (ext && ext[0]) {
        ImVec2 ts = ImGui::CalcTextSize(ext);
        dl->AddText(ImVec2((p0.x + p1.x - ts.x) * 0.5f, (p0.y + p1.y - ts.y) * 0.5f),
                    IM_COL32(220, 220, 230, 200), ext);
    }
}

static bool DrawNavArrowButton(const char* id, const char* label, bool enabled) {
    if (!enabled) ImGui::BeginDisabled();
    bool hit = ImGui::Button(label, ImVec2(28, 0));
    if (!enabled) ImGui::EndDisabled();
    (void)id;
    return hit && enabled;
}

bool DrawFileExplorer(FileExplorerState& st, Project* project, Canvas& canvas,
                      ID3D11Device* device) {
    if (!st.open) return false;

    const char* title = "File Explorer";
    switch (st.mode) {
    case FileExplorerMode::ProjectCreate: title = "New Project"; break;
    case FileExplorerMode::ImportTexture: title = "Import Maps"; break;
    case FileExplorerMode::ExportTemplate: title = "Export Folder"; break;
    case FileExplorerMode::PickFolder: title = "Select Folder"; break;
    case FileExplorerMode::SaveProject: title = "Save Project"; break;
    case FileExplorerMode::OpenProject: title = "Open Project"; break;
    case FileExplorerMode::LoadConfig: title = "Load Config"; break;
    case FileExplorerMode::SaveConfig: title = "Save Config"; break;
    case FileExplorerMode::AdvancedExport: title = "Advanced Export"; break;
    default: break;
    }

    bool confirmed = false;
    ImGui::SetNextWindowSize(ImVec2(980, 640), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(720, 460), ImVec2(2400, 1800));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
    if (!ImGui::Begin(title, &st.open, flags)) {
        ImGui::End();
        return false;
    }

    const auto& tok = Ui::Tokens();
    const bool isCreate = (st.mode == FileExplorerMode::ProjectCreate);
    const bool isImport = (st.mode == FileExplorerMode::ImportTexture);
    const bool isExport = (st.mode == FileExplorerMode::ExportTemplate ||
                           st.mode == FileExplorerMode::PickFolder);
    const bool isSaveName = (st.mode == FileExplorerMode::SaveProject ||
                             st.mode == FileExplorerMode::SaveConfig ||
                             st.mode == FileExplorerMode::AdvancedExport);
    const bool isOpenFile = (st.mode == FileExplorerMode::OpenProject ||
                             st.mode == FileExplorerMode::LoadConfig);
    const bool isAdvExport = (st.mode == FileExplorerMode::AdvancedExport);
    const bool hasSide = isCreate || isImport || isExport || isSaveName || isOpenFile;
    const bool filterImages = isCreate || isImport;
    const bool filterRayp = (st.mode == FileExplorerMode::OpenProject ||
                             st.mode == FileExplorerMode::SaveProject);
    const bool filterJson = (st.mode == FileExplorerMode::LoadConfig ||
                             st.mode == FileExplorerMode::SaveConfig);

    // ---- Toolbar (pad right so A-Z never clips) ----
    {
        const float toolbarRightReserve = 250.f;
        if (DrawNavArrowButton("##back", "\xE2\x86\x90", !st.backStack.empty()))
            NavigateBack(st);
        if (ImGui::IsItemHovered()) Ui::Tooltip("Back");
        ImGui::SameLine(0, 4);
        if (DrawNavArrowButton("##fwd", "\xE2\x86\x92", !st.forwardStack.empty()))
            NavigateForward(st);
        if (ImGui::IsItemHovered()) Ui::Tooltip("Forward");
        ImGui::SameLine(0, 4);
        if (DrawNavArrowButton("##up", "\xE2\x86\x91", true))
            NavigateUp(st);
        if (ImGui::IsItemHovered()) Ui::Tooltip("Parent folder");
        ImGui::SameLine(0, 8);

        char dirBuf[512];
        std::snprintf(dirBuf, sizeof(dirBuf), "%s", st.currentDir.c_str());
        float pathW = ImGui::GetContentRegionAvail().x - toolbarRightReserve;
        if (pathW < 120.f) pathW = 120.f;
        ImGui::SetNextItemWidth(pathW);
        if (ImGui::InputText("##dir", dirBuf, sizeof(dirBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
            std::error_code ec;
            if (fs::is_directory(PathUtil::FromUtf8(dirBuf), ec))
                NavigateTo(st, dirBuf);
            else
                st.status = "Invalid path";
        }
        ImGui::SameLine(0, 6);
        const char* views[] = { "List", "S", "M", "L" };
        int vm = (int)st.viewMode;
        ImGui::SetNextItemWidth(72.f);
        if (Ui::Combo("##view", &vm, views, 4))
            st.viewMode = (ExplorerViewMode)vm;
        if (ImGui::IsItemHovered()) Ui::Tooltip("View: List / Small / Medium / Large");
        ImGui::SameLine(0, 4);
        const char* sorts[] = { "Name", "Date", "Size", "Type" };
        int sb = (int)st.sortBy;
        ImGui::SetNextItemWidth(72.f);
        if (Ui::Combo("##sort", &sb, sorts, 4))
            st.sortBy = (ExplorerSortBy)sb;
        ImGui::SameLine(0, 4);
        if (ImGui::SmallButton(st.sortAsc ? "A-Z##az" : "Z-A##az"))
            st.sortAsc = !st.sortAsc;
    }

    ImGui::Separator();

    // Layout widths with min/max clamps
    constexpr float kBmMin = 120.f, kBmMax = 320.f;
    constexpr float kSideMin = 200.f, kSideMax = 480.f;
    constexpr float kSplit = 6.f;
    st.bookmarkW = std::clamp(st.bookmarkW, kBmMin, kBmMax);
    st.sideFormW = std::clamp(st.sideFormW, kSideMin, kSideMax);
    float sideFormW = hasSide ? st.sideFormW : 0.f;
    float bookmarkW = st.bookmarkW;
    float footerH = 56.f;
    float bodyH = ImGui::GetContentRegionAvail().y - footerH;
    if (bodyH < 120.f) bodyH = 120.f;

    // ---- Bookmarks ----
    ImGui::BeginChild("##bookmarks", ImVec2(bookmarkW, bodyH), true,
                      0);
    ImGui::TextDisabled("Places");
    ImGui::Separator();
    static std::vector<Bookmark> s_bookmarks;
    static bool s_bmInit = false;
    if (!s_bmInit) { BuildBookmarks(s_bookmarks); s_bmInit = true; }

    bool sawDrive = false;
    for (const auto& bm : s_bookmarks) {
        if (bm.isDrive && !sawDrive) {
            ImGui::Spacing();
            ImGui::TextDisabled("Drives");
            ImGui::Separator();
            sawDrive = true;
        }
        bool active = ToLower(st.currentDir) == ToLower(bm.path) ||
                      (st.currentDir.size() >= bm.path.size() &&
                       ToLower(st.currentDir.substr(0, bm.path.size())) == ToLower(bm.path) &&
                       !bm.isDrive);
        if (active)
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(tok.accent.x, tok.accent.y, tok.accent.z, 0.35f));
        if (ImGui::Selectable(bm.label.c_str(), active))
            NavigateTo(st, bm.path);
        if (active) ImGui::PopStyleColor();
    }
    if (ImGui::SmallButton("Refresh places")) {
        BuildBookmarks(s_bookmarks);
    }
    ImGui::EndChild();

    // Splitter bookmarks | browser
    ImGui::SameLine(0, 0);
    ImGui::InvisibleButton("##split_bm", ImVec2(kSplit, bodyH));
    if (ImGui::IsItemActive()) {
        st.bookmarkW = std::clamp(st.bookmarkW + ImGui::GetIO().MouseDelta.x, kBmMin, kBmMax);
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    // Visual grip
    {
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(a, b, IM_COL32(255, 255, 255, 18));
    }
    ImGui::SameLine(0, 0);

    // ---- Browser ----
    float browserW = ImGui::GetContentRegionAvail().x - (sideFormW > 0 ? sideFormW + kSplit : 0.f);
    if (browserW < 160.f) browserW = 160.f;
    ImGui::BeginChild("##browser", ImVec2(browserW, bodyH), true,
                      0);

    std::vector<FsEntry> entries;
    ListDirectory(st.currentDir, st.showHidden, entries);
    SortEntries(entries, st.sortBy, st.sortAsc);

    // Filters by mode
    if (filterImages) {
        entries.erase(std::remove_if(entries.begin(), entries.end(),
            [](const FsEntry& e) { return !e.isDir && !e.isImage; }), entries.end());
    } else if (filterRayp) {
        entries.erase(std::remove_if(entries.begin(), entries.end(),
            [](const FsEntry& e) {
                if (e.isDir) return false;
                auto el = ToLower(e.name);
                return el.size() < 5 || el.substr(el.size() - 5) != ".rayp";
            }), entries.end());
    } else if (filterJson) {
        entries.erase(std::remove_if(entries.begin(), entries.end(),
            [](const FsEntry& e) {
                if (e.isDir) return false;
                auto el = ToLower(e.name);
                return el.size() < 5 || el.substr(el.size() - 5) != ".json";
            }), entries.end());
    }

    // Multi-select: import always; create always (esp. advanced manual maps)
    const bool multiMode = (isImport && st.importMultiSelect) || isCreate;
    ImGuiIO& io = ImGui::GetIO();

    auto onActivate = [&](const FsEntry& e) {
        if (e.isDir) {
            NavigateTo(st, e.fullPath);
            if (isExport)
                std::snprintf(st.exportRoot, sizeof(st.exportRoot), "%s", e.fullPath.c_str());
            return;
        }
        if (multiMode && (io.KeyCtrl || io.KeyShift)) {
            ToggleMulti(st, e.fullPath, e.isImage);
        } else {
            SelectSingle(st, e.fullPath, e.isImage);
        }
        if (isCreate && e.isImage) {
            std::snprintf(st.baseDiffusePath, sizeof(st.baseDiffusePath), "%s", e.fullPath.c_str());
            std::snprintf(st.importFolder, sizeof(st.importFolder), "%s", st.currentDir.c_str());
            try {
                std::string stem = PathUtil::WideToUtf8(
                    fs::path(PathUtil::Utf8ToWide(e.name)).stem().wstring());
                auto low = ToLower(stem);
                for (const char* t : {"diffuse","lightmap","materialmap","normalmap",
                                      "_diffuse","_lightmap","_materialmap","_normalmap"}) {
                    auto pos = low.rfind(t);
                    if (pos != std::string::npos && pos + std::strlen(t) == low.size()) {
                        stem = stem.substr(0, pos);
                        while (!stem.empty() && (stem.back()=='_'||stem.back()=='-')) stem.pop_back();
                        break;
                    }
                }
                if (!stem.empty() && stem.size() < sizeof(st.projectName))
                    std::snprintf(st.projectName, sizeof(st.projectName), "%s", stem.c_str());
            } catch (...) {}
        }
        if (isExport) {
            // Selecting a file → use its parent folder
            std::snprintf(st.exportRoot, sizeof(st.exportRoot), "%s", st.currentDir.c_str());
        }
    };

    auto onDouble = [&](const FsEntry& e) {
        if (e.isDir) {
            NavigateTo(st, e.fullPath);
            return;
        }
        if (isImport && e.isImage) {
            SelectSingle(st, e.fullPath, true);
            confirmed = FileExplorerApplyImport(st, project, canvas, device);
            if (confirmed) FileExplorerClose(st);
        }
        if (isCreate && e.isImage) {
            std::snprintf(st.baseDiffusePath, sizeof(st.baseDiffusePath), "%s", e.fullPath.c_str());
        }
    };

    const float cell = IconCellSize(st.viewMode);
    const bool iconView = (st.viewMode != ExplorerViewMode::Details);

    if (!iconView) {
        // Details table
        if (ImGui::BeginTable("##fstable", 4,
                ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 72);
            ImGui::TableSetupColumn("Date modified", ImGuiTableColumnFlags_WidthFixed, 130);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin((int)entries.size());
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const FsEntry& e = entries[i];
                    ImGui::PushID(i);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    bool sel = (st.selectedPath == e.fullPath) || IsMultiSelected(st, e.fullPath);
                    std::string label = (e.isDir ? "[dir] " : "") + e.name;
                    if (ImGui::Selectable(label.c_str(), sel,
                            ImGuiSelectableFlags_SpanAllColumns |
                            ImGuiSelectableFlags_AllowDoubleClick)) {
                        onActivate(e);
                    }
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
                        onDouble(e);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(e.typeLabel.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(FormatTime(e.mtime).c_str());
                    ImGui::TableNextColumn();
                    if (e.isDir) ImGui::TextDisabled("—");
                    else ImGui::TextUnformatted(FormatSize(e.size).c_str());
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
    } else {
        // Icon grid
        float avail = ImGui::GetContentRegionAvail().x;
        float spacing = 10.f;
        float cellW = cell + 8.f;
        int cols = std::max(1, (int)((avail + spacing) / (cellW + spacing)));
        int col = 0;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        for (int i = 0; i < (int)entries.size(); ++i) {
            const FsEntry& e = entries[i];
            ImGui::PushID(i);
            if (col > 0) ImGui::SameLine(0, spacing);

            bool sel = (st.selectedPath == e.fullPath) || IsMultiSelected(st, e.fullPath);
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImVec2 box(cellW, cell + 28.f);

            ImGui::InvisibleButton("##cell", box);
            bool hovered = ImGui::IsItemHovered();
            bool clicked = ImGui::IsItemClicked();
            bool dbl = hovered && ImGui::IsMouseDoubleClicked(0);

            ImU32 bg = sel ? IM_COL32(60, 100, 180, 90)
                           : (hovered ? IM_COL32(255, 255, 255, 18) : 0);
            if (bg) dl->AddRectFilled(p0, ImVec2(p0.x + box.x, p0.y + box.y), bg, 6.f);
            if (sel) dl->AddRect(p0, ImVec2(p0.x + box.x, p0.y + box.y),
                                 IM_COL32(90, 140, 255, 200), 6.f, 0, 1.5f);

            float pad = 6.f;
            ImVec2 ip0(p0.x + (cellW - cell) * 0.5f + 2.f, p0.y + pad);
            ImVec2 ip1(ip0.x + cell - 4.f, ip0.y + cell - 4.f);

            if (e.isDir) {
                DrawFolderGlyph(dl, ip0, ip1, IM_COL32(230, 190, 80, 220));
            } else if (e.isImage) {
                ID3D11ShaderResourceView* srv = GetThumb(device, e.fullPath, hovered || sel ||
                    st.viewMode == ExplorerViewMode::LargeIcons ||
                    st.viewMode == ExplorerViewMode::MediumIcons);
                if (srv) {
                    dl->AddImage((ImTextureID)srv, ip0, ip1);
                    dl->AddRect(ip0, ip1, IM_COL32(0, 0, 0, 80), 2.f);
                } else {
                    DrawFileGlyph(dl, ip0, ip1, IM_COL32(50, 55, 65, 255), e.typeLabel.c_str());
                }
            } else {
                DrawFileGlyph(dl, ip0, ip1, IM_COL32(55, 58, 68, 255), e.typeLabel.c_str());
            }

            // Name under icon
            ImVec2 ts = ImGui::CalcTextSize(e.name.c_str(), nullptr, false, cellW - 4.f);
            float tx = p0.x + 2.f;
            float ty = p0.y + cell + 2.f;
            ImVec4 clip(p0.x, ty, p0.x + cellW, p0.y + box.y);
            dl->PushClipRect(ImVec2(clip.x, clip.y), ImVec2(clip.z, clip.w), true);
            // Truncate visually
            std::string shown = e.name;
            if (ts.x > cellW - 6.f) {
                while (shown.size() > 3 && ImGui::CalcTextSize((shown + "…").c_str()).x > cellW - 6.f)
                    shown.pop_back();
                shown += "…";
            }
            dl->AddText(ImVec2(tx, ty), IM_COL32(230, 230, 235, 255), shown.c_str());
            dl->PopClipRect();

            if (clicked) onActivate(e);
            if (dbl) onDouble(e);

            if (hovered) {
                char tip[512];
                if (e.isDir)
                    std::snprintf(tip, sizeof(tip), "%s\nFolder", e.name.c_str());
                else
                    std::snprintf(tip, sizeof(tip), "%s\n%s  ·  %s  ·  %s",
                                  e.name.c_str(), e.typeLabel.c_str(),
                                  FormatSize(e.size).c_str(), FormatTime(e.mtime).c_str());
                Ui::Tooltip(tip);
            }

            ImGui::PopID();
            if (++col >= cols) col = 0;
        }
    }

    ImGui::EndChild();

    // ---- Side form ----
    if (sideFormW > 0.f) {
        ImGui::SameLine(0, 0);
        ImGui::InvisibleButton("##split_side", ImVec2(kSplit, bodyH));
        if (ImGui::IsItemActive()) {
            // Dragging right edge of browser → change side form width inverted
            st.sideFormW = std::clamp(st.sideFormW - ImGui::GetIO().MouseDelta.x, kSideMin, kSideMax);
        }
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        {
            ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRectFilled(a, b, IM_COL32(255, 255, 255, 18));
        }
        ImGui::SameLine(0, 0);
        ImGui::BeginChild("##sideform", ImVec2(sideFormW, bodyH), true,
                          0);

        if (isCreate) {
            ImGui::TextUnformatted("New Project");
            ImGui::Separator();
            ImGui::InputText("Name", st.projectName, sizeof(st.projectName));
            const char* types[] = { "Simple", "Advanced", "Advanced Mod Mode" };
            Ui::Combo("##ptype", &st.projectType, types, 3, "Mode");

            if (st.projectType == 1) {
                const char* temps[] = { "Default", "ZZZ", "GI" };
                Ui::Combo("##ptempl", &st.templateIdx, temps, 3, "Template");
                ImGui::Spacing();
                ImGui::TextUnformatted("Base Diffuse");
                ImGui::TextWrapped("%s", st.baseDiffusePath[0] ? st.baseDiffusePath : "(select a texture)");
                ImGui::Checkbox("Auto-pull sibling maps", &st.autoPullSiblingMaps);
                if (ImGui::IsItemHovered())
                    Ui::Tooltip("Auto-find LightMap / MaterialMap / NormalMap by name stem");

                if (st.autoPullSiblingMaps && st.baseDiffusePath[0]) {
                    ImGui::Spacing();
                    ImGui::TextDisabled("Will import:");
                    ImGui::BulletText("Diffuse ← base");
                    for (const auto& e : entries) {
                        if (e.isDir || !e.isImage) continue;
                        if (ToLower(e.fullPath) == ToLower(st.baseDiffusePath)) continue;
                        texset::MapKind gk = GuessMapKindFromFilename(e.name);
                        if (gk != texset::MapKind::Diffuse)
                            ImGui::BulletText("%s ← %s", texset::MapKindName(gk), e.name.c_str());
                    }
                } else if (!st.autoPullSiblingMaps) {
                    ImGui::Spacing();
                    ImGui::TextDisabled("Manual map assignment");
                    ImGui::TextWrapped("Ctrl+click textures, then set map kind per file:");
                    const char* kinds[] = {
                        "Diffuse", "LightMap", "MaterialMap", "NormalMap",
                        "ExtraMap", "GlowMap", "WengineFX"
                    };
                    for (int bi = 0; bi < (int)st.importBatch.size(); ++bi) {
                        auto& b = st.importBatch[bi];
                        ImGui::PushID(bi);
                        std::string shortN;
                        try {
                            shortN = PathUtil::WideToUtf8(
                                fs::path(PathUtil::Utf8ToWide(b.path)).filename().wstring());
                        } catch (...) { shortN = b.path; }
                        ImGui::TextUnformatted(shortN.c_str());
                        int ki = (int)b.kind;
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::Combo("##mk", &ki, kinds, 7))
                            b.kind = (texset::MapKind)ki;
                        ImGui::PopID();
                    }
                    if (st.importBatch.empty())
                        ImGui::TextDisabled("(Ctrl+click files to add)");
                }
            } else if (st.projectType == 0) {
                ImGui::TextDisabled("Optional start image");
                if (st.baseDiffusePath[0])
                    ImGui::TextWrapped("%s", st.baseDiffusePath);
            } else {
                ImGui::TextDisabled("Advanced Mod: bind INI later in Mod Setup");
            }
        } else if (isImport) {
            ImGui::TextUnformatted("Import Maps");
            ImGui::Separator();
            ImGui::Checkbox("Multi-select", &st.importMultiSelect);
            if (ImGui::IsItemHovered())
                Ui::Tooltip("Ctrl+click to select several textures");

            const char* kinds[] = {
                "Diffuse", "LightMap", "MaterialMap", "NormalMap",
                "ExtraMap", "GlowMap", "WengineFX"
            };
            const char* chn[] = { "R", "G", "B", "A" };

            ImGui::Checkbox("Remap channels", &st.importRemapMode);
            if (ImGui::IsItemHovered())
                Ui::Tooltip(
                    "ON: route source R/G/B/A into any map+channel\n"
                    "(e.g. R→Material.G, G→LightMap.R). No 'what map is this'.\n"
                    "OFF: assign each file as a whole map type.");

            if (st.importRemapMode) {
                ImGui::TextDisabled("Source → Dest map · channel");
                for (int i = 0; i < 4; ++i) {
                    ImGui::PushID(i);
                    ImGui::Checkbox(chn[i], &st.remapRoutes[i].enabled);
                    ImGui::SameLine();
                    ImGui::TextUnformatted("\xE2\x86\x92"); // →
                    ImGui::SameLine();
                    int di = (int)st.remapRoutes[i].destMap;
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
                    if (ImGui::Combo("##dm", &di, kinds, 7))
                        st.remapRoutes[i].destMap = (texset::MapKind)di;
                    ImGui::SameLine();
                    int dc = st.remapRoutes[i].destChan;
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::Combo("##dc", &dc, chn, 4))
                        st.remapRoutes[i].destChan = dc;
                    ImGui::PopID();
                }
                if (!st.selectedPath.empty())
                    ImGui::TextWrapped("%s", st.selectedPath.c_str());
            } else {
                if (!st.importMultiSelect) {
                    int ki = (int)st.importMapKind;
                    if (ImGui::Combo("As map", &ki, kinds, 7))
                        st.importMapKind = (texset::MapKind)ki;
                }
                ImGui::Spacing();
                ImGui::TextDisabled("Selection (%d)", (int)st.importBatch.size());
                for (int bi = 0; bi < (int)st.importBatch.size(); ++bi) {
                    auto& b = st.importBatch[bi];
                    ImGui::PushID(1000 + bi);
                    std::string shortN;
                    try {
                        shortN = PathUtil::WideToUtf8(
                            fs::path(PathUtil::Utf8ToWide(b.path)).filename().wstring());
                    } catch (...) { shortN = b.path; }
                    ImGui::TextUnformatted(shortN.c_str());
                    if (st.importMultiSelect) {
                        int ki = (int)b.kind;
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::Combo("##mk", &ki, kinds, 7))
                            b.kind = (texset::MapKind)ki;
                    }
                    ImGui::PopID();
                }
                if (st.importBatch.empty())
                    ImGui::TextDisabled("Click a texture to select");
            }
        } else if (isAdvExport) {
            ImGui::TextUnformatted("Advanced Export");
            ImGui::Separator();
            ImGui::TextDisabled("Folder = browser path");
            ImGui::InputText("File name", st.saveFileName, sizeof(st.saveFileName));
            ImGui::TextWrapped("%s", JoinPath(st.currentDir, st.saveFileName).c_str());

            std::string name = st.saveFileName;
            std::string ext;
            try {
                ext = ToLower(fs::path(PathUtil::Utf8ToWide(name)).extension().string());
                if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
            } catch (...) {}

            ImGui::Spacing();
            ImGui::Separator();
            if (ext == "dds" || ext.empty()) {
                ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.f, 1.f), "DDS settings");
                static const char* formats[] = {
                    "BC1 (Linear, DXT1)", "BC1 (sRGB, DX 10+)",
                    "BC2 (Linear, DXT3)", "BC2 (sRGB, DX 10+)",
                    "BC3 (Linear, DXT5)", "BC3 (sRGB, DX 10+)", "BC3 (Linear, RXGB)",
                    "BC4 (Linear, Unsigned)", "BC4 (Linear, Unsigned, ATI1)",
                    "BC5 (Linear, Unsigned)", "BC5 (Linear, Unsigned, ATI2)", "BC5 (Linear, Signed)",
                    "BC6H (Linear, Unsigned, DX 11+)",
                    "BC7 (Linear, DX 11+)", "BC7 (sRGB, DX 11+)",
                    "B8G8R8A8 (Linear, A8R8G8B8)", "B8G8R8A8 (sRGB, DX 10+)",
                    "B8G8R8X8 (Linear, X8R8G8B8)", "B8G8R8X8 (sRGB, DX 10+)",
                    "R8G8B8A8 (Linear, A8B8G8R8)", "R8G8B8A8 (sRGB, DX 10+)",
                    "R8G8B8X8 (Linear, X8B8G8R8)",
                    "B5G5R5A1 (Linear, A1R5G5B5)", "B4G4R4A4 (Linear, A4R4G4B4)",
                    "B5G6R5 (Linear, R5G6B5)", "B8G8R8 (Linear, R8G8B8)",
                    "R8 (Linear, Unsigned, L8)",
                    "R8G8 (Linear, Unsigned, A8L8)", "R8G8 (Linear, Signed, V8U8)",
                    "R32 (Linear, Float)",
                    "RGBA16_FLOAT", "RGBA32_FLOAT", "RGBA8_UNORM"
                };
                int fi = 14;
                std::string cur = canvas.GetExportFormat();
                for (int i = 0; i < (int)(sizeof(formats) / sizeof(formats[0])); ++i)
                    if (cur == formats[i]) fi = i;
                ImGui::SetNextItemWidth(-1);
                if (Ui::Combo("##fmt", &fi, formats, (int)(sizeof(formats) / sizeof(formats[0])), "Format"))
                    canvas.SetExportFormat(formats[fi]);

                bool mips = canvas.GetExportGenerateMipMaps();
                if (ImGui::Checkbox("Generate Mipmaps", &mips))
                    canvas.SetExportGenerateMipMaps(mips);
                if (mips) {
                    const char* filters[] = { "Point", "Box", "Linear", "Cubic", "Fant", "Lanczos" };
                    int fli = 3;
                    std::string cf = canvas.GetExportMipFilter();
                    for (int i = 0; i < 6; ++i) if (cf == filters[i]) fli = i;
                    if (Ui::Combo("##mipf", &fli, filters, 6, "Mip Filter"))
                        canvas.SetExportMipFilter(filters[fli]);
                }
                const char* speeds[] = { "Fast", "Medium", "Slow", "Best" };
                int si = 1;
                std::string cs = canvas.GetExportCompressionSpeed();
                for (int i = 0; i < 4; ++i) if (cs == speeds[i]) si = i;
                if (Ui::Combo("##qual", &si, speeds, 4, "Quality"))
                    canvas.SetExportCompressionSpeed(speeds[si]);
            } else if (ext == "png") {
                ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.f, 1.f), "PNG settings");
                const char* iccs[] = { "sRGB", "Linear", "AdobeRGB", "DisplayP3", "None" };
                int ii = 0;
                std::string ic = Canvas::IccPresetName(canvas.GetExportIccPreset());
                for (int i = 0; i < 5; ++i) if (ic == iccs[i]) ii = i;
                if (Ui::Combo("##icc", &ii, iccs, 5, "ICC"))
                    canvas.SetExportIccPreset(Canvas::IccPresetFromName(iccs[ii]));
            } else {
                ImGui::TextDisabled("Extension: .%s", ext.c_str());
            }
        } else if (isSaveName) {
            ImGui::TextUnformatted(st.mode == FileExplorerMode::SaveProject ? "Save Project" : "Save Config");
            ImGui::Separator();
            ImGui::TextDisabled("Folder: current left path");
            ImGui::InputText("File name", st.saveFileName, sizeof(st.saveFileName));
            ImGui::TextWrapped("%s", JoinPath(st.currentDir, st.saveFileName).c_str());
        } else if (isOpenFile) {
            ImGui::TextUnformatted(st.mode == FileExplorerMode::OpenProject ? "Open Project" : "Load Config");
            ImGui::Separator();
            if (!st.selectedPath.empty())
                ImGui::TextWrapped("%s", st.selectedPath.c_str());
            else
                ImGui::TextDisabled("Select a file in the list");
        } else if (isExport) {
            ImGui::TextUnformatted("Export folder");
            ImGui::Separator();
            ImGui::TextWrapped("Choose a folder — maps will be written there. No need to pick a file.");
            ImGui::Spacing();
            if (ImGui::Button("Use current folder", ImVec2(-1, 0))) {
                std::snprintf(st.exportRoot, sizeof(st.exportRoot), "%s", st.currentDir.c_str());
            }
            ImGui::InputText("Folder", st.exportRoot, sizeof(st.exportRoot));
            ImGui::InputText("Pattern", st.namePattern, sizeof(st.namePattern));
            if (ImGui::IsItemHovered())
                Ui::Tooltip("{set} {map} {suffix}");
            ImGui::Checkbox("All texture sets", &st.exportAllSets);
            ImGui::Checkbox("Export now after apply", &st.exportAndRun);
            if (ImGui::IsItemHovered())
                Ui::Tooltip("Assign paths and run Quick Export into this folder");
            ImGui::Spacing();
            ImGui::TextDisabled("Current:");
            ImGui::TextWrapped("%s", st.exportRoot[0] ? st.exportRoot : st.currentDir.c_str());
        }

        // Preview pane for selected image
        if (!st.selectedPath.empty() && device) {
            bool isImg = false;
            try {
                isImg = IsImageExt(fs::path(PathUtil::Utf8ToWide(st.selectedPath)).extension().string());
            } catch (...) {}
            if (isImg) {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::TextDisabled("Preview");
                if (ID3D11ShaderResourceView* srv = GetThumb(device, st.selectedPath, true)) {
                    float pw = ImGui::GetContentRegionAvail().x;
                    ImGui::Image((ImTextureID)srv, ImVec2(pw, pw * 0.75f));
                }
                try {
                    auto p = PathUtil::FromUtf8(st.selectedPath);
                    auto sz = fs::file_size(p);
                    auto ft = fs::last_write_time(p);
                    ImGui::Text("%s", FormatSize((uint64_t)sz).c_str());
                    ImGui::Text("%s", FormatTime(FileTimeToEpoch(ft)).c_str());
                    ImGui::TextDisabled("%s", p.extension().string().c_str());
                } catch (...) {}
            }
        }

        ImGui::EndChild();
    }

    // ---- Footer ----
    if (!st.status.empty()) {
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.f), "%s", st.status.c_str());
        ImGui::SameLine();
    }

    float right = 220.f;
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - right - 16.f);
    if (ImGui::Button("Cancel", ImVec2(96, 0)))
        FileExplorerClose(st);
    ImGui::SameLine();

    bool canOk = true;
    const char* okLabel = "OK";
    if (st.mode == FileExplorerMode::ProjectCreate) {
        okLabel = "Create";
        if (st.projectType == 1) {
            if (st.autoPullSiblingMaps && !st.baseDiffusePath[0]) canOk = false;
            if (!st.autoPullSiblingMaps && st.importBatch.empty() && !st.baseDiffusePath[0]) canOk = false;
        }
    } else if (st.mode == FileExplorerMode::ImportTexture) {
        okLabel = (st.importBatch.size() > 1 && !st.importRemapMode) ? "Import All" : "Import";
        canOk = !st.selectedPath.empty() || !st.importBatch.empty();
    } else if (st.mode == FileExplorerMode::ExportTemplate) {
        okLabel = st.exportAndRun ? "Export" : "Set Folder";
        canOk = st.exportRoot[0] != 0 || !st.currentDir.empty();
    } else if (st.mode == FileExplorerMode::PickFolder) {
        okLabel = "Select";
        canOk = !st.currentDir.empty();
    } else if (st.mode == FileExplorerMode::SaveProject) {
        okLabel = "Save";
        canOk = st.saveFileName[0] != 0;
    } else if (st.mode == FileExplorerMode::OpenProject) {
        okLabel = "Open";
        canOk = !st.selectedPath.empty();
    } else if (st.mode == FileExplorerMode::LoadConfig) {
        okLabel = "Load";
        canOk = !st.selectedPath.empty();
    } else if (st.mode == FileExplorerMode::SaveConfig) {
        okLabel = "Save";
        canOk = st.saveFileName[0] != 0;
    } else if (st.mode == FileExplorerMode::AdvancedExport) {
        okLabel = "Export";
        canOk = st.saveFileName[0] != 0;
    }

    if (!canOk) ImGui::BeginDisabled();
    if (ImGui::Button(okLabel, ImVec2(100, 0))) {
        switch (st.mode) {
        case FileExplorerMode::ProjectCreate:
            confirmed = FileExplorerApplyProjectCreate(st, device);
            break;
        case FileExplorerMode::ImportTexture:
            confirmed = FileExplorerApplyImport(st, project, canvas, device);
            break;
        case FileExplorerMode::ExportTemplate:
            if (!st.exportRoot[0])
                std::snprintf(st.exportRoot, sizeof(st.exportRoot), "%s", st.currentDir.c_str());
            confirmed = FileExplorerApplyExportTemplate(st, project);
            break;
        case FileExplorerMode::PickFolder:
            std::snprintf(st.exportRoot, sizeof(st.exportRoot), "%s", st.currentDir.c_str());
            st.selectedPath = st.currentDir;
            confirmed = true;
            break;
        case FileExplorerMode::SaveProject:
            confirmed = FileExplorerApplySaveProject(st, project, canvas);
            break;
        case FileExplorerMode::OpenProject:
            confirmed = FileExplorerApplyOpenProject(st, device);
            break;
        case FileExplorerMode::LoadConfig:
            confirmed = FileExplorerApplyLoadConfig(st);
            break;
        case FileExplorerMode::SaveConfig:
            confirmed = FileExplorerApplySaveConfig(st);
            break;
        case FileExplorerMode::AdvancedExport:
            confirmed = FileExplorerApplyAdvancedExport(st, canvas);
            break;
        default:
            confirmed = !st.selectedPath.empty();
            break;
        }
        if (confirmed) FileExplorerClose(st);
    }
    if (!canOk) ImGui::EndDisabled();

    ImGui::End();
    return confirmed;
}

} // namespace UI
