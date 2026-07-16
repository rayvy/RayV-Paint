#pragma once
#include <string>
#include <mutex>  // recursive_mutex

class ScriptingEngine {
public:
    static ScriptingEngine& Get();

    bool Initialize();
    void Shutdown();
    bool IsInitialized() const { return m_Initialized; }

    // Run Python code from string
    bool RunString(const std::string& code);

    // Run Python script from file
    bool RunScript(const std::string& filepath);

    // Plugin host helpers (builtin + Documents/RayVPaint/scripts)
    bool ReloadPlugins(std::string* outSummary = nullptr);
    void DrawPluginsUi(); // call once per ImGui frame from UI

private:
    ScriptingEngine() = default;
    ~ScriptingEngine();
    ScriptingEngine(const ScriptingEngine&) = delete;
    ScriptingEngine& operator=(const ScriptingEngine&) = delete;

    // recursive: plugin on_ui may call rayv.plugins.reload / nested RunString
    mutable std::recursive_mutex m_Mutex;
    bool m_Initialized = false;
};
