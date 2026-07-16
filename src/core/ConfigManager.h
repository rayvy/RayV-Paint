#pragma once
#include <string>
#include <mutex>

class ConfigManager {
public:
    static ConfigManager& Get();
    static std::string GetUserDirectory();
    static std::string GetUserSubdirectory(const std::string& sub);

    bool Load(const std::string& configFilePath = "");
    bool Save(const std::string& configFilePath = "");

    // Getters & Setters (thread-safe)
    int GetDefaultWidth() const;
    void SetDefaultWidth(int width);

    int GetDefaultHeight() const;
    void SetDefaultHeight(int height);

    std::string GetLogLevel() const;
    void SetLogLevel(const std::string& level);

    float GetZoomSpeed() const;
    void SetZoomSpeed(float speed);

    std::string GetLogFilePath() const;
    void SetLogFilePath(const std::string& path);

    std::string GetTheme() const;
    void SetTheme(const std::string& theme);

    std::string GetBackupDir() const;
    void SetBackupDir(const std::string& path);

    int GetAutoSaveIntervalMinutes() const;
    void SetAutoSaveIntervalMinutes(int minutes);

    // Max autosave files kept per project BASE (UNTITLED / stem). Default 5.
    int GetAutosaveMaxPerProject() const;
    void SetAutosaveMaxPerProject(int n);

    int GetMaxUndoSteps() const;
    void SetMaxUndoSteps(int steps);

    int GetMaxUndoMemoryMB() const;
    void SetMaxUndoMemoryMB(int mb);

    // Max brush radius in canvas pixels (Ctrl+Alt+RMB range / clamp).
    float GetMaxBrushRadius() const;
    void SetMaxBrushRadius(float radiusPx);

private:
    ConfigManager() = default;
    ~ConfigManager() = default;
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    mutable std::mutex m_Mutex;
    
    // Configurations
    int m_DefaultWidth = 1024;
    int m_DefaultHeight = 1024;
    std::string m_LogLevel = "info";
    float m_ZoomSpeed = 1.15f;
    std::string m_LogFilePath = "rayv_paint.log";

    std::string m_Theme = "Dark";
    std::string m_BackupDir = "backups";
    int m_AutoSaveIntervalMinutes = 3;   // default: every 3 minutes
    int m_AutosaveMaxPerProject = 5;     // rotation limit per project BASE
    int m_MaxUndoSteps = 50;
    int m_MaxUndoMemoryMB = 512;
    float m_MaxBrushRadius = 250.0f;
};
