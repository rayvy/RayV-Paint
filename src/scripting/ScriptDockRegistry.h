#pragma once
// Python-registered ImGui docks: stable id, open flag, View menu, dockspace integration.

#include <string>
#include <vector>

namespace script {

struct ScriptDockInfo {
    std::string id;
    std::string title;
    bool open = true;
    bool defaultOpen = true;
};

class ScriptDockRegistry {
public:
    static ScriptDockRegistry& Get();

    // Register (or update title). Returns false if id empty.
    bool Register(const std::string& id, const std::string& title, bool defaultOpen = true);
    void Unregister(const std::string& id);
    void Clear();

    bool SetOpen(const std::string& id, bool open);
    bool IsOpen(const std::string& id) const;
    bool Exists(const std::string& id) const;

    // begin: ImGui::Begin with dockable chrome. Returns (visible, is_open).
    // Must pair with End() when visible.
    // Uses title "##id" pattern for stable docking id.
    bool Begin(const std::string& id, bool* outVisible);
    void End();

    std::vector<ScriptDockInfo> List() const;

    // Draw View menu items for registered docks.
    void DrawViewMenuItems();

private:
    ScriptDockRegistry() = default;
    std::vector<ScriptDockInfo> m_Docks;
    ScriptDockInfo* FindMut(const std::string& id);
    const ScriptDockInfo* Find(const std::string& id) const;
};

} // namespace script
