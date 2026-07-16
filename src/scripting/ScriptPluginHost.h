#pragma once
// Loads Python plugins from builtin + user script folders.
// Each .py may define on_load() / on_ui() / on_menu_open(id).
// UI is drawn only in Python via rayv.ui.* (ImGui wrappers).

#include <string>
#include <vector>

namespace script {

struct PluginInfo {
    std::string id;       // module stem
    std::string path;
    std::string title;    // from PLUGIN["name"] or stem
    std::string source;   // "builtin" | "user"
    bool hasUi = false;
    bool hasMenu = false;
};

class ScriptPluginHost {
public:
    static ScriptPluginHost& Get();

    // {exe}/scripts  and  Documents/RayVPaint/scripts
    static std::string BuiltinScriptsDir();
    static std::string UserScriptsDir();

    // Rescan + re-import modules. Safe to call from UI "Refresh".
    bool Reload(std::string* outSummary = nullptr);

    // ImGui frame: call on_ui() for every loaded plugin.
    void DrawAllUi();

    // Open plugin UI (sets rayv.plugins._request_open id).
    void RequestOpen(const std::string& pluginId);

    const std::vector<PluginInfo>& List() const { return m_Plugins; }

    bool Loaded() const { return m_Loaded; }

    // After a hard fault in plugin UI, stop calling on_ui for the rest of the session.
    void DisableUi() { m_UiDisabled = true; }
    bool UiDisabled() const { return m_UiDisabled; }

private:
    ScriptPluginHost() = default;
    std::vector<PluginInfo> m_Plugins;
    bool m_Loaded = false;
    bool m_UiDisabled = false;
};

} // namespace script
