#include "ProjectManager.h"
#include "Logger.h"
#include "PathUtil.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>

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
    // No layers or empty canvas still counts as blank starter tab.
    if (canvas->GetWidth() <= 0 || canvas->GetHeight() <= 0) return true;
    if (canvas->GetLayers().empty()) return true;
    return false;
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

    const int id = proj->id;
    m_Projects.push_back(std::move(proj));
    m_ActiveId = id;
    Logger::Get().Info("Project created id=" + std::to_string(id));
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

bool ProjectManager::SwitchTo(int id) {
    if (!FindMutable(id)) return false;
    if (m_ActiveId == id) return true;
    m_ActiveId = id;
    Logger::Get().Debug("Switched to project id=" + std::to_string(id));
    return true;
}

bool ProjectManager::CloseProject(int id, bool force) {
    Project* p = FindMutable(id);
    if (!p) return false;

    if (!force && p->canvas && p->canvas->IsDocumentModified()) {
        return false; // UI must confirm
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
        // Prefer neighbor tab
        const int newIdx = std::min(idx, (int)m_Projects.size() - 1);
        m_ActiveId = m_Projects[newIdx]->id;
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
        t.dirty = p->canvas && p->canvas->IsDocumentModified();
        t.active = (p->id == m_ActiveId);
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
