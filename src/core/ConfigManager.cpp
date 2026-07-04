#include "ConfigManager.h"
#include "Logger.h"
#include <nlohmann/json.hpp>
#include <fstream>

using json = nlohmann::json;

ConfigManager& ConfigManager::Get() {
    static ConfigManager instance;
    return instance;
}

bool ConfigManager::Load(const std::string& configFilePath) {
    std::unique_lock<std::mutex> lock(m_Mutex);
    
    std::ifstream file(configFilePath);
    if (!file.is_open()) {
        Logger::Get().Warn("Could not open config file: " + configFilePath + ". Creating default config.");
        // We will save default settings immediately
        lock.unlock(); // Release lock before calling Save to avoid deadlock
        Save(configFilePath);
        return true;
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
        if (j.contains("max_undo_steps")) m_MaxUndoSteps = j["max_undo_steps"].get<int>();
        if (j.contains("max_undo_memory_mb")) m_MaxUndoMemoryMB = j["max_undo_memory_mb"].get<int>();

        Logger::Get().Info("Configuration loaded successfully from " + configFilePath);
        return true;
    }
    catch (const std::exception& e) {
        Logger::Get().Error("Error parsing config file: " + std::string(e.what()));
        return false;
    }
}

bool ConfigManager::Save(const std::string& configFilePath) {
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
        j["max_undo_steps"] = m_MaxUndoSteps;
        j["max_undo_memory_mb"] = m_MaxUndoMemoryMB;

        std::ofstream file(configFilePath);
        if (!file.is_open()) {
            Logger::Get().Error("Failed to open config file for writing: " + configFilePath);
            return false;
        }

        file << j.dump(4);
        Logger::Get().Info("Configuration saved successfully to " + configFilePath);
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
