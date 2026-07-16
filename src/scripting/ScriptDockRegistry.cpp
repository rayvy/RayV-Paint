#include "ScriptDockRegistry.h"
#include "../ui/widgets/UiPanel.h"
#include <imgui.h>
#include <algorithm>

namespace script {

ScriptDockRegistry& ScriptDockRegistry::Get() {
    static ScriptDockRegistry s;
    return s;
}

ScriptDockInfo* ScriptDockRegistry::FindMut(const std::string& id) {
    for (auto& d : m_Docks)
        if (d.id == id) return &d;
    return nullptr;
}

const ScriptDockInfo* ScriptDockRegistry::Find(const std::string& id) const {
    for (const auto& d : m_Docks)
        if (d.id == id) return &d;
    return nullptr;
}

bool ScriptDockRegistry::Register(const std::string& id, const std::string& title, bool defaultOpen) {
    if (id.empty()) return false;
    if (auto* e = FindMut(id)) {
        if (!title.empty()) e->title = title;
        return true;
    }
    ScriptDockInfo d;
    d.id = id;
    d.title = title.empty() ? id : title;
    d.defaultOpen = defaultOpen;
    d.open = defaultOpen;
    m_Docks.push_back(std::move(d));
    return true;
}

void ScriptDockRegistry::Unregister(const std::string& id) {
    m_Docks.erase(std::remove_if(m_Docks.begin(), m_Docks.end(),
        [&](const ScriptDockInfo& d) { return d.id == id; }), m_Docks.end());
}

void ScriptDockRegistry::Clear() { m_Docks.clear(); }

bool ScriptDockRegistry::SetOpen(const std::string& id, bool open) {
    auto* e = FindMut(id);
    if (!e) return false;
    e->open = open;
    return true;
}

bool ScriptDockRegistry::IsOpen(const std::string& id) const {
    const auto* e = Find(id);
    return e && e->open;
}

bool ScriptDockRegistry::Exists(const std::string& id) const {
    return Find(id) != nullptr;
}

bool ScriptDockRegistry::Begin(const std::string& id, bool* outVisible) {
    auto* e = FindMut(id);
    if (!e) {
        if (outVisible) *outVisible = false;
        return false;
    }
    if (!e->open) {
        if (outVisible) *outVisible = false;
        return false;
    }
    // Stable docking name: Title###id
    std::string winName = e->title + "###script_dock_" + e->id;
    bool open = e->open;
    bool vis = Ui::BeginDockPanel(winName.c_str(), &open);
    e->open = open;
    if (!open || !vis) {
        // ImGui requires End() after Begin even when not visible.
        Ui::EndDockPanel();
        if (outVisible) *outVisible = false;
        return false;
    }
    if (outVisible) *outVisible = true;
    return true;
}

void ScriptDockRegistry::End() {
    Ui::EndDockPanel();
}

std::vector<ScriptDockInfo> ScriptDockRegistry::List() const {
    return m_Docks;
}

void ScriptDockRegistry::DrawViewMenuItems() {
    if (m_Docks.empty()) return;
    ImGui::Separator();
    ImGui::TextDisabled("Plugin docks");
    for (auto& d : m_Docks) {
        ImGui::MenuItem(d.title.c_str(), nullptr, &d.open);
    }
}

} // namespace script
