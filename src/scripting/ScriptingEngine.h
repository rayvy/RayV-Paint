#pragma once
#include <string>
#include <mutex>

class ScriptingEngine {
public:
    static ScriptingEngine& Get();

    bool Initialize();
    void Shutdown();

    // Run Python code from string
    bool RunString(const std::string& code);
    
    // Run Python script from file
    bool RunScript(const std::string& filepath);

private:
    ScriptingEngine() = default;
    ~ScriptingEngine();
    ScriptingEngine(const ScriptingEngine&) = delete;
    ScriptingEngine& operator=(const ScriptingEngine&) = delete;

    std::mutex m_Mutex;
    bool m_Initialized = false;
};
