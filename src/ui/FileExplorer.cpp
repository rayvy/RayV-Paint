#include "FileExplorer.h"
#include "../core/ProjectManager.h"
#include "../core/Logger.h"
#include "../core/PathUtil.h"
#include "../texset/TextureSetIO.h"
#include "widgets/UiTooltip.h"

#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <cstdio>

namespace fs = std::filesystem;
namespace UI {

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

static void ListDirectory(const std::string& dir, std::vector<std::string>& outDirs,
                          std::vector<std::string>& outFiles) {
    outDirs.clear();
    outFiles.clear();
    std::error_code ec;
    fs::path p = PathUtil::FromUtf8(dir);
    if (!fs::exists(p, ec) || !fs::is_directory(p, ec)) return;
    for (auto& ent : fs::directory_iterator(p, ec)) {
        if (ec) break;
        std::string name = PathUtil::WideToUtf8(ent.path().filename().wstring());
        if (name.empty() || name[0] == '.') continue;
        if (ent.is_directory()) outDirs.push_back(name);
        else outFiles.push_back(name);
    }
    std::sort(outDirs.begin(), outDirs.end());
    std::sort(outFiles.begin(), outFiles.end());
}

void FileExplorerOpen(FileExplorerState& st, FileExplorerMode mode, const std::string& startDir) {
    st.open = true;
    st.mode = mode;
    st.status.clear();
    st.selectedPath.clear();
    st.multiSelect.clear();
    if (!startDir.empty())
        st.currentDir = startDir;
    else if (st.currentDir.empty())
        st.currentDir = DefaultStartDir();
    // Sensible defaults for Advanced create
    if (mode == FileExplorerMode::ProjectCreate && st.projectType == 1 && st.templateIdx == 0)
        st.templateIdx = 1; // ZZZ
}

void FileExplorerClose(FileExplorerState& st) {
    st.open = false;
}

// ---- Project create ----
bool FileExplorerApplyProjectCreate(FileExplorerState& st, ID3D11Device* device) {
    if (!device) {
        st.status = "No GPU device";
        return false;
    }
    auto& pm = ProjectManager::Get();

    // Reuse blank tab if possible, else new
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

    // ---- Simple ----
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
        Logger::Get().InfoTag("project", "Created Simple project");
        return true;
    }

    // ---- Advanced Mod: type only for now (INI later) ----
    if (st.projectType == 2) {
        canvas.SetProjectType(Canvas::ProjectType::AdvancedModMode);
        proj->ApplyActiveSetTemplate(templates[ti]);
        if (texset::TextureSet* s = proj->textureSets.Active())
            s->name = st.projectName;
        st.status = "Advanced Mod Mode project (bind INI in Mod Setup)";
        return true;
    }

    // ---- Advanced multi-map ----
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

    // Optional: if auto-pull disabled, strip non-diffuse maps after base load
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
    if (!project || st.selectedPath.empty()) return false;
    const std::string path = st.selectedPath;

    if (st.importRemapMode) {
        texset::TextureSet* set = project->textureSets.Active();
        if (!set) return false;
        if (!texset::ImportMapFromFile(*set, st.remapDestMap, path, texset::ChannelRole::None))
            return false;
        if (texset::MapSlot* slot = set->GetMap(st.remapDestMap)) {
            for (int i = 0; i < 4; ++i) {
                if (st.remap[i].enabled)
                    slot->pack[(int)st.remap[i].srcChannel].role = st.remap[i].dstRole;
            }
            slot->enabled = true;
        }
        canvas.SetDocumentModified(true);
        canvas.SetActiveSetMaps(set->maps);
        st.status = "Imported + remapped";
        return true;
    }

    if (st.importMapKind == texset::MapKind::Diffuse && device) {
        bool ok = canvas.LoadImageToLayer(device, path);
        // Preserve Advanced if already set
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
    bool ok = project->ImportMapFile(st.importMapKind, path, solo);
    if (ok && canvas.GetProjectType() == Canvas::ProjectType::Simple)
        canvas.SetProjectType(Canvas::ProjectType::Advanced);
    if (texset::TextureSet* set = project->textureSets.Active())
        canvas.SetActiveSetMaps(set->maps);
    st.status = ok ? "Imported map" : "Import failed";
    return ok;
}

bool FileExplorerApplyExportTemplate(FileExplorerState& st, Project* project) {
    if (!project || !st.exportRoot[0]) return false;
    std::string root = st.exportRoot;
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
    if (project->canvas)
        project->canvas->SetDocumentModified(true);
    st.status = "Export paths assigned";
    return true;
}

// Collect image files in dir for base combo
static void CollectImages(const std::string& dir, std::vector<std::string>& outNames) {
    outNames.clear();
    std::error_code ec;
    fs::path p = PathUtil::FromUtf8(dir);
    if (!fs::is_directory(p, ec)) return;
    for (auto& ent : fs::directory_iterator(p, ec)) {
        if (!ent.is_regular_file()) continue;
        if (!IsImageExt(ent.path().extension().string())) continue;
        outNames.push_back(PathUtil::WideToUtf8(ent.path().filename().wstring()));
    }
    std::sort(outNames.begin(), outNames.end());
}

bool DrawFileExplorer(FileExplorerState& st, Project* project, Canvas& canvas,
                      ID3D11Device* device) {
    if (!st.open) return false;

    const char* title = "Files";
    switch (st.mode) {
    case FileExplorerMode::ProjectCreate: title = "New Project"; break;
    case FileExplorerMode::ImportTexture: title = "Import Texture"; break;
    case FileExplorerMode::ExportTemplate: title = "Export Template"; break;
    case FileExplorerMode::PickFolder: title = "Select Folder"; break;
    default: break;
    }

    bool confirmed = false;
    ImGui::SetNextWindowSize(ImVec2(780, 540), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(560, 400), ImVec2(2000, 2000));
    if (!ImGui::Begin(title, &st.open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return false;
    }

    const bool isCreate = (st.mode == FileExplorerMode::ProjectCreate);
    const float leftW = isCreate ? ImGui::GetContentRegionAvail().x * 0.48f : -1.f;

    // ================= LEFT: browser =================
    ImGui::BeginChild("##browser", ImVec2(isCreate ? leftW : 0, isCreate ? -48.f : -48.f), true);

    // Path bar
    {
        char dirBuf[512];
        std::snprintf(dirBuf, sizeof(dirBuf), "%s", st.currentDir.c_str());
        ImGui::SetNextItemWidth(-90.f);
        if (ImGui::InputText("##dir", dirBuf, sizeof(dirBuf), ImGuiInputTextFlags_EnterReturnsTrue))
            st.currentDir = dirBuf;
        ImGui::SameLine();
        if (ImGui::Button("Up")) {
            try {
                fs::path p = PathUtil::FromUtf8(st.currentDir);
                if (p.has_parent_path())
                    st.currentDir = PathUtil::WideToUtf8(p.parent_path().wstring());
            } catch (...) {}
        }
    }

    std::vector<std::string> dirs, files;
    ListDirectory(st.currentDir, dirs, files);

    ImGui::BeginChild("##list", ImVec2(0, 0), false);
    for (const auto& d : dirs) {
        ImGui::PushID(("d" + d).c_str());
        if (ImGui::Selectable(("  " + d).c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
            if (ImGui::IsMouseDoubleClicked(0)) {
                st.currentDir = JoinPath(st.currentDir, d);
                std::snprintf(st.importFolder, sizeof(st.importFolder), "%s", st.currentDir.c_str());
            }
        }
        ImGui::PopID();
    }
    for (const auto& f : files) {
        bool isImg = false;
        try { isImg = IsImageExt(fs::path(f).extension().string()); } catch (...) {}
        if ((st.mode == FileExplorerMode::ImportTexture || isCreate) && !isImg) continue;

        std::string full = JoinPath(st.currentDir, f);
        bool sel = (st.selectedPath == full) ||
                   (isCreate && st.baseDiffusePath[0] &&
                    ToLower(st.baseDiffusePath) == ToLower(full));

        ImGui::PushID(("f" + f).c_str());
        if (ImGui::Selectable(f.c_str(), sel, ImGuiSelectableFlags_AllowDoubleClick)) {
            st.selectedPath = full;
            if (isCreate && isImg) {
                // Single click = base Diffuse candidate
                std::snprintf(st.baseDiffusePath, sizeof(st.baseDiffusePath), "%s", full.c_str());
                std::snprintf(st.importFolder, sizeof(st.importFolder), "%s", st.currentDir.c_str());
                // Suggest project name from file
                try {
                    std::string stem = PathUtil::WideToUtf8(fs::path(PathUtil::Utf8ToWide(f)).stem().wstring());
                    // crude strip
                    auto low = ToLower(stem);
                    for (const char* t : {"diffuse","lightmap","materialmap","normalmap","_diffuse","_lightmap"}) {
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
            if (st.mode == FileExplorerMode::ExportTemplate) {
                // selecting a folder path already handled; file's parent as root
                std::snprintf(st.exportRoot, sizeof(st.exportRoot), "%s", st.currentDir.c_str());
            }
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) &&
            st.mode == FileExplorerMode::ImportTexture) {
            st.selectedPath = full;
            confirmed = FileExplorerApplyImport(st, project, canvas, device);
            if (confirmed) st.open = false;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::EndChild();

    // ================= RIGHT: create form =================
    if (isCreate) {
        ImGui::SameLine();
        ImGui::BeginChild("##form", ImVec2(0, -48.f), true);

        ImGui::TextUnformatted("New Project");
        ImGui::Separator();

        ImGui::InputText("Name", st.projectName, sizeof(st.projectName));

        const char* types[] = { "Simple", "Advanced", "Advanced Mod Mode" };
        ImGui::Combo("Mode", &st.projectType, types, 3);

        if (st.projectType == 1) {
            const char* temps[] = { "Default", "ZZZ", "GI" };
            ImGui::Combo("Template", &st.templateIdx, temps, 3);
            if (ImGui::IsItemHovered())
                Ui::Tooltip("Channel packing defaults for the texture set");

            ImGui::Spacing();
            ImGui::TextUnformatted("Base Diffuse");
            ImGui::TextWrapped("%s", st.baseDiffusePath[0] ? st.baseDiffusePath : "(click a texture on the left)");
            ImGui::Checkbox("Auto-pull sibling maps", &st.autoPullSiblingMaps);
            if (ImGui::IsItemHovered())
                Ui::Tooltip("LightMap / MaterialMap / NormalMap with the same name stem");

            // Preview detected siblings
            if (st.baseDiffusePath[0] && st.autoPullSiblingMaps) {
                ImGui::Spacing();
                ImGui::TextDisabled("Will import:");
                ImGui::BulletText("Diffuse ← base");
                // Show other images in folder that share stem keywords
                std::vector<std::string> imgs;
                CollectImages(st.currentDir, imgs);
                std::string baseLow = ToLower(st.baseDiffusePath);
                for (const auto& fn : imgs) {
                    std::string full = JoinPath(st.currentDir, fn);
                    if (ToLower(full) == baseLow) continue;
                    std::string fl = ToLower(fn);
                    const char* tag = nullptr;
                    if (fl.find("light") != std::string::npos) tag = "LightMap";
                    else if (fl.find("material") != std::string::npos) tag = "MaterialMap";
                    else if (fl.find("normal") != std::string::npos) tag = "NormalMap";
                    if (tag)
                        ImGui::BulletText("%s ← %s", tag, fn.c_str());
                }
            }
        } else if (st.projectType == 0) {
            ImGui::TextDisabled("Optional start image — click left to set");
            if (st.baseDiffusePath[0])
                ImGui::TextWrapped("%s", st.baseDiffusePath);
        } else {
            ImGui::TextDisabled("Advanced Mod: bind INI later in Mod Setup");
        }

        ImGui::EndChild();
    } else if (st.mode == FileExplorerMode::ImportTexture) {
        ImGui::SameLine();
        ImGui::BeginChild("##impform", ImVec2(0, -48.f), true);
        ImGui::TextUnformatted("Import");
        const char* kinds[] = {
            "Diffuse", "LightMap", "MaterialMap", "NormalMap",
            "ExtraMap", "GlowMap", "WengineFX"
        };
        int ki = (int)st.importMapKind;
        if (ImGui::Combo("As map", &ki, kinds, 7))
            st.importMapKind = (texset::MapKind)ki;
        ImGui::Checkbox("Remap channels", &st.importRemapMode);
        if (st.importRemapMode) {
            int di = (int)st.remapDestMap;
            if (ImGui::Combo("Into map", &di, kinds, 7))
                st.remapDestMap = (texset::MapKind)di;
            const char* chn[] = { "R", "G", "B", "A" };
            for (int i = 0; i < 4; ++i) {
                ImGui::PushID(i);
                ImGui::Checkbox(chn[i], &st.remap[i].enabled);
                ImGui::SameLine();
                int ri = texset::ChannelRoleToComboIndex(st.remap[i].dstRole);
                ImGui::SetNextItemWidth(-1);
                if (ImGui::Combo("##rr", &ri, texset::ChannelRoleComboNames(),
                                 texset::ChannelRoleComboCount()))
                    st.remap[i].dstRole = texset::ChannelRoleFromComboIndex(ri);
                st.remap[i].srcChannel = (texset::Chan)i;
                ImGui::PopID();
            }
        }
        if (!st.selectedPath.empty())
            ImGui::TextWrapped("%s", st.selectedPath.c_str());
        ImGui::EndChild();
    } else if (st.mode == FileExplorerMode::ExportTemplate) {
        ImGui::SameLine();
        ImGui::BeginChild("##expform", ImVec2(0, -48.f), true);
        ImGui::TextUnformatted("Export root");
        ImGui::InputText("##root", st.exportRoot, sizeof(st.exportRoot));
        ImGui::InputText("Pattern", st.namePattern, sizeof(st.namePattern));
        ImGui::Checkbox("All sets", &st.exportAllSets);
        ImGui::EndChild();
    }

    // Footer
    if (!st.status.empty()) {
        ImGui::TextColored(ImVec4(0.6f, 0.85f, 0.6f, 1.f), "%s", st.status.c_str());
        ImGui::SameLine();
    }

    float footerRight = 200.f;
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - footerRight);
    if (ImGui::Button("Cancel", ImVec2(90, 0)))
        FileExplorerClose(st);
    ImGui::SameLine();

    bool canOk = true;
    const char* okLabel = "OK";
    if (st.mode == FileExplorerMode::ProjectCreate) {
        okLabel = "Create";
        if (st.projectType == 1 && !st.baseDiffusePath[0])
            canOk = false;
    } else if (st.mode == FileExplorerMode::ImportTexture) {
        okLabel = "Import";
        canOk = !st.selectedPath.empty();
    } else if (st.mode == FileExplorerMode::ExportTemplate) {
        okLabel = "Apply";
        canOk = st.exportRoot[0] != 0;
    }

    if (!canOk) ImGui::BeginDisabled();
    if (ImGui::Button(okLabel, ImVec2(90, 0))) {
        switch (st.mode) {
        case FileExplorerMode::ProjectCreate:
            confirmed = FileExplorerApplyProjectCreate(st, device);
            break;
        case FileExplorerMode::ImportTexture:
            confirmed = FileExplorerApplyImport(st, project, canvas, device);
            break;
        case FileExplorerMode::ExportTemplate:
            confirmed = FileExplorerApplyExportTemplate(st, project);
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
