#include "ConfigManager.h"
#include "Logger.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <windows.h>
#include <shlobj.h>
#include <filesystem>
#include <cstdlib>

using json = nlohmann::json;

ConfigManager& ConfigManager::Get() {
    static ConfigManager instance;
    return instance;
}

std::string ConfigManager::GetUserDirectory() {
    wchar_t path[MAX_PATH];
    std::filesystem::path rayvPath;
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_MYDOCUMENTS, NULL, 0, path))) {
        rayvPath = std::filesystem::path(path) / "RayVPaint";
    } else {
        const char* userProfile = std::getenv("USERPROFILE");
        if (userProfile) {
            rayvPath = std::filesystem::path(userProfile) / "Documents" / "RayVPaint";
        } else {
            rayvPath = std::filesystem::current_path() / "RayVPaint";
        }
    }
    return rayvPath.string();
}

std::string ConfigManager::GetUserSubdirectory(const std::string& sub) {
    std::filesystem::path p = std::filesystem::path(GetUserDirectory()) / sub;
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return p.string();
}

// Seed user config from shipped defaults (next to exe or source tree).
static bool SeedFromShippedDefaults(const std::string& userPath) {
    std::vector<std::string> candidates = {
        "defaults/config.json",
        "bin/defaults/config.json",
        "../defaults/config.json",
        "src/resources/defaults/config.json",
        "../src/resources/defaults/config.json",
        "../../src/resources/defaults/config.json",
    };
#ifdef _WIN32
    wchar_t exeBuf[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exeBuf, MAX_PATH)) {
        std::filesystem::path exeDir = std::filesystem::path(exeBuf).parent_path();
        candidates.insert(candidates.begin(), (exeDir / "defaults" / "config.json").string());
        candidates.insert(candidates.begin(), (exeDir / ".." / "defaults" / "config.json").string());
    }
#endif
    for (const auto& c : candidates) {
        std::error_code ec;
        if (!std::filesystem::exists(std::filesystem::u8path(c), ec)) continue;
        try {
            std::filesystem::create_directories(std::filesystem::path(userPath).parent_path(), ec);
            std::filesystem::copy_file(c, userPath, std::filesystem::copy_options::overwrite_existing, ec);
            if (!ec) {
                Logger::Get().Info("Seeded default config from " + c + " → " + userPath);
                return true;
            }
        } catch (...) {}
    }
    return false;
}

bool ConfigManager::Load(const std::string& configFilePath) {
    std::string path = configFilePath;
    if (path.empty()) {
        path = GetUserSubdirectory("user") + "/config.json";
    }

    {
        std::ifstream probe(path);
        if (!probe.is_open()) {
            Logger::Get().Warn("Could not open config file: " + path + ". Seeding shipped defaults.");
            if (!SeedFromShippedDefaults(path)) {
                Save(path); // code defaults as last resort
            }
        }
    }

    std::unique_lock<std::mutex> lock(m_Mutex);
    std::ifstream file(path);
    if (!file.is_open()) {
        Logger::Get().Error("Config still missing after seed: " + path);
        return false;
    }

    try {
        json j;
        file >> j;

        if (j.contains("default_width"))  m_DefaultWidth = j["default_width"].get<int>();
        if (j.contains("default_height")) m_DefaultHeight = j["default_height"].get<int>();
        if (j.contains("log_level"))      m_LogLevel = j["log_level"].get<std::string>();
        if (j.contains("zoom_speed"))     m_ZoomSpeed = j["zoom_speed"].get<float>();
        if (j.contains("log_file_path"))  m_LogFilePath = j["log_file_path"].get<std::string>();
        if (j.contains("theme"))          m_Theme = j["theme"].get<std::string>();
        if (j.contains("backup_dir"))     m_BackupDir = j["backup_dir"].get<std::string>();
        if (j.contains("autosave_interval_minutes")) m_AutoSaveIntervalMinutes = j["autosave_interval_minutes"].get<int>();
        if (j.contains("autosave_max_per_project")) m_AutosaveMaxPerProject = j["autosave_max_per_project"].get<int>();
        if (j.contains("max_undo_steps")) m_MaxUndoSteps = j["max_undo_steps"].get<int>();
        if (j.contains("max_undo_memory_mb")) m_MaxUndoMemoryMB = j["max_undo_memory_mb"].get<int>();
        if (j.contains("max_brush_radius")) m_MaxBrushRadius = j["max_brush_radius"].get<float>();

        Logger::Get().Info("Configuration loaded successfully from " + path);
        return true;
    }
    catch (const std::exception& e) {
        Logger::Get().Error("Error parsing config file: " + std::string(e.what()));
        return false;
    }
}

bool ConfigManager::Save(const std::string& configFilePath) {
    std::string path = configFilePath;
    if (path.empty()) {
        path = GetUserSubdirectory("user") + "/config.json";
    }

    std::lock_guard<std::mutex> lock(m_Mutex);

    try {
        json j;
        j["default_width"]  = m_DefaultWidth;
        j["default_height"] = m_DefaultHeight;
        j["log_level"]      = m_LogLevel;
        j["zoom_speed"]     = m_ZoomSpeed;
        j["log_file_path"]  = m_LogFilePath;
        j["theme"]          = m_Theme;
        j["backup_dir"]     = m_BackupDir;
        j["autosave_interval_minutes"] = m_AutoSaveIntervalMinutes;
        j["autosave_max_per_project"] = m_AutosaveMaxPerProject;
        j["max_undo_steps"] = m_MaxUndoSteps;
        j["max_undo_memory_mb"] = m_MaxUndoMemoryMB;
        j["max_brush_radius"] = m_MaxBrushRadius;

        std::ofstream file(path);
        if (!file.is_open()) {
            Logger::Get().Error("Failed to open config file for writing: " + path);
            return false;
        }

        file << j.dump(4);
        Logger::Get().Info("Configuration saved successfully to " + path);
        return true;
    }
    catch (const std::exception& e) {
        Logger::Get().Error("Error writing config file: " + std::string(e.what()));
        return false;
    }
}

int ConfigManager::GetDefaultWidth() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_DefaultWidth;
}

void ConfigManager::SetDefaultWidth(int width) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_DefaultWidth = width;
}

int ConfigManager::GetDefaultHeight() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_DefaultHeight;
}

void ConfigManager::SetDefaultHeight(int height) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_DefaultHeight = height;
}

std::string ConfigManager::GetLogLevel() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_LogLevel;
}

void ConfigManager::SetLogLevel(const std::string& level) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_LogLevel = level;
}

float ConfigManager::GetZoomSpeed() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_ZoomSpeed;
}

void ConfigManager::SetZoomSpeed(float speed) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_ZoomSpeed = speed;
}

std::string ConfigManager::GetLogFilePath() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_LogFilePath == "rayv_paint.log" || m_LogFilePath.empty()) {
        return GetUserSubdirectory("user") + "/rayv_paint.log";
    }
    return m_LogFilePath;
}

void ConfigManager::SetLogFilePath(const std::string& path) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_LogFilePath = path;
}

std::string ConfigManager::GetTheme() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Theme;
}

void ConfigManager::SetTheme(const std::string& theme) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Theme = theme;
}

std::string ConfigManager::GetBackupDir() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_BackupDir == "backups" || m_BackupDir.empty()) {
        return GetUserSubdirectory("autosaves");
    }
    return m_BackupDir;
}

void ConfigManager::SetBackupDir(const std::string& path) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_BackupDir = path;
}

int ConfigManager::GetAutoSaveIntervalMinutes() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_AutoSaveIntervalMinutes;
}

void ConfigManager::SetAutoSaveIntervalMinutes(int minutes) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_AutoSaveIntervalMinutes = minutes;
}

int ConfigManager::GetAutosaveMaxPerProject() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_AutosaveMaxPerProject;
}

void ConfigManager::SetAutosaveMaxPerProject(int n) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_AutosaveMaxPerProject = std::max(1, n);
}

int ConfigManager::GetMaxUndoSteps() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_MaxUndoSteps;
}

void ConfigManager::SetMaxUndoSteps(int steps) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_MaxUndoSteps = steps;
}

int ConfigManager::GetMaxUndoMemoryMB() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_MaxUndoMemoryMB;
}

void ConfigManager::SetMaxUndoMemoryMB(int mb) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_MaxUndoMemoryMB = mb;
}

float ConfigManager::GetMaxBrushRadius() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_MaxBrushRadius;
}

void ConfigManager::SetMaxBrushRadius(float radiusPx) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (radiusPx < 1.f) radiusPx = 1.f;
    if (radiusPx > 2000.f) radiusPx = 2000.f;
    m_MaxBrushRadius = radiusPx;
}
