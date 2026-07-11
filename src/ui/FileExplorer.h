#pragma once
// Context-aware File Explorer (browse / project create / import remap / export template).

#include "../texset/TextureSetTypes.h"
#include <d3d11.h>
#include <string>
#include <vector>

class Canvas;
struct Project;

namespace UI {

enum class FileExplorerMode : uint8_t {
    Browse = 0,
    ProjectCreate,   // new project wizard + base texture + auto sibling maps
    ImportTexture,   // pick file + map kind + remap
    ExportTemplate,  // export root + patterns
    PickFolder
};

struct ImportRemapEntry {
    texset::Chan srcChannel = texset::Chan::R;
    texset::ChannelRole dstRole = texset::ChannelRole::None;
    bool enabled = true;
};

struct FileExplorerState {
    bool open = false;
    FileExplorerMode mode = FileExplorerMode::Browse;

    std::string currentDir;
    std::string selectedPath;
    std::vector<std::string> multiSelect;

    // ---- ProjectCreate ----
    int projectType = 1;            // 0 Simple, 1 Advanced, 2 AdvancedMod
    int templateIdx = 1;            // default ZZZ for Advanced
    int canvasW = 1024;
    int canvasH = 1024;
    char projectName[128] = "New Project";
    // Base Diffuse texture (full path) — required for Advanced multi-map
    char baseDiffusePath[512] = "";
    char importFolder[512] = "";
    bool autoPullSiblingMaps = true;

    // ---- ImportTexture ----
    texset::MapKind importMapKind = texset::MapKind::Diffuse;
    bool importExtractSolo = false;
    texset::ChannelRole importSoloRole = texset::ChannelRole::None;
    bool importRemapMode = false;
    ImportRemapEntry remap[4] = {
        { texset::Chan::R, texset::ChannelRole::ShadowRamp, true },
        { texset::Chan::G, texset::ChannelRole::Metallic, true },
        { texset::Chan::B, texset::ChannelRole::Glossiness, true },
        { texset::Chan::A, texset::ChannelRole::None, false },
    };
    texset::MapKind remapDestMap = texset::MapKind::LightMap;

    // ---- ExportTemplate ----
    char exportRoot[512] = "";
    char namePattern[128] = "{set}{suffix}";
    bool exportAllSets = true;

    std::string status;
};

void FileExplorerOpen(FileExplorerState& st, FileExplorerMode mode, const std::string& startDir = {});
void FileExplorerClose(FileExplorerState& st);

bool DrawFileExplorer(FileExplorerState& st, Project* project, Canvas& canvas,
                      ID3D11Device* device);

bool FileExplorerApplyProjectCreate(FileExplorerState& st, ID3D11Device* device);
bool FileExplorerApplyImport(FileExplorerState& st, Project* project, Canvas& canvas,
                             ID3D11Device* device);
bool FileExplorerApplyExportTemplate(FileExplorerState& st, Project* project);

} // namespace UI
