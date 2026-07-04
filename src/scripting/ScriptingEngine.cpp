#include "ScriptingEngine.h"
#include "Logger.h"
#include "ConfigManager.h"
#include <pybind11/embed.h>
#include <fstream>
#include <iostream>
#include <sstream>

namespace py = pybind11;

// Forward declarations for App/Canvas interactions
extern void TriggerCanvasResize(int w, int h);
extern float GetCanvasZoom();
extern void SetCanvasZoom(float zoom);
extern void SetCanvasPan(float x, float y);
extern void ResetCanvasView();
extern bool LoadCanvasImage(const std::string& filepath);
extern bool SaveCanvasDDS(const std::string& filepath, int formatChoice);
extern bool SaveCanvasStandard(const std::string& filepath, const std::string& iccProfilePath);

// Define embedded module
PYBIND11_EMBEDDED_MODULE(rayv, m) {
    m.doc() = "RayVPaint Python Scripting API";

    // Logging Bindings
    m.def("log_debug", [](const std::string& msg) { Logger::Get().Debug("[Python] " + msg); });
    m.def("log_info",  [](const std::string& msg) { Logger::Get().Info("[Python] " + msg); });
    m.def("log_warn",  [](const std::string& msg) { Logger::Get().Warn("[Python] " + msg); });
    m.def("log_error", [](const std::string& msg) { Logger::Get().Error("[Python] " + msg); });

    // Config Manager Bindings
    m.def("get_default_width",  []() { return ConfigManager::Get().GetDefaultWidth(); });
    m.def("get_default_height", []() { return ConfigManager::Get().GetDefaultHeight(); });
    m.def("set_default_width",  [](int w) { ConfigManager::Get().SetDefaultWidth(w); });
    m.def("set_default_height", [](int h) { ConfigManager::Get().SetDefaultHeight(h); });
    m.def("save_config",        []() { return ConfigManager::Get().Save(); });

    // Canvas / Viewer Bindings
    m.def("resize_canvas",      [](int w, int h) { TriggerCanvasResize(w, h); });
    m.def("get_zoom",           []() { return GetCanvasZoom(); });
    m.def("set_zoom",           [](float z) { SetCanvasZoom(z); });
    m.def("set_pan",            [](float x, float y) { SetCanvasPan(x, y); });
    m.def("reset_view",         []() { ResetCanvasView(); });
    m.def("load_image",         [](const std::string& path) { return LoadCanvasImage(path); });
    m.def("save_dds",           [](const std::string& path, int fmt) { return SaveCanvasDDS(path, fmt); });
    m.def("save_image",         [](const std::string& path, const std::string& iccPath) { return SaveCanvasStandard(path, iccPath); }, py::arg("path"), py::arg("icc_path") = "");
}

ScriptingEngine& ScriptingEngine::Get() {
    static ScriptingEngine instance;
    return instance;
}

ScriptingEngine::~ScriptingEngine() {
    Shutdown();
}

bool ScriptingEngine::Initialize() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Initialized) return true;

    try {
        Logger::Get().Info("Initializing embedded Python interpreter...");
        py::initialize_interpreter();
        
        // Print python version
        py::exec("import sys; import rayv; rayv.log_info(f'Python Interpreter Initialized. Version: {sys.version}')");
        m_Initialized = true;
        return true;
    }
    catch (const std::exception& e) {
        Logger::Get().Error("Failed to initialize Python scripting engine: " + std::string(e.what()));
        return false;
    }
}

void ScriptingEngine::Shutdown() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Initialized) return;

    try {
        Logger::Get().Info("Shutting down embedded Python interpreter...");
        py::finalize_interpreter();
        m_Initialized = false;
    }
    catch (const std::exception& e) {
        Logger::Get().Error("Error shutting down Python scripting engine: " + std::string(e.what()));
    }
}

bool ScriptingEngine::RunString(const std::string& code) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Initialized) {
        Logger::Get().Error("Scripting engine not initialized.");
        return false;
    }

    try {
        py::exec(code);
        return true;
    }
    catch (const std::exception& e) {
        Logger::Get().Error("Python execution error: " + std::string(e.what()));
        return false;
    }
}

bool ScriptingEngine::RunScript(const std::string& filepath) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Initialized) {
        Logger::Get().Error("Scripting engine not initialized.");
        return false;
    }

    std::ifstream file(filepath);
    if (!file.is_open()) {
        Logger::Get().Error("Failed to open Python script file: " + filepath);
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();

    try {
        Logger::Get().Info("Executing Python script: " + filepath);
        py::exec(buffer.str());
        return true;
    }
    catch (const std::exception& e) {
        Logger::Get().Error("Python execution error in script " + filepath + ": " + std::string(e.what()));
        return false;
    }
}
