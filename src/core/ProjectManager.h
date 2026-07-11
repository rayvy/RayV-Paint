#pragma once

#include "../Canvas.h"
#include "../texset/TextureSet.h"

#include <d3d11.h>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <stdexcept>

// One open editor document (Photoshop-style tab).
// Advanced / AdvancedMod: textureSets hold multi-map workspace; canvas is active view.
struct Project {
    int id = 0;
    std::unique_ptr<Canvas> canvas;
    int untitledIndex = 0; // >0 → "Untitled-N" when path empty

    // Cross-texture workspace (Simple: 1 Diffuse set; Advanced+: N sets)
    texset::TextureSetLibrary textureSets;

    std::string GetTabTitle() const;
    bool IsBlank() const; // no path, not modified, no useful content

    // Sync Diffuse map size/path from canvas (call after open/resize)
    void SyncTextureSetsFromCanvas();

    // Push / pull texture set meta through Canvas for .rayp I/O
    void InjectTextureSetsIntoCanvas();
    void ApplyTextureSetsFromCanvas();

    // Advanced: add another set (no path dedup)
    int AddTextureSet(const std::string& name, const std::string& templateId = "Default");

    // Import a file into active set as MapKind (optional solo channel role extract)
    bool ImportMapFile(texset::MapKind kind, const std::string& filepath,
                       texset::ChannelRole soloRole = texset::ChannelRole::None);

    // Apply built-in template (Default / ZZZ / GI) to active set
    bool ApplyActiveSetTemplate(const std::string& templateId);

    // Ctrl+E multi-map: export all enabled maps of active set
    // Diffuse uses current canvas composite (RGBA8); other maps use imported composites.
    int QuickExportAllMaps(const std::string& baseDirHint = {});

    // Create / reconfigure this project as Advanced multi-map from a base Diffuse texture.
    // Finds sibling LightMap / MaterialMap / NormalMap in the same folder by stem+suffix.
    // Returns number of maps successfully loaded (Diffuse counts as 1).
    int SetupAdvancedFromBaseTexture(
        ID3D11Device* device,
        const std::string& baseDiffusePath,
        const std::string& templateId = "ZZZ",
        const std::string& setName = {});
};

// Owns all open projects in the single app process (one D3D11 device).
class ProjectManager {
public:
    static ProjectManager& Get();

    bool Initialize(ID3D11Device* device);
    void Shutdown();

    int CreateEmptyProject();
    int PrepareOpenAsNewProject(const std::string& filepath);
    int ActivateOrPrepareOpen(const std::string& filepath);

    bool SwitchTo(int id);
    bool CloseProject(int id, bool force = false);

    Canvas& ActiveCanvas();
    Canvas* ActiveCanvasPtr();
    const Canvas& ActiveCanvas() const;

    int ActiveProjectId() const { return m_ActiveId; }
    size_t ProjectCount() const { return m_Projects.size(); }
    bool HasProjects() const { return !m_Projects.empty(); }

    Project* FindProject(int id);
    const Project* FindProject(int id) const;
    Project* ActiveProject();
    const Project* ActiveProject() const;

    struct ProjectTabInfo {
        int id = 0;
        std::string title;
        bool dirty = false;
        bool active = false;
    };
    std::vector<ProjectTabInfo> ListTabs() const;

    void EnqueueOpenPath(const std::string& path);
    void DrainPendingOpens(const std::function<void(const std::string& path, Canvas& canvas)>& openFn);

    ID3D11Device* GetDevice() const { return m_Device; }

private:
    ProjectManager() = default;

    ID3D11Device* m_Device = nullptr;
    std::vector<std::unique_ptr<Project>> m_Projects;
    int m_ActiveId = -1;
    int m_NextId = 1;
    int m_NextUntitled = 1;

    std::mutex m_PendingMutex;
    std::deque<std::string> m_PendingPaths;

    Project* FindMutable(int id);
    int IndexOf(int id) const;
};
