#pragma once
// Context-aware File Explorer — bookmarks, drives, icon views, previews, multi-import, folder export.

#include "../texset/TextureSetTypes.h"
#include <d3d11.h>
#include <cstdint>
#include <string>
#include <vector>

class Canvas;
struct Project;

namespace UI {

enum class FileExplorerMode : uint8_t {
    Browse = 0,
    ProjectCreate,   // new project wizard + base texture + auto sibling maps
    ImportTexture,   // single / multi map import + optional remap
    ExportTemplate,  // pick export folder + name pattern
    PickFolder,      // pure folder pick
    SaveProject,     // save .rayp
    OpenProject,     // open .rayp
    LoadConfig,      // load config json
    SaveConfig,      // save config json
    AdvancedExport   // single-file export with format/mips/ICC in side panel
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

// Remap: each source channel R/G/B/A routes to a dest map + dest channel
struct RemapRoute {
    bool enabled = false;
    texset::MapKind destMap = texset::MapKind::Diffuse;
    int destChan = 0; // 0=R 1=G 2=B 3=A
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
    std::string selectedPath;
    std::vector<std::string> multiSelect;
    std::vector<ImportBatchItem> importBatch;

    std::vector<std::string> backStack;
    std::vector<std::string> forwardStack;

    // Layout (resizable, persisted in session)
    float bookmarkW = 168.f;
    float sideFormW = 300.f;

    ExplorerViewMode viewMode = ExplorerViewMode::MediumIcons;
    ExplorerSortBy sortBy = ExplorerSortBy::Name;
    bool sortAsc = true;
    bool showHidden = false;

    // ---- ProjectCreate ----
    int projectType = 1;
    int templateIdx = 1;
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
    bool importMultiSelect = true;
    // remapRoutes[srcChan 0..3]: where that channel goes (multi-map allowed)
    RemapRoute remapRoutes[4] = {
        { true,  texset::MapKind::LightMap,    0 }, // R → LightMap.R
        { true,  texset::MapKind::LightMap,    1 }, // G → LightMap.G
        { true,  texset::MapKind::LightMap,    2 }, // B → LightMap.B
        { false, texset::MapKind::LightMap,    3 }, // A off
    };

    // ---- ExportTemplate / PickFolder ----
    char exportRoot[512] = "";
    char namePattern[128] = "{set}{suffix}";
    bool exportAllSets = true;
    bool exportAndRun = false;

    // ---- Save/Open path fields ----
    char saveFileName[256] = "project.rayp";
    char filterExt[32] = ".rayp"; // filter for save/open modes

    std::string status;

    // Directory listing: dirty → async worker; main thread never blocks on index.
    // Agents / benchmarks: lastListMs is worker-side cost (not UI freeze).
    bool dirCacheDirty = true;
    bool dirListingBusy = false;   // true while worker indexes current folder
    double lastListMs = 0.0;
    int lastListCount = 0;
    int thumbsPending = 0;         // in-flight async thumb decodes
};

void FileExplorerOpen(FileExplorerState& st, FileExplorerMode mode, const std::string& startDir = {});
void FileExplorerClose(FileExplorerState& st);

// True while FE is indexing a folder or decoding thumbs on worker threads.
// UI stays interactive; use for footer spinner ("app is not dead").
bool FileExplorerIsBusy();

bool DrawFileExplorer(FileExplorerState& st, Project* project, Canvas& canvas,
                      ID3D11Device* device);

bool FileExplorerApplyProjectCreate(FileExplorerState& st, ID3D11Device* device);
bool FileExplorerApplyImport(FileExplorerState& st, Project* project, Canvas& canvas,
                             ID3D11Device* device);
bool FileExplorerApplyExportTemplate(FileExplorerState& st, Project* project);
bool FileExplorerApplySaveProject(FileExplorerState& st, Project* project, Canvas& canvas);
bool FileExplorerApplyOpenProject(FileExplorerState& st, ID3D11Device* device);
bool FileExplorerApplyLoadConfig(FileExplorerState& st);
bool FileExplorerApplySaveConfig(FileExplorerState& st);
bool FileExplorerApplyAdvancedExport(FileExplorerState& st, Canvas& canvas);

texset::MapKind GuessMapKindFromFilename(const std::string& filename);

} // namespace UI
