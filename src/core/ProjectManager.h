#pragma once

#include "../Canvas.h"

#include <d3d11.h>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <stdexcept>

// One open editor document (Photoshop-style tab).
// AdvancedModMode multi-map docs live inside a single Project later.
struct Project {
    int id = 0;
    std::unique_ptr<Canvas> canvas;
    int untitledIndex = 0; // >0 → "Untitled-N" when path empty

    std::string GetTabTitle() const;
    bool IsBlank() const; // no path, not modified, no useful content
};

// Owns all open projects in the single app process (one D3D11 device).
class ProjectManager {
public:
    static ProjectManager& Get();

    bool Initialize(ID3D11Device* device);
    void Shutdown();

    // Create empty project and make it active. Returns project id, or -1.
    int CreateEmptyProject();

    // Open path as a new project tab (or reuse blank active if still empty).
    // Returns project id that will receive the document. Does not load pixels —
    // caller should OpenDocument / TriggerBackgroundOpenDocument on ActiveCanvas
    // after SwitchTo(id) (id is already active).
    int PrepareOpenAsNewProject(const std::string& filepath);

    // If a project already has this path open, switch to it and return its id.
    // Otherwise PrepareOpenAsNewProject. Returns id to load into (or -1).
    int ActivateOrPrepareOpen(const std::string& filepath);

    bool SwitchTo(int id);
    // force=true skips dirty check. Returns false if dirty and !force.
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

    // IPC / second-instance: queue paths for the main loop.
    void EnqueueOpenPath(const std::string& path);
    // Drain queue: for each path, prepare project + call openFn(path, canvas).
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
