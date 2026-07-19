#include "ProjectManager.h"
#include "Logger.h"
#include "PathUtil.h"
#include "ImageManager.h"
#include "ConfigManager.h"
#include "MemoryStats.h"
#include "../texset/TextureSetIO.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

ProjectManager& ProjectManager::Get() {
    static ProjectManager inst;
    return inst;
}

std::string Project::GetTabTitle() const {
    if (!canvas) return "Untitled";
    const std::string& path = canvas->GetCurrentProjectFilePath();
    if (!path.empty()) {
        try {
            auto fn = fs::path(PathUtil::Utf8ToWide(path)).filename();
            return PathUtil::WideToUtf8(fn.wstring());
        } catch (...) {
            size_t slash = path.find_last_of("/\\");
            return (slash == std::string::npos) ? path : path.substr(slash + 1);
        }
    }
    if (untitledIndex > 0)
        return "Untitled-" + std::to_string(untitledIndex);
    return "Untitled";
}

bool Project::IsBlank() const {
    if (!canvas) return true;
    if (canvas->IsDocumentModified()) return false;
    if (!canvas->GetCurrentProjectFilePath().empty()) return false;
    // Untitled + unmodified = reusable starter tab (even with default 1024×1024 Background).
    return true;
}

void Project::SyncTextureSetsFromCanvas() {
    if (!canvas) return;
    textureSets.EnsureSimpleDefault();
    texset::TextureSet* set = textureSets.Active();
    if (!set) return;
    int w = canvas->GetWidth();
    int h = canvas->GetHeight();
    if (w > 0 && h > 0)
        set->EnableMap(texset::MapKind::Diffuse, w, h, canvas->GetCurrentProjectFilePath());
    // Align set name with project tab when still default
    if (set->name == "Document" || set->name == "Texture Set")
        set->name = GetTabTitle();
}

void Project::InjectTextureSetsIntoCanvas() {
    if (!canvas) return;
    SyncTextureSetsFromCanvas();
    canvas->SetTextureSetsMetaJson(textureSets.MetaToJson());
}

void Project::ApplyTextureSetsFromCanvas() {
    if (!canvas) return;
    const std::string& j = canvas->GetTextureSetsMetaJson();
    if (!j.empty()) {
        // MetaFromJson rebuilds in place (TextureSetLibrary is non-copyable)
        texset::TextureSetLibrary::MetaFromJson(j, textureSets);
    }
    textureSets.EnsureSimpleDefault();
    SyncTextureSetsFromCanvas();
}

int Project::AddTextureSet(const std::string& name, const std::string& templateId) {
    texset::SetTemplate t = texset::Template_Default();
    if (templateId == "ZZZ" || templateId == "zzz") t = texset::Template_ZZZ();
    else if (templateId == "GI" || templateId == "gi") t = texset::Template_GI();
    auto set = texset::TextureSet::CreateFromTemplate(name.empty() ? "Texture Set" : name, t);
    // Match canvas size for Diffuse if available
    if (canvas && canvas->GetWidth() > 0)
        set.EnableMap(texset::MapKind::Diffuse, canvas->GetWidth(), canvas->GetHeight());
    int id = textureSets.AddSet(std::move(set));
    textureSets.SetActive(id);
    if (canvas) canvas->SetDocumentModified(true);
    return id;
}

bool Project::ImportMapFile(texset::MapKind kind, const std::string& filepath,
                            texset::ChannelRole soloRole) {
    textureSets.EnsureSimpleDefault();
    texset::TextureSet* set = textureSets.Active();
    if (!set || !canvas) return false;

    ID3D11Device* device = ProjectManager::Get().GetDevice();
    if (!device) return false;

    // Real layer in Layers panel (user-visible). Not a hidden mapComposites-only store.
    std::string layerName;
    try {
        layerName = PathUtil::WideToUtf8(
            fs::path(PathUtil::Utf8ToWide(filepath)).filename().wstring());
    } catch (...) {
        layerName = filepath;
    }

    // Blank starter tab has empty "Background" at default size (often 1024/4096).
    // Loading Diffuse must adopt image native size — never UV-upsample into blank size.
    auto isBlankStarter = [&]() {
        if (canvas->GetWidth() <= 0 || canvas->GetHeight() <= 0) return true;
        if (canvas->GetLayers().empty()) return true;
        if (canvas->GetLayers().size() == 1) {
            const auto& L = canvas->GetLayers()[0];
            if (L.name == "Background" &&
                (!L.tileCache || L.tileCache->IsEmpty()) &&
                !L.IsFill() && !L.hasMask)
                return true;
        }
        return false;
    };

    bool ok = false;
    if (kind == texset::MapKind::Diffuse && isBlankStarter()) {
        ok = canvas->LoadImageToLayer(device, filepath);
        if (ok && !canvas->GetLayers().empty()) {
            auto& L = canvas->GetLayers().back();
            L.workSpace = texset::LayerWorkSpace{};
            L.workSpace.mapMask = 0;
            L.workSpace.SetMap(texset::MapKind::Diffuse, true);
            L.workSpace.channelWriteMask = 0xF;
        }
    } else {
        ok = canvas->ImportImageAsMapLayer(device, filepath, kind, layerName);
    }

    if (ok) {
        int w = canvas->GetWidth();
        int h = canvas->GetHeight();
        // Prefer native map size on slot when available (export / pack)
        if (!canvas->GetLayers().empty()) {
            const auto& L = canvas->GetLayers().back();
            if (L.nativeMapCache && L.nativeMapW > 0 && L.nativeMapH > 0 &&
                L.nativeMapKind == kind) {
                w = L.nativeMapW;
                h = L.nativeMapH;
            }
        }
        set->EnableMap(kind, w, h, filepath);
        (void)soloRole;
        canvas->SetDocumentModified(true);
        if (texset::TextureSet* s = textureSets.Active())
            canvas->SetActiveSetMaps(s->maps);
    }
    return ok;
}

bool Project::ApplyActiveSetTemplate(const std::string& templateId) {
    texset::TextureSet* set = textureSets.Active();
    if (!set) return false;
    texset::SetTemplate t = texset::Template_Default();
    if (templateId == "ZZZ" || templateId == "zzz") t = texset::Template_ZZZ();
    else if (templateId == "GI" || templateId == "gi") t = texset::Template_GI();
    set->ApplySetTemplate(t);
    SyncTextureSetsFromCanvas();
    return true;
}

static std::string ToLowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// Strip map kind tokens from filename stem → shared base "BelleHairA"
static std::string StripMapKindFromStem(std::string stem) {
    static const char* kTokens[] = {
        "materialmap", "_materialmap", "_material", "material",
        "lightmap", "_lightmap", "normalmap", "_normalmap", "_normal", "normal",
        "diffuse", "_diffuse", "albedo", "_albedo", "basecolor", "_basecolor",
        "_d", "_lm", "_n", "_mat"
    };
    std::string lower = ToLowerCopy(stem);
    for (const char* tok : kTokens) {
        size_t tlen = std::strlen(tok);
        if (lower.size() > tlen && lower.compare(lower.size() - tlen, tlen, tok) == 0) {
            // Avoid stripping single-letter from tiny stems
            if (tlen <= 2 && lower.size() < 6) continue;
            stem = stem.substr(0, stem.size() - tlen);
            break;
        }
    }
    // trim trailing _ -
    while (!stem.empty() && (stem.back() == '_' || stem.back() == '-'))
        stem.pop_back();
    return stem;
}

static texset::MapKind GuessMapKindFromFileName(const std::string& fname) {
    std::string lower = ToLowerCopy(fname);
    if (lower.find("lightmap") != std::string::npos || lower.find("_lm") != std::string::npos)
        return texset::MapKind::LightMap;
    if (lower.find("material") != std::string::npos || lower.find("_mat") != std::string::npos)
        return texset::MapKind::MaterialMap;
    if (lower.find("normal") != std::string::npos || lower.find("_nrm") != std::string::npos ||
        lower.find("_nml") != std::string::npos)
        return texset::MapKind::NormalMap;
    if (lower.find("glow") != std::string::npos || lower.find("emiss") != std::string::npos)
        return texset::MapKind::GlowMap;
    if (lower.find("diffuse") != std::string::npos || lower.find("albedo") != std::string::npos ||
        lower.find("basecolor") != std::string::npos)
        return texset::MapKind::Diffuse;
    return texset::MapKind::Diffuse;
}

int Project::SetupAdvancedFromBaseTexture(
    ID3D11Device* device,
    const std::string& baseDiffusePath,
    const std::string& templateId,
    const std::string& setName) {

    if (!device || !canvas || baseDiffusePath.empty()) {
        Logger::Get().ErrorTag("project", "SetupAdvancedFromBaseTexture: bad args");
        return 0;
    }

    const std::string basePath = PathUtil::NormalizeToUtf8Path(baseDiffusePath);
    fs::path baseFs = PathUtil::FromUtf8(basePath);
    if (!fs::exists(baseFs)) {
        Logger::Get().ErrorTag("project", "Base texture missing: " + basePath);
        return 0;
    }

    // 1) Project type BEFORE load (LoadImage no longer forces Simple)
    canvas->SetProjectType(Canvas::ProjectType::Advanced);

    // 2) Texture set library + template (ZZZ packing)
    textureSets.sets.clear();
    textureSets.activeSetId = -1;
    textureSets.nextId = 1;
    textureSets.EnsureSimpleDefault();
    ApplyActiveSetTemplate(templateId.empty() ? "ZZZ" : templateId);

    std::string stem = PathUtil::WideToUtf8(baseFs.stem().wstring());
    std::string group = StripMapKindFromStem(stem);
    if (texset::TextureSet* set = textureSets.Active()) {
        set->name = setName.empty() ? group : setName;
    }

    // 3) Drop blank starter layers so Diffuse sets document size from the file
    //    (never keep default 1024/4096 blank and UV-upsample maps into it).
    canvas->ClearAllLayersNoUndo();
    canvas->ClearUndoHistory();

    // 4) Load base Diffuse as first real layer (adopts native W×H)
    if (!ImportMapFile(texset::MapKind::Diffuse, basePath)) {
        if (!canvas->LoadImageToLayer(device, basePath)) {
            Logger::Get().ErrorTag("project", "Failed to load base Diffuse: " + basePath);
            return 0;
        }
        if (!canvas->GetLayers().empty()) {
            auto& L = canvas->GetLayers().back();
            L.workSpace.mapMask = 0;
            L.workSpace.SetMap(texset::MapKind::Diffuse, true);
        }
    }
    canvas->SetProjectType(Canvas::ProjectType::Advanced);
    canvas->ClearUndoHistory(); // don't keep setup deletes/creates in history
    SyncTextureSetsFromCanvas();

    int loaded = 1;
    Logger::Get().InfoTag("project",
        "Advanced base Diffuse loaded " + std::to_string(canvas->GetWidth()) + "x" +
        std::to_string(canvas->GetHeight()) + " stem=" + group +
        " layers=" + std::to_string(canvas->GetLayers().size()) +
        " (document size = Diffuse native)");

    // 5) Sibling maps in same folder matching group stem
    fs::path dir = baseFs.parent_path();
    std::error_code ec;
    struct Found { texset::MapKind kind; std::string path; };
    std::vector<Found> found;
    for (auto& ent : fs::directory_iterator(dir, ec)) {
        if (ec || !ent.is_regular_file()) continue;
        std::string fname = PathUtil::WideToUtf8(ent.path().filename().wstring());
        std::string fstem = PathUtil::WideToUtf8(ent.path().stem().wstring());
        std::string ext = ToLowerCopy(ent.path().extension().string());
        if (ext != ".dds" && ext != ".png" && ext != ".jpg" && ext != ".jpeg" &&
            ext != ".tga" && ext != ".bmp")
            continue;

        std::string fgroup = StripMapKindFromStem(fstem);
        // Match group (case-insensitive)
        if (ToLowerCopy(fgroup) != ToLowerCopy(group) &&
            ToLowerCopy(fstem).find(ToLowerCopy(group)) == std::string::npos)
            continue;

        texset::MapKind mk = GuessMapKindFromFileName(fname);
        std::string full = PathUtil::WideToUtf8(ent.path().wstring());
        // Skip the base diffuse itself
        if (ToLowerCopy(full) == ToLowerCopy(basePath)) continue;
        if (mk == texset::MapKind::Diffuse) continue; // don't replace paint stack
        found.push_back({ mk, full });
    }

    // Prefer one file per MapKind (prefer .dds over .png if both)
    auto score = [](const std::string& p) {
        std::string e = ToLowerCopy(p);
        if (e.size() >= 4 && e.substr(e.size() - 4) == ".dds") return 2;
        if (e.size() >= 4 && e.substr(e.size() - 4) == ".png") return 1;
        return 0;
    };
    std::unordered_map<int, Found> best;
    for (const auto& f : found) {
        int k = (int)f.kind;
        auto it = best.find(k);
        if (it == best.end() || score(f.path) > score(it->second.path))
            best[k] = f;
    }

    for (auto& kv : best) {
        const Found& f = kv.second;
        if (ImportMapFile(f.kind, f.path)) {
            ++loaded;
            Logger::Get().InfoTag("project",
                std::string("Imported ") + texset::MapKindName(f.kind) + " ← " + f.path);
        } else {
            Logger::Get().WarnTag("project",
                std::string("Failed map ") + texset::MapKindName(f.kind) + " ← " + f.path);
        }
    }

    // 6) Wire UI/view state
    if (texset::TextureSet* set = textureSets.Active()) {
        canvas->SetActiveSetMaps(set->maps);
        set->activeMap = texset::MapKind::Diffuse;
    }
    canvas->SetViewMapKind(texset::MapKind::Diffuse);
    canvas->SetDocumentModified(true);

    Logger::Get().InfoTag("project",
        "SetupAdvancedFromBaseTexture done maps_loaded=" + std::to_string(loaded) +
        " type=" + std::to_string((int)canvas->GetProjectType()));
    return loaded;
}

int Project::QuickExportAllMaps(const std::string& baseDirHint) {
    textureSets.EnsureSimpleDefault();
    texset::TextureSet* set = textureSets.Active();
    if (!set || !canvas) return 0;

    std::string baseDir = baseDirHint;
    if (baseDir.empty()) {
        std::string exp = canvas->GetExportPath();
        if (!exp.empty()) {
            try {
                baseDir = PathUtil::WideToUtf8(
                    fs::path(PathUtil::Utf8ToWide(exp)).parent_path().wstring());
            } catch (...) { baseDir.clear(); }
        }
        if (baseDir.empty()) {
            std::string proj = canvas->GetCurrentProjectFilePath();
            if (!proj.empty()) {
                try {
                    baseDir = PathUtil::WideToUtf8(
                        fs::path(PathUtil::Utf8ToWide(proj)).parent_path().wstring());
                } catch (...) { baseDir.clear(); }
            }
        }
        if (baseDir.empty()) baseDir = ".";
    }

    // Sync Diffuse size from canvas
    SyncTextureSetsFromCanvas();

    // Pack every enabled map from layers (workSpace-filtered) — no mapComposites
    std::unordered_map<int, texset::MapExportPixels> packed;
    for (const auto& m : set->maps) {
        if (!m.enabled) continue;
        texset::MapExportPixels px;
        // Use slot native size if set
        if (canvas->ComposePackedMapRGBA8(m.kind, set->maps, nullptr, px.rgba, px.w, px.h)) {
            // Respect explicit export size on slot
            if (m.width > 0 && m.height > 0 && (px.w != m.width || px.h != m.height)) {
                std::vector<uint8_t> resized((size_t)m.width * (size_t)m.height * 4u);
                for (int y = 0; y < m.height; ++y) {
                    int sy = std::min(px.h - 1, y * px.h / m.height);
                    for (int x = 0; x < m.width; ++x) {
                        int sx = std::min(px.w - 1, x * px.w / m.width);
                        size_t di = ((size_t)y * m.width + x) * 4;
                        size_t si = ((size_t)sy * px.w + sx) * 4;
                        resized[di+0]=px.rgba[si+0]; resized[di+1]=px.rgba[si+1];
                        resized[di+2]=px.rgba[si+2]; resized[di+3]=px.rgba[si+3];
                    }
                }
                px.rgba = std::move(resized);
                px.w = m.width; px.h = m.height;
            }
            Logger::Get().InfoTag("texset",
                std::string("Packed ") + texset::MapKindName(m.kind) + " " +
                std::to_string(px.w) + "x" + std::to_string(px.h));
            packed[(int)m.kind] = std::move(px);
        } else {
            Logger::Get().WarnTag("texset",
                std::string("Pack failed for ") + texset::MapKindName(m.kind));
        }
    }

    // Build batch options from canvas ExportContainer (hard DDS/PNG switch).
    texset::BatchExportOptions opts;
    const bool asDds =
        canvas->GetExportContainer() == Canvas::ExportContainer::DDS;
    opts.container = asDds
        ? texset::BatchExportOptions::Container::DDS
        : texset::BatchExportOptions::Container::PNG;
    opts.ddsFormat = canvas->GetExportFormat();
    opts.usePerMapCodec = true; // BC5 normals etc. from template still win when DDS
    opts.generateMipMaps = canvas->GetExportGenerateMipMaps();
    opts.mipFilter = canvas->GetExportMipFilter();
    opts.compressionSpeed = canvas->GetExportCompressionSpeed();
    if (!asDds) {
        auto preset = canvas->GetExportIccPreset();
        if (preset != Canvas::IccPreset::None) {
            const auto& icc = Canvas::GetIccPresetBytes(preset);
            opts.iccBytes = icc.data();
            opts.iccSize = icc.size();
            opts.iccProfileName = Canvas::IccPresetName(preset);
        }
    }

    // Align empty/stale map paths to the active container extension
    const char* wantExt = asDds ? "dds" : "png";
    for (auto& m : set->maps) {
        if (!m.enabled) continue;
        if (m.exportPath.empty()) {
            m.exportPath = texset::DefaultMapExportPath(*set, m.kind, baseDir, wantExt);
        } else {
            m.exportPath = texset::ForcePathExtension(m.exportPath, wantExt);
        }
    }

    std::string ext = wantExt;
    auto result = texset::ExportAllMaps(
        *set, baseDir, ext, nullptr, nullptr, 0, 0, &packed, &opts);

    Logger::Get().InfoTag("texset",
        std::string("BatchExport container=") + (asDds ? "DDS" : "PNG") +
        " maps written=" + std::to_string(result.written) +
        " failed=" + std::to_string(result.failed) +
        (result.log.empty() ? "" : ("\n" + result.log)));
    return result.written;
}

bool ProjectManager::Initialize(ID3D11Device* device) {
    m_Device = device;
    m_Projects.clear();
    m_ActiveId = -1;
    m_NextId = 1;
    m_NextUntitled = 1;

    if (!device) {
        Logger::Get().Error("ProjectManager::Initialize: null device");
        return false;
    }

    if (CreateEmptyProject() < 0) {
        Logger::Get().Error("ProjectManager::Initialize: failed to create first project");
        return false;
    }
    Logger::Get().Info("ProjectManager ready (single-process multi-project)");
    return true;
}

void ProjectManager::Shutdown() {
    for (auto& p : m_Projects) {
        if (p && p->canvas)
            p->canvas->Shutdown();
    }
    m_Projects.clear();
    m_ActiveId = -1;
    m_Device = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_PendingMutex);
        m_PendingPaths.clear();
    }
}

int ProjectManager::CreateEmptyProject() {
    if (!m_Device) return -1;

    auto proj = std::make_unique<Project>();
    proj->id = m_NextId++;
    proj->untitledIndex = m_NextUntitled++;
    proj->canvas = std::make_unique<Canvas>();
    if (!proj->canvas->Initialize(m_Device)) {
        Logger::Get().Error("ProjectManager: Canvas::Initialize failed for project " +
                            std::to_string(proj->id));
        return -1;
    }
    // Default blank canvas (not 0×0) so paint / fill work immediately
    {
        int dw = ConfigManager::Get().GetDefaultWidth();
        int dh = ConfigManager::Get().GetDefaultHeight();
        if (dw < 1) dw = 1024;
        if (dh < 1) dh = 1024;
        proj->canvas->ResizeCanvas(m_Device, dw, dh);
        proj->canvas->CreateNewLayer(m_Device, "Background");
        // Fresh blank is not "modified" yet
        proj->canvas->SetDocumentModified(false);
    }
    // Texture Set library (Simple = 1 Diffuse set)
    proj->textureSets.EnsureSimpleDefault();
    if (texset::TextureSet* s = proj->textureSets.Active()) {
        s->name = proj->GetTabTitle();
        s->EnableMap(texset::MapKind::Diffuse, proj->canvas->GetWidth(), proj->canvas->GetHeight());
    }
    proj->canvas->SetActiveSetMaps(
        proj->textureSets.Active() ? proj->textureSets.Active()->maps
                                   : std::vector<texset::MapSlot>{});

    const int id = proj->id;
    proj->lastActiveTime = std::chrono::steady_clock::now();
    m_Projects.push_back(std::move(proj));
    m_ActiveId = id;
    Logger::Get().Info("Project created id=" + std::to_string(id) + " blank canvas ready");
    return id;
}

int ProjectManager::PrepareOpenAsNewProject(const std::string& filepath) {
    const std::string path = PathUtil::NormalizeToUtf8Path(filepath);

    // Reuse single blank starter instead of stacking empty tabs.
    if (Project* active = ActiveProject()) {
        if (active->IsBlank() && m_Projects.size() == 1) {
            m_ActiveId = active->id;
            Logger::Get().Info("Reusing blank project id=" + std::to_string(active->id) +
                               " for open: " + path);
            return active->id;
        }
    }

    const int id = CreateEmptyProject();
    if (id < 0) return -1;
    Logger::Get().Info("Prepared new project id=" + std::to_string(id) + " for: " + path);
    return id;
}

int ProjectManager::ActivateOrPrepareOpen(const std::string& filepath) {
    const std::string path = PathUtil::NormalizeToUtf8Path(filepath);
    if (path.empty()) return PrepareOpenAsNewProject(path);

    // Prefer absolute-normalized compare for "already open"
    std::string pathKey = path;
    try {
        pathKey = PathUtil::WideToUtf8(fs::absolute(PathUtil::Utf8ToWide(path)).wstring());
    } catch (...) {}

    for (auto& p : m_Projects) {
        if (!p || !p->canvas) continue;
        std::string existing = p->canvas->GetCurrentProjectFilePath();
        if (existing.empty()) continue;
        std::string existingKey = PathUtil::NormalizeToUtf8Path(existing);
        try {
            existingKey = PathUtil::WideToUtf8(
                fs::absolute(PathUtil::Utf8ToWide(existingKey)).wstring());
        } catch (...) {}
        // Case-insensitive on Windows
        auto lower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            return s;
        };
        if (lower(existingKey) == lower(pathKey)) {
            SwitchTo(p->id);
            Logger::Get().Info("Activate existing project id=" + std::to_string(p->id) +
                               " path=" + path);
            return p->id;
        }
    }

    return PrepareOpenAsNewProject(path);
}

bool ProjectManager::WakeProject(Project& p, ID3D11Device* device) {
    if (!p.canvas || !device) return false;
    p.restoringGpu = true;
    bool did = false;
    if (p.diskHibernated || (p.canvas->IsDiskHibernated() && !p.dormantScratchPath.empty())) {
        const std::string snap = p.dormantScratchPath;
        if (snap.empty() || !p.canvas->RestoreFromHibernateFile(device, snap)) {
            Logger::Get().ErrorTag("mem",
                "Disk hibernate RESTORE failed id=" + std::to_string(p.id) + " path=" + snap);
            p.restoringGpu = false;
            return false;
        }
        if (!p.pathBeforeHibernate.empty())
            p.canvas->SetCurrentProjectFilePath(p.pathBeforeHibernate);
        p.canvas->SetDocumentModified(p.dirtyBeforeHibernate);
        p.ApplyTextureSetsFromCanvas();
        if (p.ownsDormantScratch && !p.dormantScratchPath.empty() &&
            p.dormantScratchPath != p.pathBeforeHibernate) {
            try { std::filesystem::remove(PathUtil::FromUtf8(p.dormantScratchPath)); }
            catch (...) {}
        }
        p.diskHibernated = false;
        p.dormantScratchPath.clear();
        p.ownsDormantScratch = false;
        p.pathBeforeHibernate.clear();
        p.dirtyBeforeHibernate = false;
        did = true;
        m_ConsumeRestoring = true;
    } else if (p.canvas->IsGpuSuspended()) {
        p.canvas->EnsureGpuAwake(device);
        did = true;
        m_ConsumeRestoring = true;
    }
    p.restoringGpu = false;
    return did;
}

bool ProjectManager::TryDiskHibernate(Project& p) {
    if (!p.canvas || p.diskHibernated || p.canvas->IsDiskHibernated()) return false;
    // Skip empty blanks
    if (p.IsBlank()) return false;

    // Prefer already GPU-slept; else sleep first
    if (!p.canvas->IsGpuSuspended())
        p.canvas->SuspendGpuResources();

    p.pathBeforeHibernate = p.canvas->GetCurrentProjectFilePath();
    p.dirtyBeforeHibernate = p.canvas->IsDocumentModified();

    std::string snapPath;
    bool owns = false;
    const std::string& cur = p.pathBeforeHibernate;
    auto endsWithRayp = [](const std::string& s) {
        if (s.size() < 5) return false;
        std::string e = s.substr(s.size() - 5);
        for (char& c : e) c = (char)std::tolower((unsigned char)c);
        return e == ".rayp";
    };

    // Clean .rayp on disk and not dirty → reload from original, no extra write
    if (!p.dirtyBeforeHibernate && endsWithRayp(cur)) {
        snapPath = cur;
        owns = false;
    } else {
        // Snapshot to dormant/ (works for dirty, DDS-only opens, untitled)
        std::string dir = ConfigManager::GetUserSubdirectory("dormant");
        try {
            std::filesystem::create_directories(PathUtil::FromUtf8(dir));
        } catch (...) {}
        snapPath = dir + "/proj_" + std::to_string(p.id) + ".rayp";
        p.InjectTextureSetsIntoCanvas();
        if (!p.canvas->SaveCanvasRayp(snapPath)) {
            Logger::Get().ErrorTag("mem",
                "Disk hibernate save failed id=" + std::to_string(p.id));
            return false;
        }
        // SaveCanvasRayp updates current path to scratch — keep user-facing path/title.
        if (!p.pathBeforeHibernate.empty())
            p.canvas->SetCurrentProjectFilePath(p.pathBeforeHibernate);
        else
            p.canvas->SetCurrentProjectFilePath({});
        p.canvas->SetDocumentModified(p.dirtyBeforeHibernate);
        owns = true;
    }

    p.canvas->StripHeavyMemoryAfterHibernate();
    p.dormantScratchPath = snapPath;
    p.ownsDormantScratch = owns;
    p.diskHibernated = true;
    Logger::Get().InfoTag("mem",
        "Disk hibernate: project id=" + std::to_string(p.id) +
        " \"" + p.GetTabTitle() + "\" → " + snapPath +
        (owns ? " [scratch]" : " [original rayp]"));
    return true;
}

bool ProjectManager::SwitchTo(int id) {
    if (!FindMutable(id)) return false;
    if (m_ActiveId == id) return true;
    const auto now = std::chrono::steady_clock::now();
    if (Project* old = FindMutable(m_ActiveId))
        old->lastActiveTime = now;

    m_ActiveId = id;
    if (Project* p = FindMutable(id)) {
        p->lastActiveTime = now;
        if (m_Device)
            WakeProject(*p, m_Device);
    }
    Logger::Get().Debug("Switched to project id=" + std::to_string(id));
    return true;
}

int ProjectManager::TickDormancy(ID3D11Device* device) {
    if (!device || m_Projects.empty()) return 0;
    const auto now = std::chrono::steady_clock::now();

    // Under memory pressure, sleep inactive tabs much sooner (boss-fight / many 4K).
    int idleSec = m_GpuDormancyIdleSec;
    bool pressure = false;
    bool extreme = false;
    {
        auto mem = MemoryStats::QueryProcess();
        if (mem.totalPhysBytes > 0) {
            const double usedFrac = (double)mem.workingSetBytes / (double)mem.totalPhysBytes;
            const double availFrac = (double)mem.availPhysBytes / (double)mem.totalPhysBytes;
            if (usedFrac > 0.35 || availFrac < 0.20 || m_Projects.size() >= 6) {
                pressure = true;
                idleSec = std::min(idleSec, 8);
            }
            if (usedFrac > 0.50 || availFrac < 0.12 || m_Projects.size() >= 12) {
                idleSec = 3;
                extreme = true;
            }
            if (usedFrac > 0.60 || availFrac < 0.08 || m_Projects.size() >= 16)
                extreme = true;
        }
    }
    const auto idleLimit = std::chrono::seconds(std::max(3, idleSec));
    int changed = 0;

    // Keep active tab awake and timestamped
    if (Project* active = ActiveProject()) {
        active->lastActiveTime = now;
        WakeProject(*active, device);
    }

    for (auto& p : m_Projects) {
        if (!p || !p->canvas || p->id == m_ActiveId) continue;
        if (p->IsBlank() && m_Projects.size() <= 2) continue;
        if (p->lastActiveTime.time_since_epoch().count() == 0) {
            p->lastActiveTime = now;
            continue;
        }
        if (now - p->lastActiveTime < idleLimit) continue;

        // L2: already GPU-slept + extreme pressure → disk hibernate
        // At most ONE L2 per tick (SaveCanvasRayp can take seconds on 4K+).
        if (extreme && (p->canvas->IsGpuSuspended() || p->diskHibernated) && !p->diskHibernated) {
            if (TryDiskHibernate(*p)) {
                ++changed;
                return changed; // yield to main loop before next hibernate
            }
        }
        if (p->canvas->IsGpuSuspended() || p->diskHibernated) {
            // Still free undo COW under pressure while L1 (CPU tiles stay).
            if (pressure && !p->diskHibernated)
                p->canvas->TrimUndoHistoryForPressure(extreme);
            continue;
        }

        p->canvas->SuspendGpuResources();
        if (pressure)
            p->canvas->TrimUndoHistoryForPressure(extreme);
        ++changed;
        Logger::Get().InfoTag("mem",
            std::string("GPU dormancy: project id=") + std::to_string(p->id) +
            " \"" + p->GetTabTitle() + "\" slept after idle" +
            (pressure ? " [pressure]" : "") +
            " (CPU tiles kept, FX caches dropped)");
    }

    // Second pass under extreme pressure: hibernate GPU-slept tabs even if just slept
    // Cap at 1 L2 save per frame so UI stays responsive.
    if (extreme) {
        for (auto& p : m_Projects) {
            if (!p || !p->canvas || p->id == m_ActiveId) continue;
            if (p->diskHibernated) continue;
            if (!p->canvas->IsGpuSuspended()) continue;
            if (now - p->lastActiveTime < idleLimit) continue;
            if (TryDiskHibernate(*p)) {
                ++changed;
                break;
            }
        }
    }
    return changed;
}

int ProjectManager::SuspendInactiveNow() {
    int n = 0;
    for (auto& p : m_Projects) {
        if (!p || !p->canvas || p->id == m_ActiveId) continue;
        if (p->diskHibernated || p->canvas->IsDiskHibernated()) continue;
        if (!p->canvas->IsGpuSuspended()) {
            p->canvas->SuspendGpuResources();
            ++n;
        }
        p->canvas->TrimUndoHistoryForPressure(true);
    }
    if (n > 0)
        Logger::Get().InfoTag("mem",
            "GPU dormancy: SuspendInactiveNow slept " + std::to_string(n) + " tab(s)");
    return n;
}

int ProjectManager::HibernateInactiveNow(int maxCount) {
    if (maxCount < 1) maxCount = 1;
    int n = 0;
    for (auto& p : m_Projects) {
        if (n >= maxCount) break;
        if (!p || !p->canvas || p->id == m_ActiveId) continue;
        if (p->diskHibernated) continue;
        if (TryDiskHibernate(*p))
            ++n;
    }
    if (n > 0)
        Logger::Get().InfoTag("mem",
            "Disk hibernate: HibernateInactiveNow " + std::to_string(n) + " tab(s)");
    return n;
}

void ProjectManager::FlushAllDeferredGpuReleases() {
    for (auto& p : m_Projects) {
        if (p && p->canvas)
            p->canvas->FlushDeferredGpuReleases();
    }
}

bool ProjectManager::ConsumeRestoringFlag() {
    if (!m_ConsumeRestoring) return false;
    m_ConsumeRestoring = false;
    return true;
}

bool ProjectManager::CloseProject(int id, bool force) {
    Project* p = FindMutable(id);
    if (!p) return false;

    // Hibernated tabs may have stripped canvas dirty flag — trust snapshot meta.
    const bool dirty = p->diskHibernated
        ? p->dirtyBeforeHibernate
        : (p->canvas && p->canvas->IsDocumentModified());
    if (!force && dirty) {
        return false; // UI must confirm
    }

    // Drop owned L2 scratch so dormant/ does not accumulate closed tabs.
    if (p->ownsDormantScratch && !p->dormantScratchPath.empty() &&
        p->dormantScratchPath != p->pathBeforeHibernate) {
        try { std::filesystem::remove(PathUtil::FromUtf8(p->dormantScratchPath)); }
        catch (...) {}
        p->ownsDormantScratch = false;
        p->dormantScratchPath.clear();
    }

    if (p->canvas)
        p->canvas->Shutdown();

    const int idx = IndexOf(id);
    if (idx < 0) return false;
    m_Projects.erase(m_Projects.begin() + idx);

    if (m_Projects.empty()) {
        CreateEmptyProject();
        return true;
    }

    if (m_ActiveId == id) {
        // Prefer neighbor tab and wake it (may be L1/L2 dormant).
        const int newIdx = std::min(idx, (int)m_Projects.size() - 1);
        m_ActiveId = m_Projects[newIdx]->id;
        if (Project* next = FindMutable(m_ActiveId)) {
            next->lastActiveTime = std::chrono::steady_clock::now();
            if (m_Device)
                WakeProject(*next, m_Device);
        }
    }
    Logger::Get().Info("Closed project id=" + std::to_string(id) +
                       " active=" + std::to_string(m_ActiveId));
    return true;
}

Canvas& ProjectManager::ActiveCanvas() {
    Project* p = ActiveProject();
    if (!p || !p->canvas) {
        // Should never happen after Initialize — recover.
        if (CreateEmptyProject() < 0)
            throw std::runtime_error("ProjectManager: no active canvas");
        p = ActiveProject();
    }
    return *p->canvas;
}

Canvas* ProjectManager::ActiveCanvasPtr() {
    Project* p = ActiveProject();
    return (p && p->canvas) ? p->canvas.get() : nullptr;
}

const Canvas& ProjectManager::ActiveCanvas() const {
    const Project* p = ActiveProject();
    if (!p || !p->canvas)
        throw std::runtime_error("ProjectManager: no active canvas");
    return *p->canvas;
}

Project* ProjectManager::FindProject(int id) { return FindMutable(id); }

const Project* ProjectManager::FindProject(int id) const {
    for (const auto& p : m_Projects)
        if (p && p->id == id) return p.get();
    return nullptr;
}

Project* ProjectManager::ActiveProject() { return FindMutable(m_ActiveId); }

const Project* ProjectManager::ActiveProject() const {
    for (const auto& p : m_Projects)
        if (p && p->id == m_ActiveId) return p.get();
    return nullptr;
}

std::vector<ProjectManager::ProjectTabInfo> ProjectManager::ListTabs() const {
    std::vector<ProjectTabInfo> out;
    out.reserve(m_Projects.size());
    for (const auto& p : m_Projects) {
        if (!p) continue;
        ProjectTabInfo t;
        t.id = p->id;
        t.title = p->GetTabTitle();
        // While L2-hibernated, canvas dirty bit may not reflect pre-hibernate state.
        t.dirty = p->diskHibernated
            ? p->dirtyBeforeHibernate
            : (p->canvas && p->canvas->IsDocumentModified());
        t.active = (p->id == m_ActiveId);
        t.gpuSuspended = p->canvas && p->canvas->IsGpuSuspended() && !p->diskHibernated;
        t.diskHibernated = p->diskHibernated || (p->canvas && p->canvas->IsDiskHibernated());
        t.restoring = p->restoringGpu;
        out.push_back(std::move(t));
    }
    return out;
}

void ProjectManager::EnqueueOpenPath(const std::string& path) {
    std::string n = PathUtil::NormalizeToUtf8Path(path);
    if (n.empty()) return;
    std::lock_guard<std::mutex> lock(m_PendingMutex);
    m_PendingPaths.push_back(std::move(n));
}

void ProjectManager::DrainPendingOpens(
    const std::function<void(const std::string& path, Canvas& canvas)>& openFn) {
    std::deque<std::string> local;
    {
        std::lock_guard<std::mutex> lock(m_PendingMutex);
        local.swap(m_PendingPaths);
    }
    for (const auto& path : local) {
        const int id = ActivateOrPrepareOpen(path);
        if (id < 0) continue;
        // If already open with content, ActivateOrPrepareOpen switched — skip reload.
        Project* p = FindMutable(id);
        if (!p || !p->canvas) continue;
        const std::string existing = p->canvas->GetCurrentProjectFilePath();
        if (!existing.empty()) {
            std::string a = PathUtil::NormalizeToUtf8Path(existing);
            std::string b = PathUtil::NormalizeToUtf8Path(path);
            auto lower = [](std::string s) {
                std::transform(s.begin(), s.end(), s.begin(),
                               [](unsigned char c) { return (char)std::tolower(c); });
                return s;
            };
            if (lower(a) == lower(b) && !p->IsBlank()) {
                // Already loaded this file
                continue;
            }
        }
        openFn(path, *p->canvas);
    }
}

Project* ProjectManager::FindMutable(int id) {
    for (auto& p : m_Projects)
        if (p && p->id == id) return p.get();
    return nullptr;
}

int ProjectManager::IndexOf(int id) const {
    for (size_t i = 0; i < m_Projects.size(); ++i)
        if (m_Projects[i] && m_Projects[i]->id == id) return (int)i;
    return -1;
}
