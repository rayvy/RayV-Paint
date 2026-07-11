#pragma once
// Context-aware File Explorer — bookmarks, drives, icon views, previews, multi-import, folder export.

#include "../texset/TextureSetTypes.h"
#include <d3d11.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class Canvas;
struct Project;

namespace UI {

enum class FileExplorerMode : uint8_t {
    Browse = 0,
    ProjectCreate,   // new project wizard + base texture + auto sibling maps
    ImportTexture,   // single / multi map import + optional remap
    ExportTemplate,  // pick export folder + name pattern
    PickFolder       // pure folder pick
};

enum class ExplorerViewMode : uint8_t {
    Details = 0,   // list: name | type | date | size
    SmallIcons,
    MediumIcons,
    LargeIcons
};

enum class ExplorerSortBy : uint8_t {
    Name = 0,
    Date,
    Size,
    Type
};

struct ImportRemapEntry {
    texset::Chan srcChannel = texset::Chan::R;
    texset::ChannelRole dstRole = texset::ChannelRole::None;
    bool enabled = true;
};

// One file in a multi-import batch with assigned map kind
struct ImportBatchItem {
    std::string path;
    texset::MapKind kind = texset::MapKind::Diffuse;
};

struct FileExplorerState {
    bool open = false;
    FileExplorerMode mode = FileExplorerMode::Browse;

    std::string currentDir;
    std::string selectedPath;           // primary selection (file or folder)
    std::vector<std::string> multiSelect; // full paths (import multi)
    std::vector<ImportBatchItem> importBatch; // map assignment per file

    // Navigation
    std::vector<std::string> backStack;
    std::vector<std::string> forwardStack;

    // View / sort
    ExplorerViewMode viewMode = ExplorerViewMode::MediumIcons;
    ExplorerSortBy sortBy = ExplorerSortBy::Name;
    bool sortAsc = true;
    bool showHidden = false;

    // ---- ProjectCreate ----
    int projectType = 1;            // 0 Simple, 1 Advanced, 2 AdvancedMod
    int templateIdx = 1;            // default ZZZ for Advanced
    int canvasW = 1024;
    int canvasH = 1024;
    char projectName[128] = "New Project";
    char baseDiffusePath[512] = "";
    char importFolder[512] = "";
    bool autoPullSiblingMaps = true;

    // ---- ImportTexture ----
    texset::MapKind importMapKind = texset::MapKind::Diffuse;
    bool importExtractSolo = false;
    texset::ChannelRole importSoloRole = texset::ChannelRole::None;
    bool importRemapMode = false;
    bool importMultiSelect = true; // batch import enabled by default in Import mode
    ImportRemapEntry remap[4] = {
        { texset::Chan::R, texset::ChannelRole::ShadowRamp, true },
        { texset::Chan::G, texset::ChannelRole::Metallic, true },
        { texset::Chan::B, texset::ChannelRole::Glossiness, true },
        { texset::Chan::A, texset::ChannelRole::None, false },
    };
    texset::MapKind remapDestMap = texset::MapKind::LightMap;

    // ---- ExportTemplate / PickFolder ----
    char exportRoot[512] = "";
    char namePattern[128] = "{set}{suffix}";
    bool exportAllSets = true;
    bool exportAndRun = false; // Apply paths + QuickExportAllMaps

    std::string status;
};

void FileExplorerOpen(FileExplorerState& st, FileExplorerMode mode, const std::string& startDir = {});
void FileExplorerClose(FileExplorerState& st);

// Draws floating non-modal explorer. Returns true if an action was confirmed this frame.
bool DrawFileExplorer(FileExplorerState& st, Project* project, Canvas& canvas,
                      ID3D11Device* device);

bool FileExplorerApplyProjectCreate(FileExplorerState& st, ID3D11Device* device);
bool FileExplorerApplyImport(FileExplorerState& st, Project* project, Canvas& canvas,
                             ID3D11Device* device);
bool FileExplorerApplyExportTemplate(FileExplorerState& st, Project* project);

// Guess MapKind from filename (LightMap, NormalMap, …)
texset::MapKind GuessMapKindFromFilename(const std::string& filename);

} // namespace UI
